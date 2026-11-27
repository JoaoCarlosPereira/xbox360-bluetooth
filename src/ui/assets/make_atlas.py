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

# 0: Solid white texel for dash_rect
draw.rectangle((0, 0, 3, 3), fill=255)

# 1: High-res circle for status dots, Xbox button circles, and dash_pill caps
draw.ellipse((768, 0, 1023, 255), fill=255)

# 2: Bluetooth icon (x=76, w=52, h=64)
draw.line([(105, 5), (105, 58), (124, 42), (85, 19)], fill=255, width=5)
draw.line([(85, 45), (124, 21), (105, 5)], fill=255, width=5)

# 3: Gamepad icon (x=132, w=64, h=64)
draw.rounded_rectangle((136, 14, 193, 50), radius=14, outline=255, width=4)
draw.line((146, 32, 161, 32), fill=255, width=4)
draw.line((153, 25, 153, 39), fill=255, width=4)
draw.ellipse((174, 26, 179, 31), fill=255)
draw.ellipse((181, 34, 186, 39), fill=255)

# 4: Search magnifying glass icon (x=196, w=52, h=64)
draw.ellipse((207, 9, 242, 44), outline=255, width=5)
draw.line((212, 40, 199, 55), fill=255, width=7)

# 5..9: Xbox 360 Ring of Light (base 4-segment ring + Q1..Q4 individual quadrants, 64x64 each)
# Pillow arc angles: 0=3h, 90=6h, 180=9h, 270=12h (clockwise).
ROL_ARCS = (
    (190, 260),  # Q1: Jogador 1 (top-left)
    (280, 350),  # Q2: Jogador 2 (top-right)
    (100, 170),  # Q3: Jogador 3 (bottom-left)
    (10, 80),    # Q4: Jogador 4 (bottom-right)
)
# Base ring at x=256 (all 4 quadrants)
for a0, a1 in ROL_ARCS:
    draw.arc((260, 4, 316, 60), start=a0, end=a1, fill=255, width=8)
# Individual quadrants at x=320, 384, 448, 512
for idx, (a0, a1) in enumerate(ROL_ARCS):
    ox = 320 + idx * 64
    draw.arc((ox + 4, 4, ox + 60, 60), start=a0, end=a1, fill=255, width=9)

folder = Path(__file__).parent
glyphs = []
x, y, row_height = 0, 66, 0
points = (*range(32, 127), *range(160, 256))
for file, size in (("Lato-Regular.ttf", 20), ("Lato-Heavy.ttf", 24), ("Lato-Heavy.ttf", 48)):
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


def ribbon(x, y):
    center = 1.21 - 0.88 * x + 0.12 * sin(5 * x)
    broad = 0.62 * exp(-((y - center) / 0.22)**2)
    crest = 0.30 * exp(-((y - center + 0.10) / 0.018)**2)
    return (broad + crest) * x**1.3


add_mask((512, 512), (512, 288), ribbon)

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
