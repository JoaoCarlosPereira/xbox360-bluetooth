"""Build the BTManager's font and icon atlas from the bundled Lato fonts."""
import sys
from math import exp, sin
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit("Install Pillow for the Python interpreter selected by BTMANAGER_PYTHON.")

WIDTH, HEIGHT = 1024, 1024
FONT_LIMIT = 512
atlas = Image.new("L", (WIDTH, HEIGHT))
draw = ImageDraw.Draw(atlas)
draw.rectangle((0, 0, 3, 3), fill=255)
draw.ellipse((768, 0, 1023, 255), fill=255)
draw.line([(105, 5), (105, 58), (124, 42), (85, 19)], fill=255, width=5)
draw.line([(85, 45), (124, 21), (105, 5)], fill=255, width=5)
draw.rounded_rectangle((136, 14, 193, 50), radius=14, outline=255, width=4)
draw.line((146, 32, 161, 32), fill=255, width=4)
draw.line((153, 25, 153, 39), fill=255, width=4)
draw.ellipse((174, 26, 179, 31), fill=255)
draw.ellipse((181, 34, 186, 39), fill=255)
draw.ellipse((207, 9, 242, 44), outline=255, width=5)
draw.line((212, 40, 199, 55), fill=255, width=7)

folder = Path(__file__).parent
glyphs = []
x, y, row_height = 0, 66, 0
points = (*range(32, 127), *range(160, 256))
for file, size in (("Lato-Regular.ttf", 20), ("Lato-Heavy.ttf", 25), ("Lato-Heavy.ttf", 48)):
    font = ImageFont.truetype(folder / file, size)
    face = []
    for point in points:
        char = chr(point)
        l, t, r, b = font.getbbox(char, anchor="ls")
        w, h = r - l, b - t
        row_width = 768 if y < 256 else WIDTH
        if x + w + 2 > row_width:
            x, y, row_height = 0, y + row_height, 0
        if y + h + 2 > FONT_LIMIT:
            raise ValueError("Glyphs do not fit in the atlas")
        if w and h:
            draw.text((x + 1 - l, y + 1 - t), char, 255, font=font, anchor="ls")
        face.append((x + 1, y + 1, w, h, l, t, round(font.getlength(char))))
        x += w + 2
        row_height = max(row_height, h + 2)
    glyphs.append(face)


def add_mask(position, size, sample):
    w, h = size
    values = bytes(round(255 * max(0, min(1, sample(x / (w - 1), y / (h - 1)))))
                   for y in range(h) for x in range(w))
    atlas.paste(Image.frombytes("L", size, values), position)


def silver(x, y):
    return 0.13 + 0.59 * exp(-((x - 0.46) / 0.73)**2 - ((y - 0.65) / 0.42)**2)


def ribbon(x, y):
    center = 1.21 - 0.88 * x + 0.12 * sin(5 * x)
    broad = 0.62 * exp(-((y - center) / 0.22)**2)
    crest = 0.30 * exp(-((y - center + 0.10) / 0.018)**2)
    return (broad + crest) * x**1.3


def gleam(_, y):
    return 0.7 * (1 - y)**1.35


add_mask((0, 512), (512, 288), silver)
add_mask((512, 512), (512, 288), ribbon)
add_mask((0, 800), (8, 128), gleam)

with Path(sys.argv[1]).open("w") as output:
    print(f"enum {{ ATLAS_WIDTH = {WIDTH}, ATLAS_HEIGHT = {HEIGHT} }};", file=output)
    print(f"static const Glyph glyphs[3][{len(points)}] = {{", file=output)
    for face in glyphs:
        print("{", file=output)
        for g in face:
            print("{" + ",".join(map(str, g)) + "},", file=output)
        print("},", file=output)
    print("};", file=output)
    print("static const uint8_t atlas_pixels[] = {", file=output)
    data = atlas.tobytes()
    for start in range(0, len(data), 64):
        print(",".join(map(str, data[start:start+64])) + ",", file=output)
    print("};", file=output)
