#!/usr/bin/env python3
"""Give every tree level the silhouette coverage of its LOD0 (fair visibility).

A level that lets more through than LOD0 lets a distant viewer see a player a
near viewer cannot, and the reverse. Measured 2026-11-24: LOD1 covered
88-97 % of LOD0 for the oaks and 104-110 % for the tall spruce, LOD2 down to
55 %. Lowering the alpha cutoff to make up for it was tried and dropped: it
took the cutoff to a seventh, turning leaf cards into solid rectangles. The
gap is in the geometry (fewer, smaller cards), so this scales the cards.

For each tree prefab (Oaks, Conifers, Broadleaf) and each mesh level past
LOD0 (LOD1, LOD2), the alpha-masked parts' leaf cards (connected pieces of
the mesh, 2-11 triangles each) are scaled about their own centres by the
factor that makes the level's covered area (orthographic views from four
sides and above, the same scale for every level) equal LOD0's. The mesh is
rewritten in place (positions only); the original is kept once under
.local/woodland-gen/lod_coverage_src and every run starts from it.
Opaque parts (trunks, branches) and meshes LOD0 shares are left alone. The
impostor matches LOD0 through its bake (bake_octahedral_impostors.py).

Usage: python tools/match_lod_coverage.py [--only Name]
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402
import bake_octahedral_impostors as bo  # noqa: E402

PREFABS = wa.ASSETS / "Prefabs" / "Woodland"
FAMILIES = ("Oaks", "Conifers", "Broadleaf")
SOURCES = wa.WORK / "lod_coverage_src"
SCALE = 8.0  # pixels per metre for the coverage renders


def views():
    out = []
    for k in range(4):
        yaw = k * math.pi / 2 + 0.3
        f = np.array([math.cos(yaw), 0.0, math.sin(yaw)])
        r = np.cross([0, 1, 0], f); r /= np.linalg.norm(r)
        out.append((f, r, np.cross(f, r)))
    f = np.array([0.0, 1.0, 0.0]); r = np.array([1.0, 0, 0])
    out.append((f, r, np.cross(f, r)))
    return out


def coverage(surfaces, center, radius):
    size = int(2 * radius * SCALE) + 4
    total = 0.0
    for f, r, u in views():
        origin = (float(center @ r) - radius, float(center @ u) - radius)
        _, a, _, _ = wa.rasterize(surfaces, r, u, f, origin, SCALE, (size, size))
        total += a.sum() / SCALE ** 2
    return total


def original(mesh_path: Path) -> Path:
    """The unscaled mesh: saved on the first run, read on every run."""
    SOURCES.mkdir(parents=True, exist_ok=True)
    keep = SOURCES / (mesh_path.parent.parent.name + "__" + mesh_path.name)
    if not keep.exists():
        shutil.copyfile(mesh_path, keep)
    return keep


def card_components(idx, count):
    parent = np.arange(count)

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for a, b, c in idx:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[rc] = ra
    return np.array([find(i) for i in range(count)])


def parts_at(prefab: Path, level: int):
    """(mesh asset, Surface, masked) per part drawn at this level."""
    out = []
    for mesh_el, material_el in bo.prefab_parts(prefab):
        lods = [i.text or "" for i in mesh_el.findall("Mesh.lodMeshes/item")]
        lod_mats = [i.text or "" for i in material_el.findall("Material.lodMaterials/item")]
        base_mat = material_el.get("Material.materialAsset")
        if level == 0:
            mesh, mat = mesh_el.get("Mesh.file"), base_mat
        elif len(lods) >= level:
            mesh = lods[level - 1]
            mat = (lod_mats[level - 1] if level - 1 < len(lod_mats) else "") or base_mat
        else:
            continue
        if not mesh or "Impostor" in mesh:
            continue
        material = json.loads((wa.ROOT / mat).read_text(encoding="utf-8"))
        path = wa.ASSETS / mesh.removeprefix("Assets/")
        source = original(path) if level > 0 else path
        p, n, uv, c, idx = wa.read_native_mesh(source)
        masked = material.get("alphaMode") == "Mask"
        surface = wa.Surface(p, n, uv, c, idx, texture=bo.load_texture(material.get("baseColorTexture", "")),
                             base_color=material.get("baseColor", [1, 1, 1])[:3],
                             alpha_cutoff=material.get("alphaCutoff", 0.5) if masked else 0.0,
                             use_vertex_colors=bool(material.get("useVertexColors", False)))
        out.append((mesh, path, source, surface, masked))
    return out


def scaled_positions(surface, roots, factor):
    p = surface.p.copy()
    for root in np.unique(roots):
        members = roots == root
        centre = p[members].mean(axis=0)
        p[members] = centre + (p[members] - centre) * factor
    return p


def write_positions(source: Path, target: Path, positions):
    raw = bytearray(source.read_bytes())
    count = struct.unpack_from("<4I", raw, 0)[2]
    data = np.frombuffer(bytes(raw), dtype=np.float32, count=count * wa.VERTEX_FLOATS,
                         offset=16).reshape(count, wa.VERTEX_FLOATS).copy()
    data[:, 0:3] = positions.astype(np.float32)
    raw[16:16 + data.nbytes] = data.tobytes()
    target.write_bytes(bytes(raw))


def match(prefab: Path):
    name = prefab.stem
    base = parts_at(prefab, 0)
    if not base:
        return
    base_meshes = {b[0] for b in base}
    pts = np.concatenate([b[3].p for b in base])
    lo, hi = pts.min(0), pts.max(0)
    center = (lo + hi) / 2
    radius = float(np.max(np.linalg.norm(pts - center, axis=1))) * 1.1
    target = coverage([b[3] for b in base], center, radius)
    for level in (1, 2):
        parts = parts_at(prefab, level)
        cards = [(i, card_components(p[3].tri, len(p[3].p))) for i, p in enumerate(parts)
                 if p[4] and p[0] not in base_meshes]
        if not cards:
            continue
        rest = [p[3].p.copy() for p in parts]

        def measure(factor):
            for i, roots in cards:
                parts[i][3].p = scaled_positions(wa.Surface(rest[i], parts[i][3].n, parts[i][3].uv,
                                                            parts[i][3].c, parts[i][3].tri), roots, factor)
            return coverage([p[3] for p in parts], center, radius)

        before = measure(1.0)
        # Coverage grows about as the square of the card scale: start from
        # that and correct twice (secant steps on the measured coverage).
        factor = math.sqrt(target / max(before, 1e-6))
        after = measure(factor)
        for _ in range(2):
            if abs(after / target - 1.0) < .005:
                break
            factor *= math.sqrt(target / max(after, 1e-6))
            after = measure(factor)
        print(f"{name} LOD{level}: {before / target * 100:.1f} % -> {after / target * 100:.1f} % of LOD0 "
              f"(cards x{factor:.3f})", flush=True)
        for i, _ in cards:
            write_positions(parts[i][2], parts[i][1], parts[i][3].p)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only")
    args = parser.parse_args()
    prefabs = [p for family in FAMILIES for p in sorted((PREFABS / family).glob("*.prefab"))]
    if args.only:
        prefabs = [p for p in prefabs if p.stem == args.only]
    for prefab in prefabs:
        match(prefab)


if __name__ == "__main__":
    main()
