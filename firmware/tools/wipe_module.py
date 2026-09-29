#!/usr/bin/env python3
"""Erase the sample library from a module, so a board tested on the bench with
the licensed development library can be shipped empty.

    tools/wipe_module.py                 erase the sample library
    tools/wipe_module.py --settings      erase the settings and the 8 presets too
    tools/wipe_module.py --verify-full   read the whole region back, not just the header
    tools/wipe_module.py -y              do not ask for confirmation

This is a real erase, not an invalidated header: the PCM data is gone from the
part, which is the only version of "wipe" that answers the licensing question.

The addresses are read out of src/sample_lib.h and src/settings*.h rather than
written down here. A tool carrying its own copy of a constant instead of the
one in play has already cost this project two bugs.

The board does not need to be held in BOOTSEL: every picotool call carries -f,
so a module running its own firmware is rebooted into the bootloader and back.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"
XIP_BASE = 0x10000000

# PICO_FLASH_SIZE_BYTES lives in the SDK's board header, not in this repo, so
# it is the one number here that cannot be derived. It decides where the
# settings sectors are, hence where the library has to stop: it is checked
# against what the device reports before anything is erased.
FLASH_SIZE = 4 * 1024 * 1024


def define(header, name):
    """The value of a #define, read from the firmware's own header."""
    text = (SRC / header).read_text()
    m = re.search(r"^#define\s+%s\s+(0x[0-9a-fA-F]+|\d+)u?\b" % name, text, re.M)
    if not m:
        sys.exit("%s: #define %s not found" % (header, name))
    return int(m.group(1), 0)


def picotool(args, force, capture=False, check=True):
    cmd = ["picotool"] + args + (["-f"] if force else [])
    r = subprocess.run(cmd, capture_output=capture, text=True)
    if check and r.returncode != 0:
        if capture:
            sys.stderr.write(r.stdout + r.stderr)
        sys.exit("picotool failed: %s" % " ".join(cmd))
    return (r.returncode, (r.stdout or "") + (r.stderr or "")) if capture else (r.returncode, "")


def device_flash_size(force):
    """What the part says it has. None when picotool does not report the size;
    the script stops here if no module answers at all, which is the common
    case and deserves better than a picotool exit code."""
    rc, out = picotool(["info", "-a"], force, capture=True, check=False)
    if rc != 0:
        sys.exit("no module answered. Plug it in — with -f it does not need to "
                 "be held in BOOTSEL, but it does need to be running firmware "
                 "that picotool can reboot.\npicotool said: " + out.strip())
    m = re.search(r"flash size:\s*(\d+)\s*([KM])", out, re.I)
    if not m:
        return None
    return int(m.group(1)) * (1024 if m.group(2).upper() == "K" else 1024 * 1024)


def read_back(start, end, force):
    """Read a range off the device and return it. A fresh directory rather
    than a temp file, so picotool is never asked to overwrite one."""
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / "readback.bin"
        picotool(["save", "-r", hex(start), hex(end), str(out), "-t", "bin"],
                 force, capture=True)  # captured: it prints a progress bar
        return out.read_bytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--settings", action="store_true",
                    help="erase the settings and presets as well")
    ap.add_argument("--verify-full", action="store_true",
                    help="read the whole erased region back, not just the first sector")
    ap.add_argument("-y", "--yes", action="store_true", help="skip the confirmation")
    ap.add_argument("--no-force", action="store_true",
                    help="the board is already in BOOTSEL, do not pass -f")
    args = ap.parse_args()
    force = not args.no_force

    lib_offset = define("sample_lib.h", "SAMPLE_LIB_FLASH_OFFSET")
    sector = define("settings_flash.h", "SETTINGS_FLASH_SECTOR_SIZE")
    sectors = define("settings.h", "SETTINGS_SECTORS")
    settings_offset = FLASH_SIZE - sectors * sector

    # The firmware sits below the library and must survive; the settings sit
    # above it and only go when asked. Getting this range wrong is the one way
    # this script can do real damage, so it is checked rather than trusted.
    if lib_offset <= 0 or lib_offset % sector:
        sys.exit("the library offset is not a sector boundary: %#x" % lib_offset)
    if settings_offset <= lib_offset or settings_offset % sector:
        sys.exit("the settings offset makes no sense: %#x" % settings_offset)

    regions = [("sample library", lib_offset, settings_offset)]
    if args.settings:
        regions.append(("settings + presets", settings_offset, FLASH_SIZE))

    print("about to erase, on the connected module:")
    for name, start, end in regions:
        print("  %-18s %#010x .. %#010x  %5d KB  %4d sectors"
              % (name, XIP_BASE + start, XIP_BASE + end,
                 (end - start) // 1024, (end - start) // sector))
    print("  firmware at %#010x .. %#010x is left alone"
          % (XIP_BASE, XIP_BASE + lib_offset))

    on_part = device_flash_size(force)
    if on_part is not None and on_part != FLASH_SIZE:
        sys.exit("the device reports %d KB of flash, this script assumes %d KB.\n"
                 "Fix FLASH_SIZE before erasing: the settings sectors are not "
                 "where it thinks they are." % (on_part // 1024, FLASH_SIZE // 1024))

    if not args.yes:
        if input('this cannot be undone. Type "wipe" to confirm: ').strip() != "wipe":
            sys.exit("nothing erased")

    for name, start, end in regions:
        print("erasing %s ..." % name)
        picotool(["erase", "-r", hex(XIP_BASE + start), hex(XIP_BASE + end)], force)

    for name, start, end in regions:
        stop = end if args.verify_full else min(start + sector, end)
        data = read_back(XIP_BASE + start, XIP_BASE + stop, force)
        rest = data.lstrip(b"\xff")  # erased flash is all ones
        if rest:
            bad = len(data) - len(rest)
            sys.exit("%s: %#010x reads back as %#04x, not erased"
                     % (name, XIP_BASE + start + bad, rest[0]))
        print("verified: %s, %d KB read back as 0xff" % (name, len(data) // 1024))

    if not args.verify_full:
        print("(only the first sector was read back — pass --verify-full for the lot)")
    print("the module will come up with NO LIBRARY.")
    print("to put one back: picotool load -o %#x sample_lib.bin"
          % (XIP_BASE + lib_offset))


if __name__ == "__main__":
    main()
