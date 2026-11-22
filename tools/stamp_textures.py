"""Bakes a terrain stamp's surface textures from its scan (tools/bake_terrain_stamp.py).

The scans' own textures are photogrammetry atlases: many UV islands. A stamp's
triangles span several stamp points and would cross island seams, so the
stamp cannot draw the scan through per-point UVs. Instead the scan is
rendered straight on along the stamp's direction into images covering the
stamp grid (stamp texcoord s, t in 0..1 across its u and v extent), the
outermost surface winning per pixel, as the grid itself was baked:
  <name>_albedo.jpg   the scan's base colour (sRGB)
  <name>_normal.jpg   the scan's normal (its mesh normal bent by its normal
                      map), expressed in the stamp's own axes: r along u,
                      g along v, b along the outward direction (0..1)
  <name>_rough.jpg    roughness in the green channel (glTF)
and a <name>.material naming them, which the terrain packs as an extra layer
array slice (TerrainLayerArrays) for the stamp's points to sample.

Requires Pillow (JPEG decoding and encoding).
"""

import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

from woodland_assets import ASSETS, read_native_mesh

SIZE = 1024
QUALITY = 92


def _load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def _bilinear(image, uv):
    """Samples an (h, w, c) image at uv (n, 2), wrapping, v down the image."""
    h, w = image.shape[:2]
    x = (uv[:, 0] % 1.0) * w - 0.5
    y = (uv[:, 1] % 1.0) * h - 0.5
    x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    x0, y0 = x0 % w, y0 % h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    return ((image[y0, x0] * (1 - fx) + image[y0, x1] * fx) * (1 - fy) +
            (image[y1, x0] * (1 - fx) + image[y1, x1] * fx) * fy)


def bake_textures(stamp, mesh_path, material, output, size=SIZE):
    """Writes the stamp's images and material into `output`; returns the
    material's engine path (Assets/...)."""
    positions, normals, texcoords, _, triangles = read_native_mesh(mesh_path)
    height, width = stamp.weight.shape
    span_u = (width - 1) * stamp.spacing
    span_v = (height - 1) * stamp.spacing
    # Pixel (row, col) centre in stamp texcoords and stamp-plane u, v.
    s = (positions @ stamp.axis_u - stamp.origin[0]) / span_u * size - 0.5
    t = (positions @ stamp.axis_v - stamp.origin[1]) / span_v * size - 0.5
    w = positions @ stamp.direction
    depth = np.full((size, size), -np.inf)
    hit_triangle = np.full((size, size), -1, np.int64)
    hit_bary = np.zeros((size, size, 3))
    for index, tri in enumerate(triangles):
        xs, ys = s[tri], t[tri]
        lo_x, hi_x = max(int(np.floor(xs.min())), 0), min(int(np.ceil(xs.max())), size - 1)
        lo_y, hi_y = max(int(np.floor(ys.min())), 0), min(int(np.ceil(ys.max())), size - 1)
        if hi_x < lo_x or hi_y < lo_y:
            continue
        gx, gy = np.meshgrid(np.arange(lo_x, hi_x + 1), np.arange(lo_y, hi_y + 1))
        det = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(det) < 1e-12:
            continue
        l0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / det
        l1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / det
        l2 = 1.0 - l0 - l1
        inside = (l0 >= -1e-4) & (l1 >= -1e-4) & (l2 >= -1e-4)
        if not inside.any():
            continue
        gx, gy, l0, l1, l2 = gx[inside], gy[inside], l0[inside], l1[inside], l2[inside]
        z = l0 * w[tri[0]] + l1 * w[tri[1]] + l2 * w[tri[2]]
        better = z > depth[gy, gx]
        gx, gy = gx[better], gy[better]
        depth[gy, gx] = z[better]
        hit_triangle[gy, gx] = index
        hit_bary[gy, gx] = np.stack([l0[better], l1[better], l2[better]], 1)
    covered = hit_triangle >= 0
    # Uncovered pixels take their nearest covered pixel's hit.
    near = ndimage.distance_transform_edt(~covered, return_indices=True)[1]
    hit_triangle = hit_triangle[near[0], near[1]]
    hit_bary = hit_bary[near[0], near[1]]
    tri = triangles[hit_triangle.ravel()]
    bary = hit_bary.reshape(-1, 3)[..., None]
    uv = (texcoords[tri] * bary).sum(1)
    smooth = (normals[tri] * bary).sum(1)
    smooth /= np.maximum(np.linalg.norm(smooth, axis=1, keepdims=True), 1e-9)
    # Per-triangle tangent frame from its UVs: T along +u, B along +v (down the
    # image); an OpenGL normal map's green points up the image, along -B.
    p0, p1, p2 = positions[tri[:, 0]], positions[tri[:, 1]], positions[tri[:, 2]]
    t0, t1, t2 = texcoords[tri[:, 0]], texcoords[tri[:, 1]], texcoords[tri[:, 2]]
    e1, e2 = p1 - p0, p2 - p0
    d1, d2 = t1 - t0, t2 - t0
    r = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(r) < 1e-12, 1e-12, r)[:, None]
    tangent = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) / r
    bitangent = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) / r
    tangent -= smooth * (tangent * smooth).sum(1, keepdims=True)
    tangent /= np.maximum(np.linalg.norm(tangent, axis=1, keepdims=True), 1e-9)
    bitangent -= smooth * (bitangent * smooth).sum(1, keepdims=True)
    bitangent -= tangent * (bitangent * tangent).sum(1, keepdims=True)
    bitangent /= np.maximum(np.linalg.norm(bitangent, axis=1, keepdims=True), 1e-9)
    textures = Path(ASSETS) / "Models" / "Woodland" / "Scans" / "Textures"
    material_json = json.loads((Path(ASSETS).parent.parent.parent / material).read_text())
    resolve = lambda engine_path: Path(ASSETS) / engine_path.removeprefix("Assets/")
    albedo = _bilinear(_load(resolve(material_json["baseColorTexture"])), uv)
    tangent_normal = _bilinear(_load(resolve(material_json["normalTexture"])), uv) * 2.0 - 1.0
    roughness = _bilinear(_load(resolve(material_json["metallicRoughnessTexture"])), uv)[:, 1]
    bent = (tangent * tangent_normal[:, 0:1] - bitangent * tangent_normal[:, 1:2] +
            smooth * tangent_normal[:, 2:3])
    bent /= np.maximum(np.linalg.norm(bent, axis=1, keepdims=True), 1e-9)
    in_stamp = np.stack([bent @ stamp.axis_u, bent @ stamp.axis_v, bent @ stamp.direction], 1)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    # The scan's material tints its texture in linear light, as the engine
    # does (multiplying the stored sRGB values by it darkened red by half).
    tint = np.asarray(material_json.get("baseColor", [1.0, 1.0, 1.0]), np.float32)
    linear = np.where(albedo <= 0.04045, albedo / 12.92, ((albedo + 0.055) / 1.055) ** 2.4) * tint
    tinted = np.where(linear <= 0.0031308, linear * 12.92, 1.055 * np.power(np.clip(linear, 0.0, None), 1 / 2.4) - 0.055)
    images = {
        "albedo": np.clip(tinted, 0.0, 1.0),
        "normal": np.clip(in_stamp * 0.5 + 0.5, 0.0, 1.0),
        "rough": np.stack([np.zeros_like(roughness), roughness, np.zeros_like(roughness)], 1),
    }
    paths = {}
    for key, values in images.items():
        pixels = (values.reshape(size, size, 3) * 255.0 + 0.5).astype(np.uint8)
        path = output / f"{stamp.name}_{key}.jpg"
        Image.fromarray(pixels).save(path, quality=QUALITY)
        paths[key] = "Assets/" + path.resolve().relative_to(Path(ASSETS).resolve()).as_posix()
    material_path = output / f"{stamp.name}.material"
    # The linear mean over the scan's own pixels: what the far terrain's macro
    # colour map paints where the stamp stands (LandscapeDriver), as the paint
    # layers' terrainMacroColor does for them.
    macro = np.clip(linear.reshape(size, size, 3)[covered].mean(axis=0), 0.0, 1.0)
    material_path.write_text(json.dumps({
        "version": 1,
        "baseColor": [1.0, 1.0, 1.0],
        "metallic": 0,
        "roughness": 1.0,
        "normalScale": 1.0,
        "terrainUvScale": 1.0,
        "terrainMacroColor": [round(float(v), 5) for v in macro],
        "baseColorTexture": paths["albedo"],
        "normalTexture": paths["normal"],
        "metallicRoughnessTexture": paths["rough"],
    }, indent=4) + "\n", encoding="utf-8")
    return "Assets/" + material_path.resolve().relative_to(Path(ASSETS).resolve()).as_posix(), float(covered.mean())
