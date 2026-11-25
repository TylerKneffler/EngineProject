"""Turns terrain stamps (tools/bake_terrain_stamp.py) into terrain brushes.

A terrain brush (.brush) with a relief is what a stamp was to the editor's
terrain tools: its outline masks the brush and its heights are what Raise,
Lower and Stamp build up. This rasterizes a stamp's finest triangle level from
above (the highest surface over each sample, pinholes closed, the base set at
a low percentile of the outline's ring, the outline feathered), as the engine
did (TerrainBrushShape, until 2026-11-25), and writes next to the brush:
  <name>_tip.png     RGBA, white with the outline mask in alpha (the tip)
  <name>_relief.png  16-bit grayscale, 0 the base, 65535 the top
and <name>.brush (version 2) with a Relief section: heightRatio is the top's
height over the brush's half width, facing the stamp's outward direction
seen from above. Image rows run along +Z (row 0 at the tip's -Z edge).

Usage: python tools/stamp_brush.py [stamp ...] [--out DIR]
  Without stamps: every .stamp in Assets/TerrainData/Stamps. Default output:
  Assets/TerrainData/Brushes.
"""

import argparse
import struct
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
STAMPS = ASSETS / "TerrainData" / "Stamps"
BRUSHES = ASSETS / "TerrainData" / "Brushes"
STAMP_MAGIC = int.from_bytes(b"TRNRSTP1", "little")
RESOLUTION = 128


def read_stamp(path):
    """Points (N x 3 positions), direction and the finest triangle level."""
    data = Path(path).read_bytes()
    head = struct.Struct("<QIIIf2f3f3f3fI")
    values = head.unpack_from(data, 0)
    magic, version, width, height = values[:4]
    if magic != STAMP_MAGIC or version != 2:
        raise ValueError(f"{path}: not a version 2 stamp")
    direction = np.array(values[7:10])
    material_length = values[-1]
    at = head.size + material_length
    points = np.frombuffer(data, "<f4", width * height * 9, at).reshape(-1, 9)
    at += width * height * 9 * 4
    (levels,) = struct.unpack_from("<I", data, at)
    at += 4
    (triangles,) = struct.unpack_from("<I", data, at)
    at += 4
    finest = np.frombuffer(data, "<u4", triangles * 3, at).reshape(-1, 3)
    return points[:, 0:3].astype(np.float64), direction, finest


def rasterize(positions, direction, triangles, n=RESOLUTION):
    used = positions[np.unique(triangles)]
    low = used[:, [0, 2]].min(axis=0)
    high = used[:, [0, 2]].max(axis=0)
    centre = (low + high) * 0.5
    half = max(high[0] - low[0], high[1] - low[1]) * 0.5 * 1.04
    grid = np.stack([((positions[:, 0] - centre[0]) / half * 0.5 + 0.5) * (n - 1),
                     positions[:, 1],
                     ((positions[:, 2] - centre[1]) / half * 0.5 + 0.5) * (n - 1)], axis=1)
    lowest = -np.inf
    top = np.full((n, n), lowest)
    for tri in triangles:
        a, b, c = grid[tri[0]], grid[tri[1]], grid[tri[2]]
        area = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2])
        x0 = max(0, int(np.floor(min(a[0], b[0], c[0]))))
        x1 = min(n - 1, int(np.ceil(max(a[0], b[0], c[0]))))
        z0 = max(0, int(np.floor(min(a[2], b[2], c[2]))))
        z1 = min(n - 1, int(np.ceil(max(a[2], b[2], c[2]))))
        if abs(area) < 1e-6:
            # Faces seen edge-on (sheer walls): their corners still mark samples.
            for p in (a, b, c):
                x = int(np.clip(round(p[0]), 0, n - 1))
                z = int(np.clip(round(p[2]), 0, n - 1))
                top[z, x] = max(top[z, x], p[1])
            continue
        if x1 < x0 or z1 < z0:
            continue
        px, pz = np.meshgrid(np.arange(x0, x1 + 1, dtype=float), np.arange(z0, z1 + 1, dtype=float))
        w0 = ((b[0] - px) * (c[2] - pz) - (c[0] - px) * (b[2] - pz)) / area
        w1 = ((c[0] - px) * (a[2] - pz) - (a[0] - px) * (c[2] - pz)) / area
        w2 = 1.0 - w0 - w1
        inside = (w0 >= -0.02) & (w1 >= -0.02) & (w2 >= -0.02)
        y = w0 * a[1] + w1 * b[1] + w2 * c[1]
        window = top[z0:z1 + 1, x0:x1 + 1]
        np.maximum(window, np.where(inside, y, lowest), out=window)
    # Pinholes: an empty sample between covered ones takes their mean.
    for _ in range(2):
        filled = top.copy()
        for z in range(1, n - 1):
            for x in range(1, n - 1):
                if top[z, x] != lowest:
                    continue
                l, r, d, u = top[z, x - 1], top[z, x + 1], top[z - 1, x], top[z + 1, x]
                if l != lowest and r != lowest:
                    filled[z, x] = 0.5 * (l + r)
                elif d != lowest and u != lowest:
                    filled[z, x] = 0.5 * (d + u)
        top = filled
    covered = top != lowest
    # Base: a low percentile of the outline's ring.
    padded = np.pad(covered, 1, constant_values=False)
    border = covered & ~(padded[:-2, 1:-1] & padded[2:, 1:-1] & padded[1:-1, :-2] & padded[1:-1, 2:])
    rim = np.sort(top[border])
    base = rim[len(rim) // 10]
    # Feather: samples within a few steps of the outline fade out.
    feather = max(2, n // 40)
    inside = np.where(covered, feather, 0)
    for _ in range(feather):
        p = np.pad(inside, 1, constant_values=0)
        neighbour = np.minimum(np.minimum(p[:-2, 1:-1], p[2:, 1:-1]), np.minimum(p[1:-1, :-2], p[1:-1, 2:]))
        inside = np.where(inside > 0, np.minimum(inside, neighbour + 1), 0)
    t = np.clip(inside / feather, 0.0, 1.0)
    mask = np.where(covered, t * t * (3 - 2 * t), 0.0)
    heights = np.where(covered, np.maximum(top - base, 0.0), 0.0)
    facing = np.array([direction[0], direction[2]])
    facing = facing / np.linalg.norm(facing) if np.linalg.norm(facing) > 1e-3 else np.array([0.0, 1.0])
    return mask, heights, half, facing


def png(path, pixels, depth):
    """Grayscale (2D, 16-bit when depth == 16) or RGBA (3D, 8-bit) PNG."""
    rows, cols = pixels.shape[:2]
    if pixels.ndim == 3:
        color, raw_rows = 6, [b"\x00" + row.astype(np.uint8).tobytes() for row in pixels]
    else:
        color = 0
        raw_rows = [b"\x00" + row.astype(">u2" if depth == 16 else np.uint8).tobytes() for row in pixels]
    chunk = lambda tag, body: (struct.pack(">I", len(body)) + tag + body +
                               struct.pack(">I", zlib.crc32(tag + body)))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", cols, rows, depth, color, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(b"".join(raw_rows), 9)) + chunk(b"IEND", b""))


def assets_relative(path):
    return "Assets/" + Path(path).resolve().relative_to(ASSETS).as_posix()


def write_brush(name, mask, heights, half, facing, out=BRUSHES):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    top = float(heights.max())
    tip = np.zeros(mask.shape + (4,))
    tip[..., 0:3] = 255
    tip[..., 3] = np.round(mask * 255)
    tip_path, relief_path = out / f"{name}_tip.png", out / f"{name}_relief.png"
    png(tip_path, tip, 8)
    png(relief_path, np.round(heights / max(top, 1e-9) * 65535), 16)
    root = ET.Element("BrushPreset", {
        "version": "2", "name": name, "tip": assets_relative(tip_path), "shape": "0", "falloff": "2",
        "size": "128", "hardness": "1", "strength": "1", "opacity": "1", "flow": "1",
        "spacing": "0.08", "rotation": "0", "scatter": "0", "pressureSize": "0", "pressureStrength": "0"})
    ET.SubElement(root, "Relief", {
        "map": assets_relative(relief_path), "heightRatio": f"{top / half:.6g}",
        "facingX": f"{facing[0]:.6g}", "facingY": f"{facing[1]:.6g}", "backed": "true"})
    ET.SubElement(root, "Terrain", {"tool": "sculpt"})
    ET.indent(root)
    path = out / f"{name}.brush"
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)
    return path, top / half


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("stamps", nargs="*")
    parser.add_argument("--out", default=str(BRUSHES))
    args = parser.parse_args()
    stamps = [Path(s) for s in args.stamps] or sorted(STAMPS.glob("*.stamp"))
    for stamp in stamps:
        positions, direction, triangles = read_stamp(stamp)
        mask, heights, half, facing = rasterize(positions, direction, triangles)
        path, ratio = write_brush(stamp.stem, mask, heights, half, facing, args.out)
        print(f"{path.name}: {int((mask > 0.5).sum())} samples inside, height ratio {ratio:.3f}, "
              f"facing ({facing[0]:.2f}, {facing[1]:.2f})")


if __name__ == "__main__":
    main()
