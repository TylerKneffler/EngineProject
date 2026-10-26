"""Bake the repeating 100 metre ruler used by TerrainManager.measurementGrid."""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


SIZE = 1024
SCALE = SIZE / 100
ROOT = Path(__file__).parent
image = Image.new("RGB", (SIZE, SIZE), (27, 39, 45))
draw = ImageDraw.Draw(image)


def font(size):
    for name in ("C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf"):
        if Path(name).exists():
            return ImageFont.truetype(name, size)
    return ImageFont.load_default()


# A metre is the smallest division. Every tenth line is brighter; the centre
# and repeat boundary stay legible when the ruler has been minified.
for metre in range(100):
    pixel = round(metre * SCALE)
    if metre % 50 == 0:
        color, width = (85, 196, 198), 5
    elif metre % 10 == 0:
        color, width = (63, 120, 135), 3
    else:
        color, width = (39, 59, 66), 1
    draw.line((pixel, 0, pixel, SIZE - 1), fill=color, width=width)
    draw.line((0, pixel, SIZE - 1, pixel), fill=color, width=width)

small = font(23)
large = font(29)
for metre in range(10, 100, 10):
    p = round(metre * SCALE)
    for x, y in ((p + 8, 9), (9, p + 8)):
        draw.rectangle((x - 4, y - 3, x + 70, y + 28), fill=(27, 39, 45))
        draw.text((x, y), f"{metre}m", font=small, fill=(193, 220, 216))
    radius = 5 if metre == 50 else 3
    for x, y in ((p, 0), (0, p)):
        draw.ellipse((x - radius, y - radius, x + radius, y + radius),
                     fill=(128, 227, 221))

draw.ellipse((SIZE // 2 - 9, SIZE // 2 - 9,
              SIZE // 2 + 9, SIZE // 2 + 9), fill=(237, 210, 111))
draw.rectangle((SIZE // 2 + 15, SIZE // 2 + 13,
                SIZE // 2 + 160, SIZE // 2 + 51), fill=(27, 39, 45))
draw.text((SIZE // 2 + 21, SIZE // 2 + 16), "50m", font=large,
          fill=(237, 210, 111))
image.save(ROOT / "measurement_grid.png", optimize=True)
