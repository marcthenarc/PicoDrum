#!/usr/bin/env python3
"""
Builds the sample library for the Pico 2 Sample Player.

Reads every .wav in a directory and produces a single binary blob to be
flashed at a fixed offset, with a TOC at the front that the firmware reads
through XIP. No sample ends up inside the firmware: the library is reflashed
without rebuilding, and build times stay constant as the library grows.

Blob format (little-endian, everything 4-byte aligned):

    off  0   char     magic[4]        'S','M','P','L'
    off  4   uint16   version         = 1
    off  6   uint16   count           number of samples
    off  8   uint32   sample_rate     Hz (the same for all of them)
    off 12   uint32   total_bytes     total size of the blob
    off 16   entry[count]             32 bytes each:
                 char   name[24]      NUL-padded
                 uint32 offset_bytes  from the start of the blob
                 uint32 length_samples
    then     the mono int16 PCM data, each sample 4-byte aligned

Typical use:

    tools/convert_wav.py samples/ -o sample_lib.bin

A directory is walked recursively and sorted by path, which makes the order of
the library an accident of the file names. When the order carries meaning —
a kit-organised blob, where entry k*6+s IS the sample for slot s of kit k —
pass an explicit ordered list instead, which is what tools/pick_kit.py writes:

    tools/convert_wav.py --list samples/kits.txt --kit-size 6 -o sample_lib.bin

Dependencies: none (stdlib only). If numpy is installed it is used
automatically for the resampling, which becomes ~100x faster on large
libraries; the result is identical up to rounding.
"""

import argparse
import array
import math
import os
import struct
import sys

try:
    import numpy as _np
except ImportError:
    _np = None

MAGIC = b"SMPL"

# Blob version.
#   1  a flat list of samples, in whatever order the converter walked them.
#   2  the entries are grouped in kits of KIT_SIZE, in slot order: entry
#      k * KIT_SIZE + s is the sample for slot s of kit k. That layout is what
#      lets the firmware load a whole kit with an index computation instead of
#      guessing roles from the file names.
VERSION_FLAT = 1
VERSION_KITS = 2
KIT_SIZE = 8  # must match NUM_SLOTS in the firmware
NAME_LEN = 24
ENTRY_SIZE = 32
HEADER_SIZE = 16

DEFAULT_RATE = 22050
DEFAULT_FLASH_OFFSET = 0x80000  # 512KB: firmware below, samples above
XIP_BASE = 0x10000000

# Level below which a sample counts as silence, in dBFS.
TRIM_DB = -60.0
# Guard left before the first and after the last audible sample, in ms.
TRIM_GUARD_MS = 2.0


# --------------------------------------------------------------------------
# WAV reading
# --------------------------------------------------------------------------
# The stdlib `wave` module rejects float WAVs and WAVE_FORMAT_EXTENSIBLE, which
# is what plenty of DAWs write by default. The RIFF parser below is minimal but
# covers the formats that actually turn up.

FMT_PCM = 0x0001
FMT_FLOAT = 0x0003
FMT_EXTENSIBLE = 0xFFFE


def read_wav(path):
    """Returns (mono_float_samples, sample_rate). Floats in [-1, 1]."""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")

    fmt_chunk = None
    raw = None
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = data[pos : pos + 4]
        chunk_size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8 : pos + 8 + chunk_size]
        if chunk_id == b"fmt ":
            fmt_chunk = body
        elif chunk_id == b"data":
            raw = body
        pos += 8 + chunk_size + (chunk_size & 1)  # chunks are word-aligned

    if fmt_chunk is None or raw is None:
        raise ValueError("missing 'fmt ' or 'data' chunk")
    if len(fmt_chunk) < 16:
        raise ValueError("truncated 'fmt ' chunk")

    audio_format, channels, rate, _byte_rate, _block_align, bits = struct.unpack_from(
        "<HHIIHH", fmt_chunk, 0
    )

    # In EXTENSIBLE the real format sits in the first 2 bytes of the subformat GUID.
    if audio_format == FMT_EXTENSIBLE:
        if len(fmt_chunk) < 26:
            raise ValueError("WAVE_FORMAT_EXTENSIBLE with no subformat")
        audio_format = struct.unpack_from("<H", fmt_chunk, 24)[0]

    if channels < 1:
        raise ValueError("zero channels")

    samples = _decode_pcm(raw, audio_format, bits)

    # Drop any partial trailing frame before de-interleaving.
    usable = (len(samples) // channels) * channels
    samples = samples[:usable]

    if channels > 1:
        samples = _mix_to_mono(samples, channels)

    return samples, rate


def _decode_pcm(raw, audio_format, bits):
    """Converts the raw bytes into a list of normalised floats."""
    if audio_format == FMT_PCM:
        if bits == 8:
            # 8-bit WAV samples are unsigned with an offset of 128.
            return [(b - 128) / 128.0 for b in raw]
        if bits == 16:
            a = array.array("h")
            a.frombytes(raw[: len(raw) - (len(raw) % 2)])
            if sys.byteorder == "big":
                a.byteswap()
            return [v / 32768.0 for v in a]
        if bits == 24:
            out = []
            for i in range(0, len(raw) - 2, 3):
                v = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
                if v >= 0x800000:
                    v -= 0x1000000
                out.append(v / 8388608.0)
            return out
        if bits == 32:
            a = array.array("i")
            a.frombytes(raw[: len(raw) - (len(raw) % 4)])
            if sys.byteorder == "big":
                a.byteswap()
            return [v / 2147483648.0 for v in a]
        raise ValueError("%d bit PCM not supported" % bits)

    if audio_format == FMT_FLOAT:
        code = "f" if bits == 32 else "d" if bits == 64 else None
        if code is None:
            raise ValueError("%d bit float not supported" % bits)
        width = bits // 8
        a = array.array(code)
        a.frombytes(raw[: len(raw) - (len(raw) % width)])
        if sys.byteorder == "big":
            a.byteswap()
        return list(a)

    raise ValueError("audio format 0x%04X not supported" % audio_format)


def _mix_to_mono(samples, channels):
    if _np is not None:
        arr = _np.asarray(samples, dtype=_np.float64).reshape(-1, channels)
        return arr.mean(axis=1).tolist()
    inv = 1.0 / channels
    return [sum(samples[i : i + channels]) * inv for i in range(0, len(samples), channels)]


# --------------------------------------------------------------------------
# Resampling
# --------------------------------------------------------------------------
# Blackman-windowed sinc kernel, evaluated from an oversampled table. When the
# rate goes down the kernel widens proportionally, so the passband is cut at
# the destination's Nyquist: that is the anti-alias filter, and it is why an
# np.interp would not do.

KERNEL_HALF = 16  # lobes per side at a 1:1 ratio
KERNEL_OVERSAMPLE = 512
_CUTOFF = 0.90  # fraction of the lowest Nyquist, leaves room for the transition


def _build_kernel(ratio):
    """Kernel table and its half-width, in source samples."""
    scale = min(1.0, ratio)  # <1 when decimating: wider kernel
    half = KERNEL_HALF / scale
    taps = int(half * KERNEL_OVERSAMPLE) + 1

    table = [0.0] * (taps + 2)
    for i in range(taps):
        t = i / KERNEL_OVERSAMPLE  # distance in source samples
        x = _CUTOFF * scale * t
        sinc = 1.0 if x == 0.0 else math.sin(math.pi * x) / (math.pi * x)
        # Blackman, defined over [-half, half] and evaluated here only for t >= 0
        w = t / half
        if w >= 1.0:
            window = 0.0
        else:
            window = 0.42 + 0.5 * math.cos(math.pi * w) + 0.08 * math.cos(2 * math.pi * w)
        table[i] = sinc * window * _CUTOFF * scale
    return table, half


def resample(samples, src_rate, dst_rate):
    if src_rate == dst_rate or not samples:
        return list(samples)

    ratio = dst_rate / src_rate
    table, half = _build_kernel(ratio)
    n_in = len(samples)
    n_out = max(1, int(round(n_in * ratio)))

    if _np is not None:
        return _resample_numpy(samples, ratio, table, half, n_in, n_out)

    src = samples
    out = [0.0] * n_out
    for n in range(n_out):
        center = n / ratio
        first = int(math.ceil(center - half))
        last = int(math.floor(center + half))
        if first < 0:
            first = 0
        if last >= n_in:
            last = n_in - 1
        acc = 0.0
        for i in range(first, last + 1):
            # Linear interpolation inside the kernel table.
            pos = abs(i - center) * KERNEL_OVERSAMPLE
            k = int(pos)
            frac = pos - k
            acc += src[i] * (table[k] + (table[k + 1] - table[k]) * frac)
        out[n] = acc
    return out


def _resample_numpy(samples, ratio, table, half, n_in, n_out):
    src = _np.asarray(samples, dtype=_np.float64)
    tab = _np.asarray(table, dtype=_np.float64)

    centers = _np.arange(n_out, dtype=_np.float64) / ratio
    width = int(math.floor(half)) * 2 + 2
    base = _np.ceil(centers - half).astype(_np.int64)
    idx = base[:, None] + _np.arange(width, dtype=_np.int64)[None, :]

    valid = (idx >= 0) & (idx < n_in)
    idx_clipped = _np.clip(idx, 0, n_in - 1)

    pos = _np.abs(idx - centers[:, None]) * KERNEL_OVERSAMPLE
    k = pos.astype(_np.int64)
    k = _np.clip(k, 0, len(tab) - 2)
    frac = pos - k
    weights = tab[k] + (tab[k + 1] - tab[k]) * frac

    return (src[idx_clipped] * weights * valid).sum(axis=1).tolist()


# --------------------------------------------------------------------------
# Post-processing
# --------------------------------------------------------------------------


def trim_silence(samples, rate):
    """Strips silence at the head and tail. Returns (samples, trimmed)."""
    if not samples:
        return samples, 0
    threshold = 10.0 ** (TRIM_DB / 20.0)
    first, last = None, None
    for i, v in enumerate(samples):
        if abs(v) >= threshold:
            if first is None:
                first = i
            last = i
    if first is None:
        return [], len(samples)  # everything below the threshold

    guard = int(rate * TRIM_GUARD_MS / 1000.0)
    start = max(0, first - guard)
    end = min(len(samples), last + guard + 1)
    return samples[start:end], len(samples) - (end - start)


def peak_normalize(samples):
    peak = max((abs(v) for v in samples), default=0.0)
    if peak <= 0.0:
        return samples
    gain = 0.98 / peak
    return [v * gain for v in samples]


PEAK_CEILING = 0.999


def limit_overs(samples):
    """Turns a sample down only if it goes past full scale. Returns (samples, dB).

    The resampler produces overshoot (Gibbs) on transients already close to
    0 dBFS: without this step the kick clips on the attack. Unlike --normalize
    it does not touch samples that are already below, so the relative levels
    between the pieces of the kit stay as they were.
    """
    peak = max((abs(v) for v in samples), default=0.0)
    if peak <= PEAK_CEILING:
        return samples, 0.0
    gain = PEAK_CEILING / peak
    return [v * gain for v in samples], 20.0 * math.log10(gain)


# Truncating a sample mid-decay leaves a step, and a step on a 2s cymbal tail
# is an audible click. The mixer's own 1ms fade only covers the end of the
# sample it knows about, and after a truncation that end is the cut itself, so
# the ramp has to be baked in here.
TRUNC_FADE_MS = 20.0


def truncate(samples, rate, seconds):
    """Cuts to `seconds` and ramps the tail down. Returns (samples, cut)."""
    limit = int(seconds * rate)
    if limit <= 0 or len(samples) <= limit:
        return samples, False
    out = samples[:limit]
    fade = min(int(rate * TRUNC_FADE_MS / 1000.0), limit)
    if fade > 1:
        base = limit - fade
        for i in range(fade):
            out[base + i] *= 1.0 - (i / (fade - 1.0))
    return out, True


def to_int16(samples):
    """Clamps and quantises to int16. Returns (array, n_clipped_samples)."""
    out = array.array("h", bytes(2 * len(samples)))
    clipped = 0
    for i, v in enumerate(samples):
        s = int(round(v * 32767.0))
        if s > 32767:
            s = 32767
            clipped += 1
        elif s < -32768:
            s = -32768
            clipped += 1
        out[i] = s
    if sys.byteorder == "big":
        out.byteswap()
    return out, clipped


def sanitize_name(stem):
    name = "".join(c if (c.isalnum() or c in "_-") else "_" for c in stem).upper()
    name = name.strip("_") or "UNNAMED"
    return name[: NAME_LEN - 1]  # room for the NUL


# --------------------------------------------------------------------------
# Building the blob
# --------------------------------------------------------------------------


def duration_cap(args, name):
    """Seconds this sample may last, 0 for no limit. First match wins."""
    for pattern, seconds in args.max_duration_for:
        if pattern in name:
            return seconds
    return args.max_duration


def collect_inputs(args):
    """The source paths, in the order they must land in the blob."""
    if args.list:
        base = os.path.dirname(os.path.abspath(args.list))
        paths = []
        with open(args.list) as f:
            for line_no, line in enumerate(f, 1):
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                path = line if os.path.isabs(line) else os.path.join(base, line)
                if not os.path.isfile(path):
                    raise ValueError("%s:%d: file not found: %s" % (args.list, line_no, line))
                paths.append(path)
        # No sorting: the order of the file IS the order of the library.
        return paths, os.path.commonpath(paths) if paths else base

    wavs = []
    for root, _dirs, files in os.walk(args.input_dir):
        for fn in files:
            if fn.lower().endswith(".wav") and not fn.startswith("."):
                wavs.append(os.path.join(root, fn))
    wavs.sort()
    return wavs, args.input_dir


def build(args):
    try:
        wavs, rel_root = collect_inputs(args)
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 1

    if not wavs:
        print("No .wav to convert", file=sys.stderr)
        return 1

    print("Sample library -> %d Hz, 16 bit, mono" % args.rate)
    print("=" * 68)

    entries = []  # (name, int16 array)
    used_names = {}
    failures = 0

    for path in wavs:
        rel = os.path.relpath(path, rel_root)
        try:
            samples, src_rate = read_wav(path)
        except Exception as exc:
            print("  SKIP  %-28s %s" % (rel, exc))
            failures += 1
            continue

        if not samples:
            print("  SKIP  %-28s empty file" % rel)
            failures += 1
            continue

        src_len = len(samples)
        samples = resample(samples, src_rate, args.rate)

        trimmed = 0
        if not args.no_trim:
            samples, trimmed = trim_silence(samples, args.rate)
            if not samples:
                print("  SKIP  %-28s all silence below %.0f dBFS" % (rel, TRIM_DB))
                failures += 1
                continue

        if args.normalize:
            samples = peak_normalize(samples)
        samples, attenuation = limit_overs(samples)

        name = sanitize_name(os.path.splitext(os.path.basename(path))[0])

        # The longest sounds of a drum library are the ones nobody needs whole:
        # a 5s 808 cymbal is 45% of the flash budget on its own. The limit is
        # per name pattern so the cymbals can be capped without also cutting
        # the kicks, which are short and where a cut would be obvious.
        cap = duration_cap(args, name)
        was_truncated = False
        if cap:
            before = len(samples) / args.rate
            samples, was_truncated = truncate(samples, args.rate, cap)
            if was_truncated:
                print(
                    "  NOTE  %-28s truncated from %.2fs to %.2fs"
                    % (rel, before, cap)
                )

        pcm, clipped = to_int16(samples)

        if name in used_names:
            used_names[name] += 1
            suffix = "_%d" % used_names[name]
            name = name[: NAME_LEN - 1 - len(suffix)] + suffix
            print("  NOTE  %-28s duplicate name, renamed to %s" % (rel, name))
        else:
            used_names[name] = 0

        entries.append((name, pcm))

        note = ""
        if src_rate != args.rate:
            note += " (from %d Hz)" % src_rate
        if trimmed:
            note += " -%.0fms silence" % (trimmed * 1000.0 / args.rate)
        if attenuation:
            note += " %.2fdB (overshoot)" % attenuation
        if clipped:
            note += " WARNING: %d samples clipped" % clipped
        print(
            "  OK    %-28s %6.3fs  %7d samples%s"
            % (rel, len(pcm) / args.rate, len(pcm), note)
        )

    if not entries:
        print("\nNo sample converted.", file=sys.stderr)
        return 1

    # A kit-organised blob only means anything if every kit is complete: the
    # firmware indexes it as k * kit_size + slot, so one missing sample would
    # shift every kit after it onto the wrong slots. Better to refuse than to
    # produce a library that plays a hi-hat where the kick should be.
    version = VERSION_FLAT
    if args.kit_size:
        if len(entries) % args.kit_size:
            print(
                "\n%d samples is not a whole number of kits of %d: the last kit is "
                "short of %d." % (len(entries), args.kit_size,
                                  args.kit_size - len(entries) % args.kit_size),
                file=sys.stderr,
            )
            return 1
        version = VERSION_KITS

    # Layout: header, TOC, then the data with each sample 4-byte aligned.
    data_start = HEADER_SIZE + ENTRY_SIZE * len(entries)
    offset = data_start
    toc = []
    for name, pcm in entries:
        toc.append((name, offset, len(pcm)))
        offset += len(pcm) * 2
        offset = (offset + 3) & ~3
    total = offset

    blob = bytearray(total)
    struct.pack_into(
        "<4sHHII", blob, 0, MAGIC, version, len(entries), args.rate, total
    )
    for i, (name, off, length) in enumerate(toc):
        struct.pack_into(
            "<24sII", blob, HEADER_SIZE + i * ENTRY_SIZE, name.encode("ascii"), off, length
        )
    for (_name, pcm), (_n, off, _l) in zip(entries, toc):
        blob[off : off + len(pcm) * 2] = pcm.tobytes()

    with open(args.output, "wb") as f:
        f.write(blob)

    if args.header:
        write_header(args, len(entries), version)

    if args.manifest:
        with open(args.manifest, "w") as f:
            f.write("# idx  name                      samples    duration\n")
            for i, (name, _off, length) in enumerate(toc):
                if args.kit_size and i % args.kit_size == 0:
                    f.write("# --- kit %d ---\n" % (i // args.kit_size))
                f.write(
                    "%5d  %-24s  %8d  %6.3fs\n" % (i, name, length, length / args.rate)
                )

    report(args, toc, total, failures, version)
    return 1 if failures and args.strict else 0


def write_header(args, count, version):
    xip = XIP_BASE + args.flash_offset
    magic_le = struct.unpack("<I", MAGIC)[0]
    os.makedirs(os.path.dirname(args.header) or ".", exist_ok=True)
    with open(args.header, "w") as f:
        f.write(
            """// Generated by tools/convert_wav.py — do not edit by hand.
//
// The sample library does NOT live inside the firmware: it sits at a fixed
// flash offset and is read through XIP. To reflash it without rebuilding:
//
//     picotool load -o 0x%08X %s
//
#ifndef SAMPLE_LIB_H
#define SAMPLE_LIB_H

#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_LIB_FLASH_OFFSET 0x%Xu
#define SAMPLE_LIB_XIP_ADDR     0x%08Xu
#define SAMPLE_LIB_MAGIC        0x%08Xu  // 'SMPL' little-endian
#define SAMPLE_LIB_VERSION      %d  // what this blob is
#define SAMPLE_LIB_VERSION_MIN  %d  // oldest blob the firmware still reads
#define SAMPLE_LIB_RATE         %d
#define SAMPLE_LIB_NAME_LEN     %d

// Entries per kit in a version-%d blob: entry k * SAMPLE_LIB_KIT_SIZE + s is
// the sample for slot s of kit k. Must equal NUM_SLOTS.
#define SAMPLE_LIB_KIT_SIZE     %d

typedef struct {
    char     name[SAMPLE_LIB_NAME_LEN];  // NUL-padded
    uint32_t offset_bytes;               // from the start of the blob
    uint32_t length_samples;
} SampleEntry;

typedef struct {
    char        magic[4];
    uint16_t    version;
    uint16_t    count;
    uint32_t    sample_rate;
    uint32_t    total_bytes;
    SampleEntry entries[];
} SampleLib;

_Static_assert(sizeof(SampleEntry) == %d, "SampleEntry layout does not match the blob");
_Static_assert(sizeof(SampleLib) == %d, "SampleLib layout does not match the blob");

// XIP pointer to the library. Only valid once sample_lib_valid() says so.
#define SAMPLE_LIB ((const SampleLib *)SAMPLE_LIB_XIP_ADDR)

// Unprogrammed flash reads back as 0xFF: without this check the firmware would
// read garbage as a TOC and dereference random offsets.
static inline bool sample_lib_valid(const SampleLib *lib) {
    return lib->magic[0] == 'S' && lib->magic[1] == 'M' &&
           lib->magic[2] == 'P' && lib->magic[3] == 'L' &&
           lib->version >= SAMPLE_LIB_VERSION_MIN &&
           lib->version <= SAMPLE_LIB_VERSION && lib->count > 0;
}

// Whether the entries are grouped in kits. A flat version-1 blob is still
// playable, it just has no kit to load: the firmware falls back to filling the
// slots by position.
static inline bool sample_lib_is_kits(const SampleLib *lib) {
    return lib->version >= %d && lib->count >= SAMPLE_LIB_KIT_SIZE &&
           lib->count %% SAMPLE_LIB_KIT_SIZE == 0;
}

static inline unsigned sample_lib_kit_count(const SampleLib *lib) {
    return sample_lib_is_kits(lib) ? lib->count / SAMPLE_LIB_KIT_SIZE : 0;
}

static inline const int16_t *sample_lib_data(const SampleLib *lib, unsigned i) {
    return (const int16_t *)((const uint8_t *)lib + lib->entries[i].offset_bytes);
}

#endif // SAMPLE_LIB_H
"""
            % (
                xip,
                os.path.basename(args.output),
                args.flash_offset,
                xip,
                magic_le,
                version,
                VERSION_FLAT,
                args.rate,
                NAME_LEN,
                VERSION_KITS,
                # The size this blob was actually built with, not the module
                # default: writing the default here once shipped a header
                # claiming kits of 6 for a blob laid out in kits of 8, and the
                # firmware quietly decided the library was not kit-organised.
                args.kit_size or KIT_SIZE,
                ENTRY_SIZE,
                HEADER_SIZE,
                VERSION_KITS,
            )
        )


def report(args, toc, total, failures, version):
    total_samples = sum(t[2] for t in toc)
    budget = args.flash_size - args.flash_offset
    print("=" * 68)
    print("  samples converted : %d" % len(toc))
    if version == VERSION_KITS:
        print(
            "  layout            : %d kits of %d (blob version %d)"
            % (len(toc) // args.kit_size, args.kit_size, version)
        )
    if failures:
        print("  files skipped     : %d" % failures)
    print("  total duration    : %.2fs" % (total_samples / args.rate))
    print(
        "  blob              : %s (%.1f KB), TOC %d B + data"
        % (args.output, total / 1024.0, ENTRY_SIZE * len(toc))
    )
    print(
        "  flash budget      : %.1f KB of %.1f KB available (%.1f%%)"
        % (total / 1024.0, budget / 1024.0, 100.0 * total / budget)
    )
    if total > budget:
        print("  WARNING: the library exceeds the space available in flash!")
    print()
    print("  To flash it:")
    print(
        "    picotool load -o 0x%08X %s"
        % (XIP_BASE + args.flash_offset, args.output)
    )


def parse_duration_for(spec):
    """PATTERN=SECONDS for --max-duration-for."""
    if "=" not in spec:
        raise argparse.ArgumentTypeError("expected PATTERN=SECONDS, got %r" % spec)
    pattern, _, seconds = spec.partition("=")
    try:
        value = float(seconds)
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not a number of seconds" % seconds)
    if value <= 0:
        raise argparse.ArgumentTypeError("the limit must be positive, got %s" % seconds)
    return (sanitize_name(pattern), value)


def main():
    p = argparse.ArgumentParser(
        description="Builds the sample library blob for the Pico 2 Sample Player.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "input_dir",
        nargs="?",
        help="directory holding the source .wav files (recursive, sorted by path)",
    )
    p.add_argument(
        "--list",
        help="text file with one .wav path per line, IN ORDER; '#' starts a comment. "
        "Paths are relative to the file itself. Use it when the order of the "
        "library carries meaning, instead of relying on the file names sorting right",
    )
    p.add_argument(
        "--kit-size",
        type=int,
        default=0,
        help="entries per kit (0 = flat library). With a value, the count must be a "
        "whole number of kits and the blob is written as version %d" % VERSION_KITS,
    )
    p.add_argument("-o", "--output", default="sample_lib.bin", help="output blob")
    p.add_argument(
        "--header",
        default="src/sample_lib.h",
        help="C header with the TOC and constants ('' to skip generating it)",
    )
    p.add_argument(
        "--manifest",
        default="sample_lib_manifest.txt",
        help="human-readable listing of the samples ('' to skip generating it)",
    )
    p.add_argument("--rate", type=int, default=DEFAULT_RATE, help="target sample rate")
    p.add_argument(
        "--flash-offset",
        type=lambda s: int(s, 0),
        default=DEFAULT_FLASH_OFFSET,
        help="flash offset of the library",
    )
    p.add_argument(
        "--flash-size",
        type=lambda s: int(s, 0),
        default=4 * 1024 * 1024,
        help="total flash size",
    )
    p.add_argument(
        "--max-duration",
        type=float,
        default=0.0,
        help="truncate samples longer than N seconds (0 = no limit)",
    )
    p.add_argument(
        "--max-duration-for",
        metavar="PATTERN=SECONDS",
        action="append",
        default=[],
        type=parse_duration_for,
        help="per-name limit, overriding --max-duration for the samples whose "
        "(sanitised, uppercase) name contains PATTERN. Repeatable, first match "
        "wins: --max-duration-for FLEX=2.0 caps the cymbals without touching "
        "the kicks",
    )
    p.add_argument(
        "--normalize",
        action="store_true",
        help="normalise each sample to its peak (default off: preserves the kit's relative levels)",
    )
    p.add_argument(
        "--no-trim", action="store_true", help="do not strip the silence at head/tail"
    )
    p.add_argument(
        "--strict", action="store_true", help="exit with an error if any file was skipped"
    )
    args = p.parse_args()

    if bool(args.input_dir) == bool(args.list):
        print("Give either a directory or --list, not both and not neither.",
              file=sys.stderr)
        return 1
    if args.input_dir and not os.path.isdir(args.input_dir):
        print("Directory not found: %s" % args.input_dir, file=sys.stderr)
        return 1
    if args.list and not os.path.isfile(args.list):
        print("List file not found: %s" % args.list, file=sys.stderr)
        return 1
    if args.kit_size < 0:
        print("--kit-size cannot be negative.", file=sys.stderr)
        return 1
    return build(args)


if __name__ == "__main__":
    sys.exit(main())
