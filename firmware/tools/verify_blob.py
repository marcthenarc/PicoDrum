#!/usr/bin/env python3
"""Check a sample library blob using the same rules the firmware will apply
when reading it through XIP.

It is a parser independent of convert_wav.py: if the two agree, the format
really is what both sides believe it to be.

    tools/verify_blob.py sample_lib.bin
"""

import array
import math
import struct
import sys

HEADER_SIZE = 16
ENTRY_SIZE = 32
# Entries per kit in a version-2 blob, and the slot each position stands for.
# The blob header does not carry the kit size, so this list is what the check
# compares against: keep it in step with ROLES in pick_kit.py and SLOT_ROLE in
# the firmware.
ROLES = ["KICK", "SNARE", "CH", "OH", "FLEX1", "FLEX2", "FLEX3", "FLEX4"]
KIT_SIZE = len(ROLES)


def verify(path):
    with open(path, "rb") as f:
        blob = f.read()

    if len(blob) < HEADER_SIZE:
        print("file too short to hold a header", file=sys.stderr)
        return 1

    magic, version, count, rate, total = struct.unpack_from("<4sHHII", blob, 0)
    problems = []

    if magic != b"SMPL":
        problems.append("magic %r instead of b'SMPL'" % magic)
    if version not in (1, 2):
        problems.append("version %d not supported" % version)
    # A version-2 blob is indexed as kit * KIT_SIZE + slot. A count that is not
    # a whole number of kits would make the firmware read the last kit past the
    # end of the TOC, so it is a defect in the blob, not a detail.
    if version == 2 and count % KIT_SIZE:
        problems.append(
            "version 2 with %d entries: not a whole number of kits of %d"
            % (count, KIT_SIZE)
        )
    if total != len(blob):
        problems.append("total_bytes %d but the file is %d bytes" % (total, len(blob)))

    print("magic=%s version=%d count=%d rate=%d total=%d"
          % (magic.decode("ascii", "replace"), version, count, rate, total))
    if version == 2:
        print("layout: %d kits of %d" % (count // KIT_SIZE, KIT_SIZE))

    data_start = HEADER_SIZE + ENTRY_SIZE * count
    prev_end = data_start
    peak_global = 0

    for i in range(count):
        name_b, off, length = struct.unpack_from("<24sII", blob, HEADER_SIZE + i * ENTRY_SIZE)
        name = name_b.split(b"\0")[0].decode("ascii", "replace")

        if b"\0" not in name_b:
            problems.append("[%d] name with no NUL terminator" % i)
        if off % 4:
            problems.append("[%d] %s: offset %d not 4-byte aligned" % (i, name, off))
        if off < data_start:
            problems.append("[%d] %s: the data runs into the TOC" % (i, name))
        if off < prev_end:
            problems.append("[%d] %s: overlaps the previous sample" % (i, name))
        if off + length * 2 > total:
            problems.append("[%d] %s: the data runs past the end of the blob" % (i, name))
            continue

        pcm = array.array("h")
        pcm.frombytes(blob[off : off + length * 2])
        if sys.byteorder == "big":
            pcm.byteswap()

        peak = max(max(pcm), -min(pcm)) if len(pcm) else 0
        peak_global = max(peak_global, peak)
        dc = sum(pcm) / len(pcm) if len(pcm) else 0.0

        prev_end = off + length * 2
        if version == 2 and i % KIT_SIZE == 0:
            print("  --- kit %d" % (i // KIT_SIZE))
        print("  [%2d] %-22s off=%-8d len=%-7d peak=%-6d dc=%+7.1f  %.3fs"
              % (i, name, off, length, peak, dc, length / rate if rate else 0))

    if version == 2:
        for i in range(count):
            name_b, _o, _l = struct.unpack_from(
                "<24sII", blob, HEADER_SIZE + i * ENTRY_SIZE
            )
            name = name_b.split(b"\0")[0].decode("ascii", "replace")
            want = ROLES[i % KIT_SIZE]
            # The firmware never reads these names, it goes by position — which
            # is exactly why a mismatch has to be caught here: it would put a
            # hi-hat on the kick slot and nothing downstream would notice.
            if not name.endswith("_" + want):
                problems.append(
                    "[%d] %s sits on the %s slot of kit %d"
                    % (i, name, want, i // KIT_SIZE)
                )

    pad = total - prev_end
    if pad >= 4:
        problems.append("trailing padding of %d bytes, expected less than 4" % pad)

    if peak_global:
        print("\nglobal peak: %d (%.2f dBFS)"
              % (peak_global, 20 * math.log10(peak_global / 32767.0)))

    if problems:
        print("\n%d PROBLEMS:" % len(problems))
        for p in problems:
            print("  - %s" % p)
        return 1

    print("OK: blob consistent")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(verify(sys.argv[1]))
