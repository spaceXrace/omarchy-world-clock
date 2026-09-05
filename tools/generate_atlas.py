#!/usr/bin/env python3
"""Generate the redistributable font/glow/star atlas used by the renderer."""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import math

ROOT = Path(__file__).resolve().parents[1]
FONT = Path("/usr/share/fonts/TTF/DejaVuSansCondensed-Bold.ttf")
SIZES = (20, 22, 23)
ATLAS_W, ATLAS_H = 1024, 256


def main():
    if not FONT.exists():
        raise SystemExit("DejaVu Sans is required to regenerate the atlas (package: ttf-dejavu)")

    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H))
    draw = ImageDraw.Draw(atlas)
    fonts = []

    for slot, size in enumerate(SIZES):
        font = ImageFont.truetype(str(FONT), size)
        ascent, descent = font.getmetrics()
        height = ascent + descent + 2
        x, y = slot * 256 + 1, 1
        rows = [[0.0, 1.0, 0.0, 1.0, 0.0] for _ in range(256)]

        for code in range(32, 127):
            char = chr(code)
            advance = max(1, round(font.getlength(char)))
            cell_width = advance + 2
            if x + cell_width >= (slot + 1) * 256:
                x = slot * 256 + 1
                y += height
            if y + height >= ATLAS_H:
                raise SystemExit(f"font slot {slot} overflowed the atlas")
            draw.text((x + 1, y + 1 + ascent), char, font=font, fill="white", anchor="ls")
            rows[code] = [x / ATLAS_W, y / ATLAS_H,
                          (x + cell_width) / ATLAS_W, (y + height) / ATLAS_H,
                          float(advance)]
            x += cell_width
        fonts.append((float(height), rows))

    for y in range(128):
        for x in range(128):
            radius = math.hypot((x - 63.5) / 63.5, (y - 63.5) / 63.5)
            alpha = max(0, math.exp(-radius * radius * 7) - math.exp(-7)) if radius < 1 else 0
            atlas.putpixel((768 + x, y), (255, 255, 255, round(alpha * 255)))
    for y in range(32):
        for x in range(32):
            radius = math.hypot((x - 15.5) / 15.5, (y - 15.5) / 15.5)
            alpha = max(0, min(1, (1 - radius) * 2.5))
            atlas.putpixel((896 + x, y), (255, 255, 255, round(alpha * 255)))

    atlas.save(ROOT / "assets/atlas.png", optimize=True)
    with (ROOT / "src/fonts.h").open("w") as output:
        output.write("/* Generated from DejaVu Sans; see third-party/DEJAVU-FONTS-LICENSE.txt. */\n")
        output.write("static const float font_heights[3] = {" + ",".join(str(item[0]) for item in fonts) + "};\n")
        output.write("static const float glyphs[3][256][5] = {\n")
        for _, rows in fonts:
            output.write("{\n" + ",\n".join("{" + ",".join(f"{value:.9f}f" for value in row) + "}" for row in rows) + "\n},\n")
        output.write("};\n")


if __name__ == "__main__":
    main()
