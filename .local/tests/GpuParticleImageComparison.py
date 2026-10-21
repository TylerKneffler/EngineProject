"""Compare rendered particle silhouettes after subtracting the empty scene."""

from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parent.parent / "artifacts"
NAMES = ("DX11", "DX12", "Vulkan")


def read_bmp(path):
    data = path.read_bytes()
    assert data[:2] == b"BM", path
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    assert (width, height) == (128, -128), path
    pixels = data[offset:]
    assert len(pixels) == width * -height * 4, path
    return pixels


def difference_mask(alive, empty):
    return {
        index // 4
        for index in range(0, len(alive), 4)
        if max(abs(alive[index + channel] - empty[index + channel])
               for channel in range(3)) > 8
    }


masks = {}
for name in NAMES:
    alive = read_bmp(ROOT / f"gpu-particles-{name}.bmp")
    empty = read_bmp(ROOT / f"gpu-particles-{name}-empty.bmp")
    mask = difference_mask(alive, empty)
    assert len(mask) > 25, f"{name}: no visible particle"
    masks[name] = mask
    xs = [index % 128 for index in mask]
    ys = [index // 128 for index in mask]
    print(f"{name}: {len(mask)} changed pixels, bounds "
          f"({min(xs)}, {min(ys)}) to ({max(xs)}, {max(ys)})")

for first, second in (("DX11", "DX12"), ("DX11", "Vulkan"),
                      ("DX12", "Vulkan")):
    a, b = masks[first], masks[second]
    overlap = len(a & b) / len(a | b)
    assert overlap > 0.7, f"{first}/{second}: particle silhouettes differ ({overlap:.3f})"
    print(f"{first}/{second}: silhouette overlap {overlap:.3f}")

try:
    from PIL import Image, ImageChops, ImageDraw
except ImportError:
    pass
else:
    sheet = Image.new("RGB", (3 * 256, 2 * 286), (20, 24, 30))
    draw = ImageDraw.Draw(sheet)
    for column, name in enumerate(NAMES):
        alive = Image.open(ROOT / f"gpu-particles-{name}.bmp").convert("RGB")
        empty = Image.open(ROOT / f"gpu-particles-{name}-empty.bmp").convert("RGB")
        difference = ImageChops.difference(alive, empty).point(
            lambda value: min(255, value * 8))
        x = column * 256
        sheet.paste(alive.resize((256, 256)), (x, 24))
        sheet.paste(difference.resize((256, 256)), (x, 310))
        draw.text((x + 8, 5), name + " rendered", fill="white")
        draw.text((x + 8, 291), name + " particle difference x8",
                  fill="white")
    sheet.save(ROOT / "gpu-particles-comparison.png")
