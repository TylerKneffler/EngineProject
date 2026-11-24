#!/usr/bin/env python3
"""Bake hemi-octahedral impostors for the woodland trees (.md/todo.md Foliage Phase 5).

Replaces each tree's far level (the 6-view + top card impostor) with an
octahedral one: GRID x GRID orthographic views over the upper hemisphere,
each FRAME pixels square, in one colour/alpha atlas and one normal atlas.
The views are the tree as its source levels draw it (the prefab's LOD1
parts, or LOD0 where a part has no LOD1, with their own textures), with:
  * normals: the surfaces' normals, rounded toward the crown (as baked
    normals usually are, which foliage wants), in each view's tangent frame;
  * occlusion: darker deep inside the crown and low down, so a far tree has
    the dark interior its near levels get from the sun's shadow and the
    canopy sky term, instead of a flat front-lit card.

View (i, j) looks from the direction that decodes from (i, j) / (GRID - 1)
(hemi-octahedral: u' = u - 0.5, v' = v - 0.5, x = u' + v', z = u' - v',
y = 1 - |x| - |z|); its camera basis is forward = that direction,
right = normalize(cross(up0, forward)) with up0 = +Y (or -Z for the top
view), up = cross(forward, right). Object.hlsl (VSMain, octahedral impostor)
decodes the same way: keep the two in step.

The impostor mesh is three quads (6 triangles). Vertex data the shader reads:
  uv            the quad corner, 0..1 (v down)
  uvChannels[2] crown centre (object space) and radius
  uvChannels[3] GRID, quad index (0-2)
Positions only give the mesh its bounds (a cube round the crown); the vertex
shader places the quads. The material keeps impostorBillboard; the prefab,
mesh and material paths are unchanged (overwritten in place).

Writes, per tree:
  <the prefab's impostor mesh>                   (overwritten)
  <the prefab's impostor material>               (textures, normal scale)
  Models/Woodland/<Family>/Textures/<Tree>_Octa_{Color,Normal}.ktx2
Sources and previews go to .local/woodland-gen/octahedral.

Usage: python tools/bake_octahedral_impostors.py [--only Name] [--grid 8] [--frame 128]
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402

NS = "{urn:engine-components}"
PREFABS = wa.ASSETS / "Prefabs" / "Woodland"
FAMILIES = ("Oaks", "Conifers", "Broadleaf")
WORK = wa.WORK / "octahedral"
CUTOFF = 0.4
SUPERSAMPLE = 2
TEXTURE_MAX = 1024
_textures: dict = {}


def load_texture(asset: str) -> wa.Texture | None:
    """A material texture as linear RGBA (sRGB colour textures)."""
    if not asset:
        return None
    if asset in _textures:
        return _textures[asset]
    path = wa.ASSETS / asset.removeprefix("Assets/")
    png = path.with_suffix(".png")
    if png.exists():
        image = wa.read_png(png)
        factor = 1
        while image.shape[0] // factor > TEXTURE_MAX:
            factor *= 2
        if factor > 1:
            a = image[..., 3:4]
            rgb = wa.downsample(image[..., :3] * a, factor) / np.maximum(wa.downsample(a, factor), 1e-6)
            image = np.concatenate([rgb, wa.downsample(a, factor)], axis=2)
    else:
        # Cooked KTX2 without its PNG source: TextureCook --decode.
        tool = wa.find_texture_cook()
        if tool is None:
            raise RuntimeError("TextureCook is needed to read " + asset)
        WORK.mkdir(parents=True, exist_ok=True)
        raw_path = WORK / (path.stem + ".rgba")
        subprocess.run([str(tool), "--decode", str(path), str(raw_path), str(TEXTURE_MAX)], check=True)
        raw = raw_path.read_bytes()
        assert raw[:4] == b"RGBA"
        width, height = struct.unpack_from("<II", raw, 4)
        image = np.frombuffer(raw, dtype=np.uint8, offset=12).reshape(height, width, 4) / 255.0
    image = np.array(image, dtype=np.float64)
    image[..., :3] = wa.srgb_to_linear(image[..., :3])
    texture = wa.Texture(image)
    _textures[asset] = texture
    return texture


def prefab_parts(prefab: Path):
    """(mesh element, material element) per part of a prefab."""
    root = ET.parse(prefab).getroot()
    for components in root.iter("components"):
        mesh = components.find(NS + "Mesh")
        material = components.find(NS + "Material")
        if mesh is not None and material is not None:
            yield mesh, material


def tree_surfaces(prefab: Path, level: int = 1):
    """The tree at a level (per part: that level, else the base) and the
    prefab's impostor mesh and material paths."""
    surfaces, impostor_mesh, impostor_material = [], None, None
    for mesh_el, material_el in prefab_parts(prefab):
        lods = [item.text or "" for item in mesh_el.findall("Mesh.lodMeshes/item")]
        lod_materials = [item.text or "" for item in material_el.findall("Material.lodMaterials/item")]
        for index, entry in enumerate(lods):
            if "Impostor" in entry:
                impostor_mesh = entry
                impostor_material = lod_materials[index] if index < len(lod_materials) else ""
        if len(lods) >= level:
            mesh_asset = lods[level - 1]
            material_asset = (lod_materials[level - 1] if level - 1 < len(lod_materials) else "") or \
                material_el.get("Material.materialAsset")
        else:
            mesh_asset = mesh_el.get("Mesh.file")
            material_asset = material_el.get("Material.materialAsset")
        if not mesh_asset or "Impostor" in mesh_asset:
            continue  # hidden at this level
        material = json.loads((wa.ROOT / material_asset).read_text(encoding="utf-8"))
        p, n, uv, c, idx = wa.read_native_mesh(wa.ASSETS / mesh_asset.removeprefix("Assets/"))
        masked = material.get("alphaMode") == "Mask"
        surfaces.append(wa.Surface(p, n, uv, c, idx, texture=load_texture(material.get("baseColorTexture", "")),
                                   base_color=material.get("baseColor", [1, 1, 1])[:3],
                                   alpha_cutoff=material.get("alphaCutoff", 0.5) if masked else 0.0,
                                   use_vertex_colors=bool(material.get("useVertexColors", False))))
    return surfaces, impostor_mesh, impostor_material


def view_basis(i: int, j: int, grid: int):
    """forward, right, up of view (i, j): as Object.hlsl decodes it."""
    u, v = i / (grid - 1) - 0.5, j / (grid - 1) - 0.5
    x, z = u + v, u - v
    forward = np.array([x, max(1.0 - abs(x) - abs(z), 0.0), z])
    forward /= np.linalg.norm(forward)
    up0 = np.array([0.0, 0.0, -1.0]) if forward[1] > 0.999 else np.array([0.0, 1.0, 0.0])
    right = np.cross(up0, forward)
    right /= np.linalg.norm(right)
    up = np.cross(forward, right)
    return forward, right, up


def bake(prefab: Path, grid: int, frame: int):
    name = prefab.stem
    surfaces, impostor_mesh, impostor_material = tree_surfaces(prefab)
    if not surfaces or not impostor_mesh or not impostor_material:
        print(f"{name}: no source levels or no impostor level; skipped")
        return
    points = np.concatenate([s.p for s in surfaces if len(s.p)])
    lo, hi = points.min(axis=0), points.max(axis=0)
    center = (lo + hi) * 0.5
    radius = float(np.max(np.linalg.norm(points - center, axis=1))) * 1.02
    # Crown ellipsoid for the occlusion term: the foliage-height part of the
    # bounds (the trunk below the crown is shaded by its height term).
    half = np.maximum((hi - lo) * 0.5, 1e-3)
    size = frame * SUPERSAMPLE
    scale = size / (2.0 * radius)
    atlas = grid * frame
    color = np.zeros((atlas, atlas, 3))
    alpha = np.zeros((atlas, atlas))
    normals = np.zeros((atlas, atlas, 3))
    for j in range(grid):
        for i in range(grid):
            forward, right, up = view_basis(i, j, grid)
            origin = (float(center @ right) - radius, float(center @ up) - radius)
            albedo, a, n, depth = wa.rasterize(surfaces, right, up, forward, origin, scale, (size, size))
            # Row 0 of the rasterizer is the lowest; images keep row 0 at the top.
            albedo, a, n, depth = albedo[::-1], a[::-1], n[::-1], depth[::-1]
            ys, xs = np.mgrid[0:size, 0:size]
            px = (xs + 0.5) / scale + origin[0]
            py = (size - ys - 0.5) / scale + origin[1]
            d = np.where(np.isfinite(depth), depth, 0.0)
            position = (right[None, None] * px[..., None] + up[None, None] * py[..., None] +
                        forward[None, None] * d[..., None])
            relative = (position - center) / half
            depth_in_crown = np.linalg.norm(relative, axis=-1)
            height = np.clip((position[..., 1] - lo[1]) / max(hi[1] - lo[1], 1e-3), 0, 1)
            occlusion = (0.42 + 0.58 * np.clip((depth_in_crown - 0.25) / 0.7, 0, 1) ** 0.8) * \
                (0.72 + 0.28 * height)
            rounded = relative / np.maximum(depth_in_crown[..., None], 1e-6)
            n = wa.normalize(n * 0.55 + rounded * 0.45)
            # Downsample (alpha-weighted colour and normals).
            a_small = wa.downsample(a, SUPERSAMPLE)
            weight = np.maximum(a_small, 1e-6)[..., None]
            c_small = wa.downsample(albedo * (occlusion * a)[..., None], SUPERSAMPLE) / weight
            n_small = wa.normalize(wa.downsample(n * a[..., None], SUPERSAMPLE) + 1e-9)
            # Tangent frame of the quad: T = right, B = cross(N, T) * w = -up
            # (texture v grows downward), N = forward.
            ts = np.stack([n_small @ right, -(n_small @ up), n_small @ forward], axis=-1)
            ts[..., 2] = np.maximum(ts[..., 2], 0.05)
            ts = wa.normalize(ts)
            y0, x0 = j * frame, i * frame
            color[y0:y0 + frame, x0:x0 + frame] = c_small
            alpha[y0:y0 + frame, x0:x0 + frame] = a_small
            normals[y0:y0 + frame, x0:x0 + frame] = ts
        print(f"  {name}: row {j + 1}/{grid}", flush=True)
    filled = wa.push_pull_fill(color, alpha)
    nfill = wa.normalize(wa.push_pull_fill(normals, alpha) + np.array([0, 0, 1e-6]))
    WORK.mkdir(parents=True, exist_ok=True)
    color_png = WORK / f"{name}_Octa_Color.png"
    normal_png = WORK / f"{name}_Octa_Normal.png"
    wa.write_png(color_png, np.concatenate([wa.linear_to_srgb(filled), alpha[..., None]], axis=2))
    wa.write_png(normal_png, nfill * 0.5 + 0.5)
    family = prefab.parent.name
    textures = wa.ASSETS / "Models" / "Woodland" / family / "Textures"
    color_asset = wa.cook(color_png, textures / f"{name}_Octa_Color.ktx2", "color", alpha_cutoff=CUTOFF)
    normal_asset = wa.cook(normal_png, textures / f"{name}_Octa_Normal.ktx2", "normal")
    write_mesh(wa.ASSETS / impostor_mesh.removeprefix("Assets/"), center, radius, grid)
    material_path = wa.ROOT / impostor_material
    material = json.loads(material_path.read_text(encoding="utf-8"))
    material.update(baseColorTexture=wa.asset_path(color_asset), normalTexture=wa.asset_path(normal_asset),
                    baseColor=[1, 1, 1], useVertexColors=False, alphaMode="Mask", alphaCutoff=CUTOFF,
                    normalScale=1.0, doubleSided=True, impostorBillboard=True)
    material_path.write_text(json.dumps(material, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{name}: {grid}x{grid} views of {frame} px, radius {radius:.2f} m")


def write_mesh(path: Path, center, radius: float, grid: int):
    """Three quads; positions span a cube round the crown (bounds only)."""
    corners = ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0))
    planes = ((0, 1), (2, 1), (0, 2))  # XY, ZY, XZ: together the full cube
    rows, indices = [], []
    for quad, (a, b) in enumerate(planes):
        base = len(rows)
        for u, v in corners:
            vertex = np.zeros(wa.VERTEX_FLOATS, dtype=np.float32)
            position = np.array(center, dtype=np.float64)
            position[a] += (u * 2 - 1) * radius
            position[b] += (1 - v * 2) * radius
            vertex[0:3] = position
            vertex[3:6] = (0, 0, 1)
            vertex[6:8] = (u, v)
            vertex[8:12] = (1, 0, 0, -1)
            vertex[12:14] = (u, v)
            vertex[14:18] = (1, 1, 1, 1)
            vertex[34:38] = (u, v, 0, 0)               # uvChannels[0]
            vertex[38:42] = (u, v, 0, 0)               # uvChannels[1]
            vertex[42:46] = (*center, radius)          # uvChannels[2]
            vertex[46:50] = (grid, quad, 0, 0)         # uvChannels[3]
            rows.append(vertex)
        # Counter-clockwise seen from the front (as MeshBuilder.quad): corners
        # run top-left, top-right, bottom-right, bottom-left.
        indices += [base, base + 3, base + 2, base, base + 2, base + 1]
    data = np.stack(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack("<4I", wa.NATIVE_MESH_MAGIC, wa.NATIVE_MESH_VERSION, len(data), len(indices)))
        f.write(data.tobytes())
        f.write(np.asarray(indices, dtype=np.uint32).tobytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", help="one tree (prefab name)")
    parser.add_argument("--grid", type=int, default=8)
    parser.add_argument("--frame", type=int, default=128)
    args = parser.parse_args()
    prefabs = [p for family in FAMILIES for p in sorted((PREFABS / family).glob("*.prefab"))]
    if args.only:
        prefabs = [p for p in prefabs if p.stem == args.only]
    for prefab in prefabs:
        bake(prefab, args.grid, args.frame)


if __name__ == "__main__":
    main()
