"""Bakes terrain stamps from the cliff scan meshes (.md/todo.md, "Cliffs as terrain stamps").

A stamp is a scan's outer surface as a regular grid of 3D points (a vector
displacement map), seen along the face's outward direction rather than from
above, so overhangs fit: a height map holds one height per ground position and
cannot (tried 2026-11-21). The terrain will draw the grid as a patch whose
border blends into the ground (stage 3); the scans stay as bake sources only.

Frame: the mesh's own (x along the face, y up, z out of the face, origin at the
base centre). `direction` is the area-weighted mean surface normal (out of the
face and somewhat up); `axis_u` is x made perpendicular to it, `axis_v` =
direction x axis_u (up the face). Grid point (i, j) sits at
  u = origin_u + i * spacing, v = origin_v + j * spacing
on the plane through the mesh origin, and stores the outermost surface point
along `direction` there:
  position (x, y, z), normal (x, y, z), texcoord (u, v) of the scan's own
  material, weight (0 at the outline, 1 from EDGE metres inside; where the
  terrain patch blends from the ground to the rock).
Points the scan does not cover have weight 0 and the position of the nearest
covered point.

Steps are eased (ease_steps): where the outermost layer jumps back to a layer
behind it (a ledge over a recess), one grid triangle bridged the whole jump as
a long face along the direction, which caught the sun as a bright vertical
sliver (reported 2026-11-21). Points behind such a jump are pulled out until
the depth changes by at most STEP_SLOPE per metre across the grid, so the
recess becomes a ramp over several points; the outer silhouette is unchanged.

The terrain draws a stamp from simplified triangle lists over these points
(tools/stamp_simplify.py), one per detail level, with surface errors up to
LEVEL_TOLERANCES (stamp-space metres; pieces are placed at 1.6-4.9 times that).
Fine relief below that is meant to come back as a normal map (stage 4).

File (Assets/TerrainData/Stamps/<name>.stamp), little endian:
  u64 magic "TRNRSTP1", u32 version 2, u32 width, u32 height, f32 spacing,
  f32 origin u, origin v, f32 direction[3], axis_u[3], axis_v[3],
  u32 material path length, material path (UTF-8),
  then width*height points (rows along v) of 9 f32: position[3], normal[3],
  texcoord[2], weight,
  then u32 level count, and per level u32 triangle count followed by that many
  triangles of 3 u32 point indices (u + v * width), wound out of the face.

Usage: python tools/bake_terrain_stamp.py [--preview DIR]
  --preview writes <DIR>/<name>_check.png: the scan and each detail level,
  flat shaded, from the front (along -direction) and from 60 degrees to the
  side, scan on the left. Printed per stamp: points covered, and how much of the scan
  the grid loses (surface hidden behind the outermost layer) or stretches
  (neighbouring points more than STRETCH apart along the direction).
"""

import argparse
import re
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy import ndimage

import stamp_simplify
from woodland_assets import ASSETS, MeshBuilder, engine_path, read_native_mesh, write_png

SPACING = 0.25        # metres per grid step, at the mesh's own scale
EDGE = 1.0            # metres over which the weight rises from the outline
STRETCH = 1.0         # a neighbour jump beyond this along the direction is a fold
LEVEL_TOLERANCES = (0.2, 0.4, 0.8)  # detail levels: largest surface error, metres
STEP_SLOPE = 1.0      # eased steps: most depth change per metre across the grid
STAMP_MAGIC = int.from_bytes(b"TRNRSTP1", "little")
OUTPUT = ASSETS / "TerrainData" / "Stamps"
PROXY_MESHES = ASSETS / "Models" / "Woodland" / "Stamps"
PROXY_PREFABS = ASSETS / "Prefabs" / "Woodland" / "Stamps"
MESHES = ASSETS / "Models" / "Woodland" / "Scans" / "Meshes"
PREFABS = ASSETS / "Prefabs" / "Woodland" / "Scans"
# The five photo-scanned faces the landscape places, in the order of
# LandscapeDriver.cpp `Cliffs` (half width along the face, half depth out of it,
# base scale; the C++ table and tools/landscape_cliffs.py share these values).
SHAPES = (
    ("Cliff_Scan_02", 10.0, 3.3, 2.2),
    ("Mountainside_01", 5.1, 2.6, 3.6),
    ("Cliff_Scan_01", 4.1, 2.2, 4.0),
    ("RockFace_01", 3.6, 2.8, 4.0),
    ("RockFace_02", 2.4, 1.7, 4.5),
)


@dataclass
class Stamp:
    name: str
    material: str
    spacing: float
    origin: tuple            # (u, v) of point (0, 0)
    direction: np.ndarray
    axis_u: np.ndarray
    axis_v: np.ndarray
    position: np.ndarray     # (height, width, 3), rows along v
    normal: np.ndarray       # (height, width, 3)
    texcoord: np.ndarray     # (height, width, 2)
    weight: np.ndarray       # (height, width)
    hidden_share: float      # share of scan area behind the outermost layer
    stretch_share: float     # share of covered neighbour pairs that jump > STRETCH
    levels: list = None      # (k, 3) point-index triangles per detail level


def scan_material(name):
    text = (PREFABS / f"{name}.prefab").read_text(encoding="utf-8")
    found = re.search(r'Material\.materialAsset="([^"]+)"', text)
    return found.group(1) if found else ""


def frame(positions, triangles):
    a, b, c = positions[triangles[:, 0]], positions[triangles[:, 1]], positions[triangles[:, 2]]
    face = np.cross(b - a, c - a)
    direction = face.sum(axis=0)
    direction /= np.linalg.norm(direction)
    axis_u = np.array([1.0, 0.0, 0.0]) - direction * direction[0]
    axis_u /= np.linalg.norm(axis_u)
    axis_v = np.cross(direction, axis_u)
    return direction, axis_u, axis_v


def rasterize(uvw, attributes, triangles, origin, spacing, shape, outermost=True):
    """Per grid point, the attributes of the triangle point with the largest
    (outermost) or smallest w over it. uvw: (n, 3) projected vertices."""
    rows, cols = shape
    depth = np.full(shape, -np.inf if outermost else np.inf)
    result = np.zeros(shape + (attributes.shape[1],))
    for tri in triangles:
        p = uvw[tri]
        lo = np.floor((p[:, :2].min(axis=0) - origin) / spacing).astype(int)
        hi = np.ceil((p[:, :2].max(axis=0) - origin) / spacing).astype(int)
        lo = np.maximum(lo, 0)
        hi = np.minimum(hi, (cols - 1, rows - 1))
        if (hi < lo).any():
            continue
        gi, gj = np.meshgrid(np.arange(lo[0], hi[0] + 1), np.arange(lo[1], hi[1] + 1))
        gu = origin[0] + gi * spacing
        gv = origin[1] + gj * spacing
        (u0, v0), (u1, v1), (u2, v2) = p[0, :2], p[1, :2], p[2, :2]
        det = (v1 - v2) * (u0 - u2) + (u2 - u1) * (v0 - v2)
        if abs(det) < 1e-12:
            continue
        l0 = ((v1 - v2) * (gu - u2) + (u2 - u1) * (gv - v2)) / det
        l1 = ((v2 - v0) * (gu - u2) + (u0 - u2) * (gv - v2)) / det
        l2 = 1.0 - l0 - l1
        inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        if not inside.any():
            continue
        gi, gj, l0, l1, l2 = gi[inside], gj[inside], l0[inside], l1[inside], l2[inside]
        w = l0 * p[0, 2] + l1 * p[1, 2] + l2 * p[2, 2]
        better = w > depth[gj, gi] if outermost else w < depth[gj, gi]
        if not better.any():
            continue
        gi, gj, w = gi[better], gj[better], w[better]
        l0, l1, l2 = l0[better, None], l1[better, None], l2[better, None]
        depth[gj, gi] = w
        att = attributes[tri]
        result[gj, gi] = l0 * att[0] + l1 * att[1] + l2 * att[2]
    return depth, result


def ease_steps(position, normal, mask, movable, direction, spacing):
    """Pulls points behind depth jumps outward (along `direction`) until no
    neighbour pair differs by more than STEP_SLOPE per metre, then recomputes
    the normals of moved points from the grid. Only `movable` points move:
    the band along the outline blends into the terrain anyway, and thin
    points there pulled forward (the direction leans up) stood above the
    crest as needles. Returns how many moved."""
    depth = np.where(mask, position @ direction, -np.inf)
    eased = depth.copy()
    offsets = [(dr, dc) for dr in (-1, 0, 1) for dc in (-1, 0, 1) if dr or dc]
    rows, cols = depth.shape
    for _ in range(4 * max(rows, cols)):
        before = eased.copy()
        for dr, dc in offsets:
            limit = STEP_SLOPE * spacing * np.hypot(dr, dc)
            shifted = np.full_like(eased, -np.inf)
            shifted[max(dr, 0):rows + min(dr, 0), max(dc, 0):cols + min(dc, 0)] =                 eased[max(-dr, 0):rows + min(-dr, 0), max(-dc, 0):cols + min(-dc, 0)]
            eased = np.maximum(eased, shifted - limit)
        eased = np.where(mask, eased, -np.inf)
        if np.array_equal(eased, before):
            break
    eased = np.where(movable, eased, depth)
    # The band along the outline eases the other way: points in front of a
    # jump are pulled back into the rock (they only move down and back).
    band = mask & ~movable
    cut = np.where(mask, eased, np.inf)
    for _ in range(4 * max(rows, cols)):
        before = cut.copy()
        for dr, dc in offsets:
            limit = STEP_SLOPE * spacing * np.hypot(dr, dc)
            shifted = np.full_like(cut, np.inf)
            shifted[max(dr, 0):rows + min(dr, 0), max(dc, 0):cols + min(dc, 0)] =                 cut[max(-dr, 0):rows + min(-dr, 0), max(-dc, 0):cols + min(-dc, 0)]
            cut = np.where(band, np.minimum(cut, shifted + limit), cut)
        if np.array_equal(cut, before):
            break
    eased = np.where(band, cut, eased)
    moved = mask.copy()
    moved[mask] = np.abs(eased[mask] - depth[mask]) > 1e-4
    position[moved] += (eased[moved] - depth[moved])[:, None] * direction
    # Normals of moved points (and their neighbours) from the eased grid:
    # u x v faces along the direction.
    refresh = ndimage.binary_dilation(moved) & mask
    du = np.zeros_like(position)
    dv = np.zeros_like(position)
    du[:, 1:-1] = position[:, 2:] - position[:, :-2]
    dv[1:-1, :] = position[2:, :] - position[:-2, :]
    fresh = np.cross(du, dv)
    length = np.linalg.norm(fresh, axis=-1, keepdims=True)
    good = refresh & (length[..., 0] > 1e-9)
    normal[good] = (fresh / np.maximum(length, 1e-9))[good]
    return int(moved.sum())


def bake(name, lod=1, spacing=SPACING):
    positions, normals, texcoords, _, triangles = read_native_mesh(MESHES / f"{name}_LOD{lod}.mesh")
    direction, axis_u, axis_v = frame(positions, triangles)
    uvw = np.stack([positions @ axis_u, positions @ axis_v, positions @ direction], axis=1)
    origin = np.floor(uvw[:, :2].min(axis=0) / spacing) * spacing - spacing
    size = np.ceil((uvw[:, :2].max(axis=0) - origin) / spacing).astype(int) + 2
    shape = (int(size[1]), int(size[0]))
    attributes = np.concatenate([positions, normals, texcoords], axis=1)
    depth, front = rasterize(uvw, attributes, triangles, origin, spacing, shape, True)
    back, _ = rasterize(uvw, attributes, triangles, origin, spacing, shape, False)
    covered = np.isfinite(depth)
    mask = ndimage.binary_fill_holes(ndimage.binary_closing(covered, iterations=2)) | covered
    near = ndimage.distance_transform_edt(~covered, return_indices=True)[1]
    front = np.where(covered[..., None], front, front[near[0], near[1]])
    position, normal, texcoord = front[..., 0:3], front[..., 3:6], front[..., 6:8]
    normal /= np.maximum(np.linalg.norm(normal, axis=-1, keepdims=True), 1e-9)
    raw_depth = np.where(covered, position @ direction, np.nan)
    raw_jumps = np.concatenate([np.abs(np.diff(raw_depth, axis=0)).ravel(),
                                np.abs(np.diff(raw_depth, axis=1)).ravel()])
    raw_jumps = raw_jumps[np.isfinite(raw_jumps)]
    inside = ndimage.distance_transform_edt(mask) * spacing
    eased = ease_steps(position, normal, mask, inside >= EDGE, direction, spacing)
    t = np.clip(inside / EDGE, 0.0, 1.0)
    weight = np.where(mask, t * t * (3 - 2 * t), 0.0)
    # Hidden: surface behind the outermost layer (front and back hits apart).
    layered = covered & np.isfinite(back) & (depth - back > STRETCH)
    hidden = layered.sum() / max(covered.sum(), 1)
    w = np.where(covered, position @ direction, np.nan)
    jumps = np.concatenate([np.abs(np.diff(w, axis=0)).ravel(), np.abs(np.diff(w, axis=1)).ravel()])
    jumps = jumps[np.isfinite(jumps)]
    stretch = (jumps > STRETCH).mean() if jumps.size else 0.0
    print(f"  {name}: eased {eased} points; neighbour jumps over 0.5 m {(raw_jumps > 0.5).mean() * 100:.1f} % "
          f"-> {(jumps > 0.5).mean() * 100:.1f} %, largest {raw_jumps.max():.2f} -> {jumps.max():.2f} m")
    levels = stamp_simplify.simplify_levels(position.reshape(-1, 3), weight.reshape(-1),
                                            stamp_simplify.grid_triangles(weight), LEVEL_TOLERANCES)
    return Stamp(name, scan_material(name), spacing, (float(origin[0]), float(origin[1])),
                 direction, axis_u, axis_v, position, normal, texcoord, weight,
                 float(hidden), float(stretch), levels)


def bake_all():
    return [bake(name) for name, *_ in SHAPES]


def write_stamp(stamp, directory=OUTPUT):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    height, width = stamp.weight.shape
    material = stamp.material.encode("utf-8")
    points = np.concatenate([stamp.position, stamp.normal, stamp.texcoord, stamp.weight[..., None]],
                            axis=-1).astype("<f4")
    with open(directory / f"{stamp.name}.stamp", "wb") as file:
        file.write(struct.pack("<QIIIf2f9fI", STAMP_MAGIC, 2, width, height, stamp.spacing,
                               *stamp.origin, *stamp.direction, *stamp.axis_u, *stamp.axis_v,
                               len(material)))
        file.write(material)
        file.write(points.tobytes())
        file.write(struct.pack("<I", len(stamp.levels)))
        for level in stamp.levels:
            file.write(struct.pack("<I", len(level)))
            file.write(np.ascontiguousarray(level, dtype="<u4").tobytes())


# --- proxies -------------------------------------------------------------------

def write_proxy(stamp, material):
    """A prefab drawing the stamp as an ordinary mesh, for far cells (the far
    blocks place it where loaded cells draw the stamp as terrain): its detail
    levels as LOD0-2 over the eased points, textured with the stamp's own
    images. Each vertex's normal is the stamp direction and its tangent the
    u axis, so the mesh's tangent frame is the stamp's axes, which the normal
    image is expressed in (stamp_textures.py); the shading normals come from
    that image. Triangles wholly on the weight-0 ring are left out (far away
    the ring has no ground to blend to)."""
    height, width = stamp.weight.shape
    points = stamp.position.reshape(-1, 3)
    weight = stamp.weight.reshape(-1)
    tangent = (*stamp.axis_u, 1.0)
    files = []
    for level, triangles in enumerate(stamp.levels):
        builder = MeshBuilder()
        slots = {}

        def vertex(index):
            if index not in slots:
                u, v = index % width, index // width
                slots[index] = builder.vertex(points[index], stamp.direction,
                                              (u / (width - 1), v / (height - 1)), tangent=tangent)
            return slots[index]

        for a, b, c in triangles:
            if weight[a] <= 0 and weight[b] <= 0 and weight[c] <= 0:
                continue
            builder.tri(vertex(a), vertex(b), vertex(c))
        path = PROXY_MESHES / f"{stamp.name}_LOD{level}.mesh"
        builder.write_native(path)
        files.append("Assets/" + path.resolve().relative_to(ASSETS.resolve()).as_posix())
    lods = "".join(f"\n          <item>{path}</item>" for path in files[1:])
    prefab = PROXY_PREFABS / f"{stamp.name}.prefab"
    prefab.parent.mkdir(parents=True, exist_ok=True)
    prefab.write_text(f"""<?xml version='1.0' encoding='utf-8'?>
<Prefab xmlns:components="urn:engine-components" version="1" type="prefab">
  <object name="{stamp.name}" enabled="true">
    <components>
      <components:Mesh Mesh.file="{files[0]}" Mesh.foliageWindStrength="0" Mesh.useLightProbes="false" Mesh.gpuLodSelection="false">
        <Mesh.lodMeshes>{lods}
        </Mesh.lodMeshes>
      </components:Mesh>
      <components:Material Material.materialAsset="{engine_path(ASSETS / material.removeprefix('Assets/'))}" />
    </components>
    <children />
    <transform>
      <Transform.position _arrayEncoding="spaceSeparatedNumbers">0 0 0</Transform.position>
      <Transform.rotation _arrayEncoding="spaceSeparatedNumbers">0 0 0</Transform.rotation>
      <Transform.scale _arrayEncoding="spaceSeparatedNumbers">1 1 1</Transform.scale>
    </transform>
  </object>
</Prefab>
""", encoding="utf-8")
    return prefab


# --- preview ------------------------------------------------------------------

def _render(positions, triangles, view, size=384):
    """Flat-shaded orthographic render along -view (z-buffer), grey 0..1."""
    view = view / np.linalg.norm(view)
    right = np.cross(np.array([0.0, 1.0, 0.0]), view)
    right /= np.linalg.norm(right)
    up = np.cross(view, right)
    p = np.stack([positions @ right, positions @ up, positions @ view], axis=1)
    lo, hi = p[:, :2].min(axis=0), p[:, :2].max(axis=0)
    scale = (size - 8) / max((hi - lo).max(), 1e-6)
    xy = (p[:, :2] - lo) * scale + 4
    image = np.zeros((size, size))
    depth = np.full((size, size), -np.inf)
    light = np.array([0.4, 0.8, 0.45])
    light /= np.linalg.norm(light)
    a, b, c = positions[triangles[:, 0]], positions[triangles[:, 1]], positions[triangles[:, 2]]
    face = np.cross(b - a, c - a)
    face /= np.maximum(np.linalg.norm(face, axis=1, keepdims=True), 1e-12)
    shade = 0.25 + 0.75 * np.abs(face @ light)
    for index, tri in enumerate(triangles):
        q = xy[tri]
        lo_i = np.maximum(np.floor(q.min(axis=0)).astype(int), 0)
        hi_i = np.minimum(np.ceil(q.max(axis=0)).astype(int), size - 1)
        if (hi_i < lo_i).any():
            continue
        gx, gy = np.meshgrid(np.arange(lo_i[0], hi_i[0] + 1), np.arange(lo_i[1], hi_i[1] + 1))
        (x0, y0), (x1, y1), (x2, y2) = q
        det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(det) < 1e-12:
            continue
        l0 = ((y1 - y2) * (gx - x2) + (x2 - x1) * (gy - y2)) / det
        l1 = ((y2 - y0) * (gx - x2) + (x0 - x2) * (gy - y2)) / det
        l2 = 1 - l0 - l1
        inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
        z = l0 * p[tri[0], 2] + l1 * p[tri[1], 2] + l2 * p[tri[2], 2]
        better = inside & (z > depth[gy, gx])
        depth[gy[better], gx[better]] = z[better]
        image[gy[better], gx[better]] = shade[index]
    return image[::-1]


def stamp_mesh(stamp, level=0):
    """A detail level's triangles over the stamp's points."""
    return stamp.position.reshape(-1, 3), stamp.levels[level]


def preview(stamp, directory):
    positions, _, _, _, triangles = read_native_mesh(MESHES / f"{stamp.name}_LOD1.mesh")
    side = stamp.direction * np.cos(np.radians(60)) + stamp.axis_u * np.sin(np.radians(60))
    rows = []
    for view in (stamp.direction, side):
        row = [_render(positions, triangles, view)]
        for level in range(len(stamp.levels)):
            row.append(_render(*stamp_mesh(stamp, level), view))
        rows.append(np.concatenate(row, axis=1))
    write_png(Path(directory) / f"{stamp.name}_check.png", np.concatenate(rows, axis=0))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", help="write <name>_check.png renders to this directory")
    arguments = parser.parse_args()
    import stamp_textures
    for stamp in bake_all():
        # The stamp's own surface images (stamp_textures.py); the stamp file
        # names their material.
        stamp.material, coverage = stamp_textures.bake_textures(
            stamp, MESHES / f"{stamp.name}_LOD0.mesh", stamp.material, OUTPUT)
        print(f"  {stamp.name}: textures {stamp_textures.SIZE} px, {coverage * 100:.0f}% of pixels on the scan")
        write_stamp(stamp)
        write_proxy(stamp, stamp.material)
        height, width = stamp.weight.shape
        counts = ", ".join(f"{len(np.unique(level))}" for level in stamp.levels)
        print(f"{stamp.name:16s} {width}x{height} points at {stamp.spacing:g} m (levels use {counts}), "
              f"{(stamp.weight > 0).mean() * 100:.0f}% covered, direction "
              f"({stamp.direction[0]:.2f}, {stamp.direction[1]:.2f}, {stamp.direction[2]:.2f}); "
              f"hidden {stamp.hidden_share * 100:.0f}% of covered points, "
              f"folds {stamp.stretch_share * 100:.1f}% of neighbour pairs")
        if arguments.preview:
            preview(stamp, arguments.preview)


if __name__ == "__main__":
    main()
