#!/usr/bin/env python3
"""Generate boulders and cliff blocks: noise-displaced, faceted rock meshes
with a ground pivot, three LODs and a world-space triplanar rock material.

Writes (under Engine/Core/Assets):
  Models/Woodland/Boulders/Meshes/<Rock>_LOD{0,1,2}.mesh
  Models/Woodland/Boulders/Materials/Boulder_Rock.material
  Prefabs/Woodland/Boulders/<Rock>.prefab

Shape: each rock is a radius function over the unit sphere, so every LOD
samples the same surface (an icosphere of 5120, 1280 or 320 triangles for
boulders). The function is an ellipsoid cut by a handful of random planes
(smooth-min, so the cuts read as broad facets with slightly eased edges),
then displaced by 3D value-noise FBM; cliff blocks add near-vertical cuts and
horizontal strata. Normals are split at creases sharper than 38 degrees, so
facets keep hard edges while noise areas stay smooth.

Pivot: the lowest point sinks below the origin by a share of the height, so
uneven bases sit in the ground on slopes (TerrainObjectLayer aligns props to
the terrain normal).

Material: Material.triplanarScale projects the tileable Rock030 set in world
space (no stretching on scaled placements, one texel density for every
size). The meshes still carry box-projected UVs at the same scale, for
texture streaming estimates and as the fallback mapping. Vertex colours add
cavity darkening and faint moss on upward faces.

Usage: python Tools/generate_boulders.py
"""
from __future__ import annotations

import math
import random
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402

MODELS = wa.ASSETS / "Models" / "Woodland" / "Boulders"
MESHES = MODELS / "Meshes"
MATERIALS = MODELS / "Materials"
PREFABS = wa.ASSETS / "Prefabs" / "Woodland" / "Boulders"

TRIPLANAR_SCALE = 2.4      # metres per texture repeat
CREASE_DEGREES = 38.0


@dataclass
class Rock:
    name: str
    seed: int
    size: tuple              # ellipsoid radii (x, y, z), metres
    cuts: int                # random cutting planes
    cut_depth: tuple         # cut offset range as a share of the radius
    noise: float             # displacement amplitude (share of the radius)
    frequency: float
    sink: float              # share of the height below the pivot
    levels: tuple = (4, 3, 2)
    vertical_cuts: int = 0   # extra near-vertical planes (cliff faces)
    strata: float = 0.0      # horizontal ledge amplitude
    boxiness: float = 2.6    # superellipsoid exponent of the base (2 = ellipsoid)
    edge: float = 0.035      # smooth-min width of the cuts (smaller = crisper)
    bottom: float = 0.55     # flat base plane offset
    lod_distances: tuple = (35.0, 100.0)
    # Shear toward +z growing with height (share of the z radius at the top):
    # the upper part leans out over the base, an overhang a height field
    # cannot have. Placed facing downhill (LandscapeDriver GenerateCliffs).
    overhang: float = 0.0


ROCKS = [
    Rock("Boulder_01", 5101, (1.1, 0.8, 0.95), 9, (0.55, 0.82), 0.05, 1.6, 0.16, (3, 2, 1)),
    Rock("Boulder_02", 5203, (1.6, 1.1, 1.3), 10, (0.52, 0.8), 0.05, 1.4, 0.16, (4, 3, 2)),
    Rock("Boulder_03", 5307, (2.4, 1.4, 1.7), 11, (0.5, 0.8), 0.06, 1.2, 0.18, (4, 3, 2)),
    Rock("Boulder_flat_01", 5411, (2.2, 0.6, 1.8), 8, (0.6, 0.88), 0.04, 1.3, 0.22, (4, 3, 2),
         boxiness=2.3, bottom=0.6),
    Rock("Cliff_block_01", 5503, (4.0, 6.5, 2.8), 5, (0.62, 0.9), 0.045, 0.9, 0.08, (4, 3, 2),
         vertical_cuts=7, strata=0.05, boxiness=4.0, edge=0.025, bottom=0.7,
         lod_distances=(60.0, 180.0)),
    Rock("Cliff_block_02", 5609, (6.0, 9.0, 3.6), 6, (0.6, 0.9), 0.045, 0.8, 0.08, (4, 3, 2),
         vertical_cuts=8, strata=0.055, boxiness=4.5, edge=0.025, bottom=0.7,
         lod_distances=(70.0, 220.0)),
    # A crag whose top leans ~3 m out over its foot.
    Rock("Cliff_overhang_01", 5711, (4.5, 6.0, 3.0), 5, (0.62, 0.9), 0.05, 0.9, 0.08, (4, 3, 2),
         vertical_cuts=6, strata=0.06, boxiness=3.6, edge=0.025, bottom=0.7,
         lod_distances=(60.0, 180.0), overhang=1.1),
    # Crags: rock-face pieces 25-40 m across, strongly bedded and fractured,
    # packed and stacked on steep slopes (LandscapeDriver GenerateCliffs) so
    # they overlap into a broken wall. A height field alone gives smooth
    # steep ground; these are what make it read as a mountain's rocky edge.
    Rock("Crag_face_01", 5917, (14.0, 11.0, 6.0), 7, (0.55, 0.88), 0.06, 0.8, 0.12, (5, 4, 2),
         vertical_cuts=11, strata=0.11, boxiness=4.5, edge=0.02, bottom=0.75,
         lod_distances=(140.0, 400.0), overhang=0.35),
    Rock("Crag_face_02", 6029, (19.0, 15.0, 7.5), 8, (0.55, 0.88), 0.065, 0.7, 0.12, (5, 4, 2),
         vertical_cuts=13, strata=0.12, boxiness=5.0, edge=0.02, bottom=0.75,
         lod_distances=(160.0, 450.0), overhang=0.2),
    Rock("Crag_spire_01", 6131, (6.5, 15.0, 5.5), 6, (0.6, 0.9), 0.07, 0.9, 0.1, (5, 4, 2),
         vertical_cuts=8, strata=0.1, boxiness=3.6, edge=0.02, bottom=0.8,
         lod_distances=(140.0, 400.0), overhang=0.25),
    # A wide, low shelf with stepped strata that juts from the slope.
    Rock("Outcrop_ledge_01", 5813, (5.5, 2.2, 3.4), 6, (0.6, 0.9), 0.045, 1.0, 0.1, (4, 3, 2),
         vertical_cuts=5, strata=0.09, boxiness=3.4, edge=0.025, bottom=0.6,
         lod_distances=(60.0, 180.0), overhang=0.55),
]


# --- icosphere ----------------------------------------------------------------

def icosphere(level):
    t = (1 + 5 ** 0.5) / 2
    verts = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
             (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    verts = [tuple(v / np.linalg.norm(v)) for v in np.array(verts, dtype=float)]
    faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2),
             (10, 7, 6), (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5),
             (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]
    for _ in range(level):
        cache = {}

        def mid(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                m = (np.array(verts[a]) + np.array(verts[b])) * 0.5
                verts.append(tuple(m / np.linalg.norm(m)))
                cache[key] = len(verts) - 1
            return cache[key]
        refined = []
        for a, b, c in faces:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            refined += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = refined
    v = np.array(verts)
    f = np.array(faces)
    # Outward winding: cross(b - a, c - a) along the vertex direction.
    n = np.cross(v[f[:, 1]] - v[f[:, 0]], v[f[:, 2]] - v[f[:, 0]])
    flip = np.sum(n * v[f[:, 0]], axis=1) < 0
    f[flip] = f[flip][:, [0, 2, 1]]
    return v, f


# --- shape ----------------------------------------------------------------------

def smooth_min(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1)
    return b * (1 - h) + a * h - k * h * (1 - h)


class Shape:
    def __init__(self, rock: Rock):
        rng = random.Random(rock.seed)
        self.rock = rock
        self.size = np.array(rock.size)
        self.planes = []
        for _ in range(rock.cuts):
            n = np.array([rng.gauss(0, 1), rng.gauss(0, 1) * 0.8 + 0.25, rng.gauss(0, 1)])
            n /= np.linalg.norm(n)
            self.planes.append((n, rng.uniform(*rock.cut_depth)))
        for k in range(rock.vertical_cuts):
            a = math.tau * k / rock.vertical_cuts + rng.uniform(-0.35, 0.35)
            n = np.array([math.cos(a), rng.uniform(-0.12, 0.15), math.sin(a)])
            n /= np.linalg.norm(n)
            self.planes.append((n, rng.uniform(0.62, 0.86)))
        # Flat-ish base so the rock sits on the ground instead of on a point.
        n = np.array([rng.uniform(-0.1, 0.1), -1.0, rng.uniform(-0.1, 0.1)])
        self.planes.append((n / np.linalg.norm(n), rock.bottom))
        self.seed = rock.seed
        self.strata_phase = rng.random() * 10

    def radius(self, directions):
        """Distance from the centre to the surface along unit directions,
        in the ellipsoid's normalised space (1 = ellipsoid surface)."""
        d = np.asarray(directions)
        p = self.rock.boxiness
        r = np.sum(np.abs(d) ** p, axis=1) ** (-1.0 / p)   # superellipsoid
        for n, offset in self.planes:
            # Plane dot(x, n) = offset in normalised space.
            facing = d @ n
            limit = np.where(facing > 1e-3, offset / np.maximum(facing, 1e-3), 10.0)
            r = smooth_min(r, limit, self.rock.edge)
        noise = wa.fbm_3d(d * 2.0, self.rock.frequency, 4, self.seed) - 0.5
        fine = wa.fbm_3d(d * 2.0, self.rock.frequency * 4, 2, self.seed + 3) - 0.5
        r = r * (1 + 2 * self.rock.noise * noise + 0.5 * self.rock.noise * fine)
        if self.rock.strata > 0:
            # Stepped ledges: a sawtooth in height, warped by noise.
            y = d[:, 1] * r * 9 + self.strata_phase + 2.5 * wa.fbm_3d(d, 1.5, 2, self.seed + 9)
            ledge = (y - np.floor(y)) ** 3
            r = r * (1 - self.rock.strata * ledge)
        return r

    def points(self, directions):
        r = self.radius(directions)
        points = directions * r[:, None] * self.size[None, :]
        if self.rock.overhang > 0:
            rise = np.clip(points[:, 1] / self.size[1], 0.0, 1.5)
            points[:, 2] += self.rock.overhang * self.size[2] * rise ** 1.5
        return points


# --- mesh -----------------------------------------------------------------------

def crease_split(positions, faces, crease_degrees):
    """Split vertices whose incident faces differ by more than the crease
    angle. Returns new positions, normals, faces."""
    fn = np.cross(positions[faces[:, 1]] - positions[faces[:, 0]], positions[faces[:, 2]] - positions[faces[:, 0]])
    area = np.linalg.norm(fn, axis=1, keepdims=True)
    unit = fn / np.maximum(area, 1e-12)
    cos_limit = math.cos(math.radians(crease_degrees))
    incident = [[] for _ in range(len(positions))]
    for f, tri in enumerate(faces):
        for corner in tri:
            incident[corner].append(f)
    new_pos, new_nrm = [], []
    corner_index = np.zeros_like(faces)
    for v, fs in enumerate(incident):
        groups = []   # list of (face list, accumulated normal)
        for f in fs:
            for g in groups:
                if unit[f] @ (g[1] / max(np.linalg.norm(g[1]), 1e-12)) >= cos_limit:
                    g[0].append(f); g[1] += fn[f]
                    break
            else:
                groups.append([[f], fn[f].copy()])
        for members, accum in groups:
            new_pos.append(positions[v])
            new_nrm.append(accum / max(np.linalg.norm(accum), 1e-12))
            idx = len(new_pos) - 1
            for f in members:
                corner = np.nonzero(faces[f] == v)[0][0]
                corner_index[f, corner] = idx
    return np.array(new_pos), np.array(new_nrm), corner_index


def build_lod(shape: Shape, level: int):
    dirs, faces = icosphere(level)
    p = shape.points(dirs)
    # Ground pivot: the base sinks by a share of the height.
    height = p[:, 1].max() - p[:, 1].min()
    p[:, 1] -= p[:, 1].min() + shape.rock.sink * height
    # Flatten the part far below ground into a footing (never visible).
    floor = -shape.rock.sink * height * 0.85
    p[:, 1] = np.maximum(p[:, 1], floor)
    faces_src = faces
    positions, normals, faces = crease_split(p, faces, CREASE_DEGREES)
    # Cavity: how far each vertex sits inside a smoothed version of the shape.
    r_vertex = np.linalg.norm(p / shape.size, axis=1)
    neighbours = [[] for _ in range(len(p))]
    for a, b, c in faces_src:
        neighbours[a] += [b, c]; neighbours[b] += [a, c]; neighbours[c] += [a, b]
    r_mean = np.array([r_vertex[n].mean() for n in neighbours])
    cavity_src = np.clip((r_mean - r_vertex) * 6, 0, 1)
    # Map split vertices back to their source vertex by position.
    keys = {tuple(np.round(v, 6)): i for i, v in enumerate(p)}
    source = np.array([keys[tuple(np.round(v, 6))] for v in positions])
    cavity = cavity_src[source]
    moss_noise = wa.fbm_3d(positions * 0.6, 1.0, 3, shape.seed + 77)
    moss = np.clip((normals[:, 1] - 0.55) * 2.5, 0, 1) * np.clip((moss_noise - 0.45) * 3, 0, 1)
    tone = 0.92 + 0.16 * (wa.fbm_3d(positions * 0.3, 1.0, 2, shape.seed + 5) - 0.5) * 2
    base = np.stack([tone, tone * 0.98, tone * 0.95], axis=1)
    base *= (1 - 0.45 * cavity)[:, None]
    moss_color = np.array([0.55, 0.62, 0.42])
    color = base * (1 - 0.5 * moss[:, None]) + moss_color * 0.5 * moss[:, None] * tone[:, None]
    # Box-projected UVs at the triplanar scale (dominant normal axis per vertex).
    axis = np.argmax(np.abs(normals), axis=1)
    u = np.where(axis == 0, positions[:, 2], positions[:, 0])
    v = np.where(axis == 1, positions[:, 2], -positions[:, 1])
    uv = np.stack([u, v], axis=1) / TRIPLANAR_SCALE
    mesh = wa.MeshBuilder()
    for i in range(len(positions)):
        mesh.vertex(positions[i], normals[i], uv[i], (color[i, 0], color[i, 1], color[i, 2], 1.0))
    for a, b, c in faces:
        mesh.tri(int(a), int(b), int(c))
    return mesh


def main():
    material = wa.write_material(
        MATERIALS / "Boulder_Rock.material",
        baseColor=[0.86, 0.84, 0.8],
        baseColorTexture="Assets/Textures/Terrain/PBR/Rock030/Rock030_Color.jpg",
        normalTexture="Assets/Textures/Terrain/PBR/Rock030/Rock030_NormalGL.jpg",
        metallicRoughnessTexture="Assets/Textures/Terrain/PBR/Rock030/Rock030_Roughness.jpg",
        occlusionTexture="Assets/Textures/Terrain/PBR/Rock030/Rock030_AmbientOcclusion.jpg",
        occlusionStrength=1.0, useVertexColors=True, metallic=0, roughness=1.0, normalScale=1.0,
        triplanarScale=TRIPLANAR_SCALE)
    for rock in ROCKS:
        shape = Shape(rock)
        paths, counts = [], []
        for lod, level in enumerate(rock.levels):
            mesh = build_lod(shape, level)
            path = MESHES / f"{rock.name}_LOD{lod}.mesh"
            counts.append(mesh.write_native(path))
            paths.append(wa.asset_path(path))
        wa.write_prefab(PREFABS / f"{rock.name}.prefab", rock.name, [
            dict(mesh=paths[0], lods=paths[1:], material=wa.engine_path(material),
                 lod1=rock.lod_distances[0], lod2=rock.lod_distances[1])])
        print(f"{rock.name}: {rock.size[0] * 2:.1f} x {rock.size[1] * 2:.1f} x {rock.size[2] * 2:.1f} m; "
              f"triangles LOD0/1/2 = {'/'.join(str(c) for c in counts)}")


if __name__ == "__main__":
    main()
