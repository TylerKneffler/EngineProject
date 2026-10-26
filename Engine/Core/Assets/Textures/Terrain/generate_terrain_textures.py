"""Generate deterministic, tileable color textures for the terrain demo."""

from pathlib import Path
import math
import random
from PIL import Image

SIZE = 512
OUTPUT = Path(__file__).parent


def lattice(seed, frequency):
    rng = random.Random(seed * 997 + frequency)
    return [[rng.uniform(-1, 1) for _ in range(frequency)]
            for _ in range(frequency)]


def sample(grid, x, y):
    frequency = len(grid)
    fx, fy = x * frequency / SIZE, y * frequency / SIZE
    ix, iy = math.floor(fx), math.floor(fy)
    tx, ty = fx - ix, fy - iy
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a = grid[iy % frequency][ix % frequency]
    b = grid[iy % frequency][(ix + 1) % frequency]
    c = grid[(iy + 1) % frequency][ix % frequency]
    d = grid[(iy + 1) % frequency][(ix + 1) % frequency]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def make(name, base, variation, seed, grass=False):
    broad = lattice(seed, 18)
    medium = lattice(seed + 1, 55)
    fine = lattice(seed + 2, 151)
    image = Image.new("RGB", (SIZE, SIZE))
    pixels = image.load()
    for y in range(SIZE):
        for x in range(SIZE):
            noise = (0.54 * sample(broad, x, y) +
                     0.31 * sample(medium, x, y) +
                     0.15 * sample(fine, x, y))
            fleck = ((x * 73856093 ^ y * 19349663 ^ seed * 83492791) & 255)
            detail = (fleck - 127) / 255
            if grass:
                # Sparse pale blades break up the green without directional seams.
                blade = 0.15 if fleck > 245 else 0.0
            else:
                # Small light stones and dark organic flecks.
                blade = 0.16 if fleck > 250 else (-0.10 if fleck < 8 else 0.0)
            pixels[x, y] = tuple(max(0, min(255, round(
                channel + variation * noise + 20 * detail + 255 * blade)))
                for channel in base)
    image.save(OUTPUT / f"{name}.png", optimize=True)


make("dark_soil", (66, 57, 43), 38, 11)
make("warm_dirt", (119, 92, 62), 43, 23)
make("meadow_grass", (70, 111, 47), 46, 37, grass=True)
make("highland_grass", (126, 135, 74), 37, 53, grass=True)
make("valley_grass", (58, 105, 44), 40, 67, grass=True)
make("field_grass", (100, 127, 56), 39, 79, grass=True)
