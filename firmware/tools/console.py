#!/usr/bin/env python3
"""Drives the firmware's USB console from the Mac, with no terminal emulator.

    tools/console.py i          state and statistics
    tools/console.py m          MIDI counters
    tools/console.py p 4        probe the MIDI pin (give it time to finish)
    tools/console.py a 20       burst for 20s, then 'a' again to stop it

Every character of the first argument is sent as a separate command, and
whatever comes back within the wait is printed. That is enough to script the
checks that used to need a terminal open by hand: flash, read 'i', run a burst,
read it again.

The second argument is the seconds to wait after each character. The default
suits the quick commands; 'p' takes 2s of probing on its own and 'a' runs until
it is toggled off, so those need it raised.
"""
import glob
import os
import select
import subprocess
import sys
import time

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("No Pico connected (no /dev/cu.usbmodem*).")
port = ports[0]

# raw and -echo: the console is a byte stream, and the line discipline would
# otherwise swallow single characters until a newline arrives.
subprocess.run(["stty", "-f", port, "115200", "raw", "-echo"], check=True)
fd = os.open(port, os.O_RDWR | os.O_NONBLOCK)


def drain(seconds):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                out += os.read(fd, 4096)
            except OSError:
                pass
    return out.decode("utf-8", "replace")


cmds = sys.argv[1] if len(sys.argv) > 1 else "i"
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0

drain(0.4)  # the boot banner repeats until a key arrives; throw it away
for c in cmds:
    os.write(fd, c.encode())
    sys.stdout.write(drain(wait))
    sys.stdout.flush()
os.close(fd)
