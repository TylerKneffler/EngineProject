"""A seamless rock material cut from a cliff scan's own photos (tools/cliff_sculpt.py).

Sculpted cells (tools/cliff_sculpt.py) project this onto their rock from three
sides in the terrain shader, so it must tile. It is cut from the scan's colour,
normal and roughness atlases where one UV island covers a square, at the
scan's own texel density, and made seamless by cross-fading with a copy of
itself shifted by half a tile (whose edges meet). The repeat in metres
(at the pieces' typical placed scale) goes into the material's terrainUvScale
as 1 / repeat.

  python tools/cliff_rock_texture.py
"""

import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

import bake_terrain_stamp
from woodland_assets import ASSETS, read_native_mesh

SCAN = "Mountainside_01"
TILE = 512             # texels per side (of the 1024 px stamp images)
PLACED_SCALE = 2.5     # the pieces' typical scale (base 2.2 x 0.75-1.35)
OUTPUT = ASSETS / "TerrainData" / "Sculpts"
QUALITY = 92


def coverage(triangles, uv, size):
    """Atlas texels inside some UV triangle."""
    covered = np.zeros((size, size), bool)
    pixels = uv * size - 0.5
    for tri in triangles:
        xs, ys = pixels[tri, 0], pixels[tri, 1]
        lo_x, hi_x = max(int(np.ceil(xs.min())), 0), min(int(np.floor(xs.max())), size - 1)
        lo_y, hi_y = max(int(np.ceil(ys.min())), 0), min(int(np.floor(ys.max())), size - 1)
        if hi_x < lo_x or hi_y < lo_y:
            continue
        det = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(det) < 1e-12:
            continue
        gx, gy = np.meshgrid(np.arange(lo_x, hi_x + 1), np.arange(lo_y, hi_y + 1))
        l0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / det
        l1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / det
        inside = (l0 >= -0.01) & (l1 >= -0.01) & (1 - l0 - l1 >= -0.01)
        covered[gy[inside], gx[inside]] = True
    return covered


def seamless(image):
    """Cross-fades the tile with itself shifted by half (whose edges meet):
    the shifted copy at the borders, the original in the middle."""
    size = image.shape[0]
    shifted = np.roll(image, (size // 2, size // 2), axis=(0, 1))
    ramp = np.minimum(np.arange(size), size - 1 - np.arange(size)) / (size * 0.25)
    ramp = np.clip(ramp, 0.0, 1.0)
    ramp = ramp * ramp * (3 - 2 * ramp)
    mask = np.minimum.outer(ramp, ramp)[..., None]
    return image * mask + shifted * (1 - mask)


def read_stamp_depth(name):
    """The stamp's grid (tools/bake_terrain_stamp.py): depth along its
    direction per point, its weight, and its metres per point."""
    import struct
    raw = (bake_terrain_stamp.OUTPUT / f"{name}.stamp").read_bytes()
    head = struct.unpack_from("<QIIIf2f9fI", raw, 0)
    width, height, spacing = head[2], head[3], head[4]
    direction = np.array(head[7:10])
    at = struct.calcsize("<QIIIf2f9fI") + head[-1]
    points = np.frombuffer(raw, "<f4", width * height * 9, at).reshape(height, width, 9).astype(np.float64)
    return points[..., :3] @ direction, points[..., 8], spacing


def main():
    # The stamp's images: the scan seen straight on as one picture (no UV
    # islands), normals already in its own axes (out of the picture = +z).
    load = lambda key: np.asarray(Image.open(bake_terrain_stamp.OUTPUT / f"{SCAN}_{key}.jpg").convert("RGB"),
                                  np.float32) / 255.0
    albedo, normal, rough = load("albedo"), load("normal"), load("rough")
    size = albedo.shape[0]
    depth, weight, spacing = read_stamp_depth(SCAN)
    # Where the face turns away from the view, the picture stretches: pick
    # the square with the least steep depth (and wholly on the scan).
    slope = np.hypot(*np.gradient(depth, spacing))
    bad = ((slope > 0.8) | (weight < 0.99)).astype(np.float64)
    bad = ndimage.zoom(bad, (size / bad.shape[0], size / bad.shape[1]), order=1)
    texel = (depth.shape[1] - 1) * spacing / size * PLACED_SCALE
    half = TILE // 2
    score = ndimage.uniform_filter(bad, TILE, mode="constant", cval=1.0)
    score[:half, :] = score[-half:, :] = 1.0
    score[:, :half] = score[:, -half:] = 1.0
    cy, cx = np.unravel_index(np.argmin(score), score.shape)
    covered = 1.0 - score[cy, cx]
    crop = lambda image: image[cy - half:cy + half, cx - half:cx + half]
    # The stamp images are already tinted (sRGB); work in linear light.
    linear = np.where(crop(albedo) <= 0.04045, crop(albedo) / 12.92, ((crop(albedo) + 0.055) / 1.055) ** 2.4)
    tile_albedo = seamless(linear)
    tile_normal = seamless(crop(normal) * 2.0 - 1.0)
    tile_normal /= np.maximum(np.linalg.norm(tile_normal, axis=-1, keepdims=True), 1e-6)
    tile_rough = seamless(crop(rough)[..., 1:2])[..., 0]
    srgb = np.where(tile_albedo <= 0.0031308, tile_albedo * 12.92,
                    1.055 * np.power(np.clip(tile_albedo, 0.0, None), 1 / 2.4) - 0.055)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    images = {
        "albedo": srgb,
        "normal": tile_normal * 0.5 + 0.5,
        "rough": np.stack([np.zeros_like(tile_rough), tile_rough, np.ones_like(tile_rough)], -1),
    }
    paths = {}
    for key, value in images.items():
        path = OUTPUT / f"cliff_rock_{key}.jpg"
        Image.fromarray((np.clip(value, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)).save(path, quality=QUALITY)
        paths[key] = "Assets/" + path.resolve().relative_to(Path(ASSETS).resolve()).as_posix()
    repeat = TILE * texel
    mean = tile_albedo.reshape(-1, 3).mean(0)
    (OUTPUT / "cliff_rock.material").write_text(json.dumps({
        "version": 1, "baseColor": [1.0, 1.0, 1.0], "metallic": 0, "roughness": 1.0, "normalScale": 1.0,
        "terrainUvScale": round(1.0 / repeat, 6),
        "terrainMacroColor": [round(float(v), 5) for v in mean],
        "baseColorTexture": paths["albedo"], "normalTexture": paths["normal"],
        "metallicRoughnessTexture": paths["rough"],
    }, indent=4) + "\n", encoding="utf-8")
    print(f"{SCAN}: tile at ({cx}, {cy}), {covered * 100:.0f}% clean, {repeat:.1f} m per repeat, "
          f"mean {np.round(mean, 3)}")
    return mean


if __name__ == "__main__":
    main()
