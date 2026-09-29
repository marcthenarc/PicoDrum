#!/usr/bin/env python3
"""Encoder probe: runs the firmware's 'e' command and shows the result.

    tools/probe_encoder.py

As soon as the message appears, TURN THE KNOB. You have 3 seconds.

What to read in the result:

    state trace (A<<1|B): 013201320132...   healthy quadrature
    state trace (A<<1|B): 030303030303...   the two pins are the same node

In the second case the two bits change together, the two-bit jump has no
direction and the decoder never emits a step: CLK and DT shorted together, or a
defective encoder. Swap the part before debugging the firmware.
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

subprocess.run(["stty", "-f", port, "115200", "raw", "-echo"], check=True)
fd = os.open(port, os.O_RDWR | os.O_NONBLOCK)


def drain(seconds, echo=False):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        out += chunk
        if echo:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    return out.decode("utf-8", "replace")


# The firmware repeats the banner until it receives a key: discard it.
drain(1.5)
print(f">>> port {port}. Sending 'e': as soon as you see the message, TURN THE KNOB.\n",
      flush=True)
os.write(fd, b"e")
drain(6, echo=True)
os.close(fd)
