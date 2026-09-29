#!/usr/bin/env bash
# Tests that run on the Mac, with no hardware.
#
# The logic modules do not depend on the SDK, only on their own headers: they
# compile natively and get verified before anything is flashed.
#
#   mixer   fades, choke, voice allocation, saturation, kit loading
#   encoder quadrature decoding, debounce, long press
#   midi    running status, interleaved clock, velocity 0, channel filter
#   ui      select/assign/kit transitions and framebuffer content
#   settings persistence: virgin flash, A/B sectors, power cut, CRC
#
# The UI runs against a stub of the I2C bus: the framebuffer the tests inspect
# is the same one that would end up on the display. The library blob is re-read
# by an independent parser.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CFLAGS=(-std=c11 -Wall -Wextra -Werror -O1 -I "$HERE/src")

echo "== mixer =="
cc "${CFLAGS[@]}" \
    "$HERE/test/test_mixer.c" "$HERE/src/mixer.c" -o "$OUT/test_mixer"
"$OUT/test_mixer"

echo
echo "== encoder =="
cc "${CFLAGS[@]}" \
    "$HERE/test/test_encoder.c" "$HERE/src/encoder.c" -o "$OUT/test_encoder"
"$OUT/test_encoder"

echo
echo "== midi =="
cc "${CFLAGS[@]}" \
    "$HERE/test/test_midi.c" "$HERE/src/midi.c" -o "$OUT/test_midi"
"$OUT/test_midi"

echo
echo "== ui (1U/128x32) =="
cc "${CFLAGS[@]}" \
    "$HERE/test/test_ui.c" "$HERE/test/stub_oled_bus.c" \
    "$HERE/src/ui.c" "$HERE/src/ssd1306.c" "$HERE/src/mixer.c" \
    "$HERE/src/settings.c" \
    -o "$OUT/test_ui"
"$OUT/test_ui"

echo
echo "== ui (6HP/128x64) =="
cc "${CFLAGS[@]}" -DOLED_PANEL_128X64 \
    "$HERE/test/test_ui.c" "$HERE/test/stub_oled_bus.c" \
    "$HERE/src/ui.c" "$HERE/src/ssd1306.c" "$HERE/src/mixer.c" \
    "$HERE/src/settings.c" \
    -o "$OUT/test_ui_6hp"
"$OUT/test_ui_6hp"

echo
echo "== settings =="
cc "${CFLAGS[@]}" \
    "$HERE/test/test_settings.c" "$HERE/src/settings.c" "$HERE/src/mixer.c" \
    -o "$OUT/test_settings"
"$OUT/test_settings"

if [ -f "$HERE/sample_lib.bin" ]; then
    echo
    echo "== sample library =="
    python3 "$HERE/tools/verify_blob.py" "$HERE/sample_lib.bin"
else
    echo
    echo "== sample library: sample_lib.bin missing, skipped =="
fi
