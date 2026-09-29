#!/usr/bin/env python3
"""Convert the brand SVG (and the splash's copyright line) into the 1-bit
bitmaps baked into the firmware.

The logo source, webapp/exportsBrandStyle/logomark-black.svg, is one
drawing: a DIP-8 chip body (8 pin legs + a rounded rect) with the
circle+dot+"uR" mark cut *into* it as a mask - the crescent and the dot are
silkscreen-style details on the chip's own body, not a separate mark next to
it. Earlier versions of this tool split those into two bitmaps drawn side by
side, which made the crescent+dot read as a little face next to the chip
instead of as part of it. This renders the SVG exactly as authored - one
shape, mask included - and packs that.

The copyright line ("(c) muR Lab 2026") is pre-rendered the same way rather
than drawn with the UI's own font5x7 at runtime: it needs to read distinctly
smaller than the rest of the splash, and font5x7's 5x7 cells are already the
smallest text anywhere else in the UI, so a second, genuinely tiny font
table would exist only for this one fixed string. Baking it as a bitmap
instead means no new font asset for a string that never changes at runtime -
exactly the same trade a fixed-width font table would have made, minus the
table.

Rasterising text or an SVG with real glyphs (the u/R letters) needs a
renderer that does font substitution properly. The logo uses macOS's own
`qlmanage` (Quick Look); the copyright line renders directly through Pillow
with a system TrueType font, which is simpler for plain text and does not
need a temp SVG file. Both are already on every Mac that can run build.sh,
rather than adding a cairosvg/resvg dependency for a script that runs once
per logo or copyright change.

Usage:
    tools/make_logo.py webapp/exportsBrandStyle/logomark-black.svg

Regenerate whenever the source artwork or the copyright text changes; the
header this writes (src/logo_bitmap.h) is committed like sample_lib.h's
shape is, not rebuilt from Python at firmware build time.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

RASTER_SIZE = 1600       # qlmanage render size in px, cropped tight afterward
INK_THRESHOLD = 128      # luminance below this, on a white canvas -> ink
BILEVEL_THRESHOLD = 110  # after resampling, gray above this -> lit pixel

COPYRIGHT_TEXT = "(c) muR Lab 2026"
COPYRIGHT_FONT = "/System/Library/Fonts/Supplemental/Courier New Bold.ttf"
COPYRIGHT_SOURCE_PT = 200  # rendered large, then downsampled - see render()


def rasterize_svg(svg_path, workdir):
    if shutil.which("qlmanage") is None:
        sys.exit("qlmanage not found - this tool needs macOS's Quick Look "
                 "to rasterise SVG text properly. Export a PNG by hand and "
                 "adapt this script if run elsewhere.")
    subprocess.run(
        ["qlmanage", "-t", "-s", str(RASTER_SIZE), "-o", str(workdir), str(svg_path)],
        check=True, capture_output=True)
    png_path = workdir / f"{svg_path.name}.png"
    if not png_path.exists():
        sys.exit(f"qlmanage did not produce a thumbnail for {svg_path.name}")
    return Image.open(png_path).convert("RGBA")


def rasterize_text(text, font_path):
    font = ImageFont.truetype(font_path, COPYRIGHT_SOURCE_PT)
    im = Image.new("L", (COPYRIGHT_SOURCE_PT * len(text) * 2, COPYRIGHT_SOURCE_PT * 2), 0)
    ImageDraw.Draw(im).text((0, 0), text, font=font, fill=255)
    return im


def extract_shape(im, dark_is_ink):
    """Tight-crop to whichever polarity is ink: dark-on-white for the
    qlmanage SVG render, or the light glyphs already drawn on black for the
    text render (rasterize_text draws white ink directly, no inversion
    needed there - dark_is_ink is False for it)."""
    gray = im.convert("L")
    mask = gray.point(lambda p: 255 if (p < INK_THRESHOLD) == dark_is_ink else 0)
    bbox = mask.getbbox()
    if bbox is None:
        sys.exit("no ink found - rasterisation failed?")
    return mask.crop(bbox)


def pack_1bit(im):
    """Row-major, MSB-first, ceil(w/8) bytes per row - what
    Image.convert('1').tobytes() produces, decoded by ssd1306_bitmap()."""
    w, h = im.size
    stride = (w + 7) // 8
    data = bytearray(stride * h)
    px = im.load()
    for y in range(h):
        for x in range(w):
            if px[x, y]:
                data[y * stride + x // 8] |= 0x80 >> (x % 8)
    return bytes(data)


def emit_array(out, data):
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        out.append("    " + ", ".join(f"0x{b:02X}" for b in row) + ",")


def render(shape, target_h):
    w = round(target_h * shape.size[0] / shape.size[1])
    resized = shape.resize((w, target_h), Image.LANCZOS)
    bilevel = resized.point(lambda p: 255 if p > BILEVEL_THRESHOLD else 0)
    return w, target_h, pack_1bit(bilevel)


def emit_bitmap_block(out, name, w, h, data):
    out.append(f"#define {name}_W {w}")
    out.append(f"#define {name}_H {h}")
    out.append(f"static const uint8_t {name}_BITS[] = {{")
    emit_array(out, data)
    out.append("};")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="brand SVG, the chip with its own <mask> cutouts")
    ap.add_argument("-o", "--output", default="src/logo_bitmap.h")
    ap.add_argument("--h32", type=int, default=22,
                     help="logo height in px for the 128x32 splash (default 22)")
    ap.add_argument("--h64", type=int, default=48,
                     help="logo height in px for the 128x64 splash (default 48)")
    ap.add_argument("--copy-h32", type=int, default=6,
                     help="copyright bitmap height in px on 128x32, 0 to omit "
                          "it there and keep drawing the string with font5x7 "
                          "(default 6)")
    ap.add_argument("--copy-h64", type=int, default=0,
                     help="same, for 128x64 (default 0: omitted, not asked for yet)")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        logo = extract_shape(rasterize_svg(Path(args.source).resolve(), workdir), True)
    print(f"logo extracted: {logo.size[0]}x{logo.size[1]}, "
          f"aspect {logo.size[0] / logo.size[1]:.3f}")

    copyright_shape = None
    if args.copy_h32 or args.copy_h64:
        copyright_shape = extract_shape(
            rasterize_text(COPYRIGHT_TEXT, COPYRIGHT_FONT), False)
        print(f"copyright extracted: {copyright_shape.size[0]}x{copyright_shape.size[1]}, "
              f"aspect {copyright_shape.size[0] / copyright_shape.size[1]:.3f}")

    out = [
        "// Boot splash bitmaps, generated by tools/make_logo.py from",
        f"// {args.source}",
        f'// and the literal string "{COPYRIGHT_TEXT}" - do not hand-edit either.',
        "//",
        "// Bitmaps compiled in are picked by OLED_H like the rest of the panel-",
        "// specific code: a panel that does not reference one of these must not",
        "// define it either, or it is an unused static array under -Werror.",
        "#ifndef LOGO_BITMAP_H",
        "#define LOGO_BITMAP_H",
        "",
        "#include <stdint.h>",
        "",
        "#include \"ssd1306.h\"",
        "",
        "#if OLED_H == 64",
    ]
    w, h, data = render(logo, args.h64)
    print(f"128x64 logo: {w}x{h}, {len(data)} bytes")
    emit_bitmap_block(out, "LOGO", w, h, data)
    if args.copy_h64:
        w, h, data = render(copyright_shape, args.copy_h64)
        print(f"128x64 copyright: {w}x{h}, {len(data)} bytes")
        emit_bitmap_block(out, "COPYRIGHT", w, h, data)
    out.append("#else")
    w, h, data = render(logo, args.h32)
    print(f"128x32 logo: {w}x{h}, {len(data)} bytes")
    emit_bitmap_block(out, "LOGO", w, h, data)
    if args.copy_h32:
        w, h, data = render(copyright_shape, args.copy_h32)
        print(f"128x32 copyright: {w}x{h}, {len(data)} bytes")
        emit_bitmap_block(out, "COPYRIGHT", w, h, data)
    out += [
        "#endif",
        "",
        "#endif // LOGO_BITMAP_H",
        "",
    ]

    with open(args.output, "w") as f:
        f.write("\n".join(out))
    print(f"written {args.output}")


if __name__ == "__main__":
    main()
