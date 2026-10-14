"""Regenerate the self-authored calibration texture atlases (requires Pillow)."""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).parent
CELL = 256
FONT = ImageFont.load_default()


def tile(kind):
    image = Image.new("RGB", (CELL, CELL), "#273445")
    draw = ImageDraw.Draw(image)
    if kind == "checker":
        for y in range(8):
            for x in range(8):
                color = "#3c5366" if (x + y) % 2 else "#75909b"
                draw.rectangle((x * 32, y * 32, x * 32 + 31, y * 32 + 31), fill=color)
        for n in range(0, CELL, 32):
            draw.line((n, 0, n, 255), fill="#a8c0c3", width=1)
            draw.line((0, n, 255, n), fill="#a8c0c3", width=1)
        draw.line((128, 0, 128, 255), fill="#e3cb83", width=3)
        draw.line((0, 128, 255, 128), fill="#e3cb83", width=3)
    elif kind == "ruler":
        for n in range(0, CELL, 8):
            major = n % 64 == 0
            draw.line((n, 0, n, 255), fill="#8fb8c3" if major else "#426174", width=3 if major else 1)
            draw.line((0, n, 255, n), fill="#8fb8c3" if major else "#426174", width=3 if major else 1)
        for y in range(4):
            for x in range(4):
                draw.text((x * 64 + 5, y * 64 + 5), f"{x},{y}", fill="#dce8e3", font=FONT)
    else:
        draw.rectangle((0, 0, 255, 255), fill="#303c49")
        for n in range(-256, 512, 32):
            draw.line((n, 0, n + 256, 255), fill="#566c78", width=3)
        for y in (48, 128, 208):
            for x in (48, 128, 208):
                draw.polygon(((x, y - 22), (x + 19, y + 9), (x + 6, y + 9),
                              (x + 6, y + 22), (x - 6, y + 22), (x - 6, y + 9),
                              (x - 19, y + 9)), fill="#d9a969")
        draw.line((0, 0, 255, 0), fill="#d9a969", width=4)
        draw.line((0, 0, 0, 255), fill="#8fb8c3", width=4)
    return image


for name in ("checker", "ruler", "direction"):
    face = tile(name)
    atlas = Image.new("RGB", (CELL * 3, CELL * 2))
    for row in range(2):
        for col in range(3):
            atlas.paste(face, (col * CELL, row * CELL))
    atlas.save(ROOT / f"{name}.png", optimize=True)
