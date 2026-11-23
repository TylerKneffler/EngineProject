"""Morphs a height grid to the terrain stamps placed on it (.md/todo.md,
"Cliffs as terrain stamps").

A stamp (tools/bake_terrain_stamp.py) is drawn by the terrain as a patch whose
border closes onto the ground (Terrain::AppendStampGeometry). Where the ground
does not meet that border, the engine pulls the border onto it, so a stamp
reads as a shell with stretched skirts sitting on unrelated ground. This module
moves the ground instead, like a sculpting brush with a falloff:

  * A stamp's top edge (outline points, weight 0..BORDER_WEIGHT, with no rock
    above them up the face) that stands above the ground lifts the ground to
    it, where it is the highest stamp point over a grid sample. Its side and
    bottom edges do not: a height grid cannot follow an edge that runs down a
    wall (tried 2026-11-23: buttress mounds at every wall end), and the
    engine tucks those into the terrain behind them. Buried edges leave the
    ground alone, so the ground is never lowered.
  * Lifts are carried outward from where they apply and faded to nothing
    over FALLOFF metres with a smoothstep, then softened with a small
    Gaussian, so the ground rises into the stamp without a step.
  * In front of a stamp's foot the ground rises by a share of the rock's
    height (an apron, at most APRON_HEIGHT) and fades back over APRON_REACH,
    so the face sinks into a slope instead of meeting flat ground in a line.

Used by tools/build_landscape_demo.py (world archive) and
tools/build_stamp_test_scene.py; heights are a (rows along +z, cols along +x)
array of metres with sample (0, 0) at `origin` and `spacing` metres apart.
"""

import math
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[1]
STAMPS = ROOT / "Engine" / "Core" / "Assets" / "TerrainData" / "Stamps"
STAMP_MAGIC = int.from_bytes(b"TRNRSTP1", "little")

BORDER_WEIGHT = 0.5   # border ramp points (weight at or below this) meet the ground
TOP_ROWS = 3          # stamp rows below a column's topmost covered point that count as its top edge
SUPPORT = 0.3         # share of an edge's height above the ground in front that the
                      # ground behind must rise for the edge to count as a fin
FALLOFF = 14.0        # metres over which a lift fades out
APRON_HEIGHT = 2.5    # most the ground rises at a stamp's foot
APRON_SHARE = 0.12    # of the rock's height above the ground there
APRON_REACH = 8.0     # metres the apron fades over, outward from the foot
KERNEL = 0.6          # Gaussian sigma softening the lifts, in grid spacings


@dataclass
class Stamp:
    name: str
    width: int
    height: int
    spacing: float
    direction: np.ndarray
    axis_u: np.ndarray
    axis_v: np.ndarray
    material: str
    position: np.ndarray   # (height, width, 3), stamp space
    normal: np.ndarray     # (height, width, 3)
    weight: np.ndarray     # (height, width)


def read_stamp(path: Path) -> Stamp:
    """A .stamp file (format in tools/bake_terrain_stamp.py); levels are skipped."""
    raw = Path(path).read_bytes()
    magic, version, width, height, spacing = struct.unpack_from("<QIIIf", raw, 0)
    if magic != STAMP_MAGIC or version != 2:
        raise ValueError(f"{path}: not a v2 stamp")
    at = 24
    _origin = struct.unpack_from("<2f", raw, at); at += 8
    direction = np.array(struct.unpack_from("<3f", raw, at)); at += 12
    axis_u = np.array(struct.unpack_from("<3f", raw, at)); at += 12
    axis_v = np.array(struct.unpack_from("<3f", raw, at)); at += 12
    (length,) = struct.unpack_from("<I", raw, at); at += 4
    material = raw[at:at + length].decode("utf-8"); at += length
    points = np.frombuffer(raw, np.float32, width * height * 9, at).reshape(height, width, 9)
    return Stamp(Path(path).stem, width, height, spacing, direction, axis_u, axis_v, material,
                 points[..., 0:3].astype(np.float64), points[..., 3:6].astype(np.float64),
                 points[..., 8].astype(np.float64))


def load_stamp(name: str, cache={}) -> Stamp:
    if name not in cache:
        cache[name] = read_stamp(STAMPS / f"{name}.stamp")
    return cache[name]


def surface_rotation(normal, yaw_degrees):
    """TerrainObjectLayer::SurfaceRotation: up turned onto `normal` (shortest
    arc), after a yaw about +y."""
    up = np.array([0.0, 1.0, 0.0])
    n = np.asarray(normal, float)
    n = n / np.linalg.norm(n)
    axis = np.cross(up, n)
    sine, cosine = np.linalg.norm(axis), float(np.dot(up, n))
    if sine < 1e-9:
        align = np.eye(3)
    else:
        k = axis / sine
        kx = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
        align = np.eye(3) + sine * kx + (1 - cosine) * kx @ kx
    t = math.radians(yaw_degrees)
    yaw = np.array([[math.cos(t), 0, math.sin(t)], [0, 1, 0], [-math.sin(t), 0, math.cos(t)]])
    return align @ yaw


def placement_matrix(position, normal, yaw_degrees, scale):
    """The 4x4 transform LandscapeDriver gives a stamp instance:
    translate * SurfaceRotation(normal, yaw) * scale."""
    m = np.eye(4)
    m[:3, :3] = surface_rotation(normal, yaw_degrees) * scale
    m[:3, 3] = position
    return m


def sample(heights, spacing, origin, x, z):
    """Bilinear heights at world (x, z) arrays, clamped to the grid."""
    rows, cols = heights.shape
    fx = np.clip((np.asarray(x) - origin[0]) / spacing, 0, cols - 1.000001)
    fz = np.clip((np.asarray(z) - origin[1]) / spacing, 0, rows - 1.000001)
    c, r = np.floor(fx).astype(int), np.floor(fz).astype(int)
    tx, tz = fx - c, fz - r
    return ((heights[r, c] * (1 - tx) + heights[r, c + 1] * tx) * (1 - tz) +
            (heights[r + 1, c] * (1 - tx) + heights[r + 1, c + 1] * tx) * tz)


def top_edge(stamp: Stamp):
    """Points within TOP_ROWS of the topmost covered point of their stamp
    column (rows run up the face along v)."""
    covered = stamp.weight > 0.0
    rows = np.arange(stamp.height)[:, None]
    highest = np.where(covered, rows, -1).max(axis=0)
    return covered & (rows >= highest[None, :] - TOP_ROWS) & (highest[None, :] >= 0)


def add_apron(apron, heights, spacing, origin, stamp, world, outward):
    """Raises `apron` (absolute heights) in front of the stamp's foot: per
    stamp column, its lowest covered point; the ground there rises by a
    share of the rock's height above it (at most APRON_HEIGHT) and fades back
    over APRON_REACH metres outward, so the face sinks into a soft slope."""
    rows, cols = heights.shape
    covered = stamp.weight > 0.0
    if not covered.any():
        return
    index = np.arange(stamp.height)[:, None]
    lowest = np.where(covered, index, stamp.height).min(axis=0)
    has = lowest < stamp.height
    columns = np.nonzero(has)[0]
    feet = world[lowest[has], columns]
    tops = np.where(covered, world[..., 1], -np.inf).max(axis=0)[has]
    ground = sample(heights, spacing, origin, feet[:, 0], feet[:, 2])
    rise = np.minimum(APRON_HEIGHT, APRON_SHARE * np.maximum(tops - ground, 0.0))
    for d in np.arange(0.0, APRON_REACH + 1e-6, spacing * 0.5):
        x = feet[:, 0] + outward[0] * d
        z = feet[:, 2] + outward[1] * d
        t = 1.0 - d / APRON_REACH
        r = np.rint((z - origin[1]) / spacing).astype(int)
        c = np.rint((x - origin[0]) / spacing).astype(int)
        ok = (r >= 0) & (c >= 0) & (r < rows) & (c < cols)
        # Relative to the sample written, not the bilinear ground at the
        # point: over a steep step that is half the wall's height.
        height = heights[r[ok], c[ok]] + rise[ok] * t * t * (3 - 2 * t)
        np.maximum.at(apron, (r[ok], c[ok]), height)


def morph(heights, spacing, origin, placements, falloff=FALLOFF, editable=None):
    """Heights lifted to meet the placed stamps. placements: (Stamp, 4x4 matrix)
    in the grid's world frame. editable: optional boolean mask of samples that
    may change. Returns (new heights, lift).

    A height grid holds one height per sample, so each sample looks at the
    highest stamp point over its footprint (spacing x spacing around it). If
    that point is on a stamp's outline (a top edge, or the top of a side
    edge) and above the ground, the ground rises to meet it; if it is rock
    (the face, a ledge), the rock covers the sample and nothing changes, and
    lower points in the column (the face, the buried base) never count."""
    heights = np.asarray(heights, float)
    rows, cols = heights.shape
    target = np.full_like(heights, -np.inf)  # height the ground behind a top edge rises to
    apron = np.full_like(heights, -np.inf)   # height the ground in front of a foot rises to
    for stamp, matrix in placements:
        linear, offset = matrix[:3, :3], matrix[:3, 3]
        outward = linear @ stamp.direction
        flat = np.array([outward[0], outward[2]])
        if np.linalg.norm(flat) < 1e-3:
            continue
        inward = -flat / np.linalg.norm(flat)
        world = stamp.position @ linear.T + offset
        add_apron(apron, heights, spacing, origin, stamp, world, -inward)
        edge = top_edge(stamp) & (stamp.weight > 0.0) & (stamp.weight <= BORDER_WEIGHT)
        points = world[edge]
        if not len(points):
            continue
        # The ground just behind the edge (into the hill or block it is set in).
        behind = np.maximum.reduce([
            sample(heights, spacing, origin, points[:, 0] + inward[0] * d, points[:, 2] + inward[1] * d)
            for d in (0.5 * spacing, spacing, 1.5 * spacing)])
        # ... and in front of the face. An edge that stands against something
        # (ground behind well above the ground in front: a hillside, the
        # block) is a fin to fill behind; one with about the same ground on
        # both sides is a free end (past a corner, a lone rock), and raising
        # the ground to it would build a mound.
        front = np.minimum.reduce([
            sample(heights, spacing, origin, points[:, 0] - inward[0] * d, points[:, 2] - inward[1] * d)
            for d in (spacing, 2.0 * spacing)])
        against = (behind - front) >= SUPPORT * np.maximum(points[:, 1] - front, 1e-3)
        fin = (points[:, 1] > behind) & against
        if not fin.any():
            continue
        # Raise the samples behind the fin to it, if they are part of the
        # higher ground it stands against (not low ground beside a corner
        # that the bilinear "behind" sample only grazed).
        floor = front[fin] + SUPPORT * (points[fin, 1] - front[fin])
        for d in (0.5 * spacing, 1.0 * spacing):
            x = points[fin, 0] + inward[0] * d
            z = points[fin, 2] + inward[1] * d
            r = np.rint((z - origin[1]) / spacing).astype(int)
            c = np.rint((x - origin[0]) / spacing).astype(int)
            ok = (r >= 0) & (c >= 0) & (r < rows) & (c < cols)
            ok[ok] &= heights[r[ok], c[ok]] >= floor[ok]
            np.maximum.at(target, (r[ok], c[ok]), points[fin][ok, 1])
    wanted = np.where(target > heights, target - heights, 0.0)
    supported = wanted > 0.0
    lift = wanted
    # Carry each lift outward from where it applies and fade it over `falloff`,
    # so the ground rises into the stamp like a brush with a soft edge.
    if supported.any() and falloff > 0:
        distance, (near_r, near_c) = ndimage.distance_transform_edt(~supported, return_indices=True)
        t = np.clip(1.0 - distance * spacing / falloff, 0.0, 1.0)
        fade = t * t * (3 - 2 * t)
        lift = np.maximum(wanted, ndimage.gaussian_filter(wanted[near_r, near_c] * fade, KERNEL))
    lift = np.maximum(lift, np.where(apron > heights, apron - heights, 0.0))
    lift = np.maximum(lift, 0.0)
    if editable is not None:
        lift = np.where(editable, lift, 0.0)
    return heights + lift, lift


# --- vector displacement: the grid drapes over the stamps ---------------------

DISPLACE_REACH = 2.0     # grid spacings from a stamp's rock over which vertices take part
DISPLACE_INSET = 0.35    # metres the draped grid stays inside the rock (the patch draws on top)
DISPLACE_STEPS = 40      # relaxation iterations
DISPLACE_LIMIT = 0.95    # most a vertex moves sideways, in grid spacings (no folding)


def displace(heights, spacing, origin, placements, editable=None):
    """Grid vertices near the stamps' rock relax over it (Terrain.lateralOffsets).

    A 16 m wall on a 4 m grid is one grid step: vertices only at its top and
    foot, so no height edit can give the face a shape. Here each vertex near
    rock repeatedly moves to the mean of its four neighbours (in 3D) and is
    then put back on the surface: onto the nearest stamp point's tangent
    plane (DISPLACE_INSET inside it, so the stamp's own patch still draws on
    top), or onto the height field away from rock, blended by distance. The
    mean pulls rim and foot vertices onto the face, so the face gains
    vertices and the ground under every patch has the rock's shape, overhangs
    included. Sideways motion is capped at DISPLACE_LIMIT spacings and the
    grid's border never moves (neighbouring cells share it).

    heights: (rows along +z, cols along +x) metres, already morphed. Returns
    (heights, lateral) with lateral[..., 0:2] the x and z offsets."""
    from scipy.spatial import cKDTree
    heights = np.asarray(heights, float)
    rows, cols = heights.shape
    zz, xx = np.mgrid[0:rows, 0:cols].astype(float)
    base = np.stack([origin[0] + xx * spacing, heights, origin[1] + zz * spacing], -1)
    # The rock: every placed stamp's inside points and normals.
    points, normals = [], []
    for stamp, matrix in placements:
        linear, offset = matrix[:3, :3], matrix[:3, 3]
        inside = stamp.weight > BORDER_WEIGHT
        world = stamp.position[inside] @ linear.T + offset
        n = stamp.normal[inside] @ np.linalg.inv(linear)   # normals transform by the inverse transpose
        n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)
        points.append(world)
        normals.append(n)
    if not points:
        return heights, np.zeros(heights.shape + (2,))
    points, normals = np.concatenate(points), np.concatenate(normals)
    tree = cKDTree(points)
    reach = DISPLACE_REACH * spacing
    distance, _ = tree.query(base.reshape(-1, 3), distance_upper_bound=reach * 2)
    distance = distance.reshape(rows, cols)
    t = np.clip(1.0 - distance / reach, 0.0, 1.0)
    weight = t * t * (3 - 2 * t)                       # 1 at the rock, 0 at `reach`
    active = weight > 0.0
    active[0, :] = active[-1, :] = active[:, 0] = active[:, -1] = False
    if editable is not None:
        active &= editable
    if not active.any():
        return heights, np.zeros(heights.shape + (2,))
    position = base.copy()
    limit = DISPLACE_LIMIT * spacing
    index = np.nonzero(active)
    for _ in range(DISPLACE_STEPS):
        mean = 0.25 * (np.roll(position, 1, 0) + np.roll(position, -1, 0) +
                       np.roll(position, 1, 1) + np.roll(position, -1, 1))
        q = mean[index]
        # Onto the rock: the nearest stamp point's tangent plane, inset.
        _, nearest = tree.query(q)
        s, n = points[nearest], normals[nearest]
        on_rock = q - np.sum((q - s) * n, axis=1, keepdims=True) * n - DISPLACE_INSET * n
        # Onto the ground: the height field under the moved point.
        on_ground = q.copy()
        on_ground[:, 1] = sample(heights, spacing, origin, q[:, 0], q[:, 2])
        w = weight[index][:, None]
        moved = on_ground * (1 - w) + on_rock * w
        # No folding: sideways motion stays within DISPLACE_LIMIT of the grid point.
        home = base[index]
        moved[:, 0] = np.clip(moved[:, 0], home[:, 0] - limit, home[:, 0] + limit)
        moved[:, 2] = np.clip(moved[:, 2], home[:, 2] - limit, home[:, 2] + limit)
        position[index] = moved
    lateral = np.stack([position[..., 0] - base[..., 0], position[..., 2] - base[..., 2]], -1)
    return position[..., 1], lateral


def folded_triangles(heights, lateral, spacing):
    """Grid triangles (two per quad, as Terrain::Rebuild splits them) whose xz
    winding flipped against the plain grid's while facing up: folds rather
    than overhangs. Returns their count."""
    rows, cols = heights.shape
    zz, xx = np.mgrid[0:rows, 0:cols].astype(float) * spacing
    x, z = xx + lateral[..., 0], zz + lateral[..., 1]
    p = np.stack([x, heights, z], -1)
    a, b, c, d = p[:-1, :-1], p[1:, :-1], p[:-1, 1:], p[1:, 1:]
    count = 0
    for u, v, w in ((a, b, c), (c, b, d)):
        normal = np.cross(v - u, w - u)
        # The plain grid's triangles face up (+y after its winding); a triangle
        # whose normal now points down while its xz order flipped is a fold.
        count += int(np.sum(normal[..., 1] < -1e-6))
    return count


if __name__ == "__main__":
    for path in sorted(STAMPS.glob("*.stamp")):
        s = read_stamp(path)
        inside = s.weight > BORDER_WEIGHT
        p = s.position[inside]
        print(f"{s.name}: {s.width}x{s.height} points, spacing {s.spacing:.2f}, "
              f"x {p[:, 0].min():.1f}..{p[:, 0].max():.1f}, y {p[:, 1].min():.1f}..{p[:, 1].max():.1f}, "
              f"z {p[:, 2].min():.1f}..{p[:, 2].max():.1f}, direction {np.round(s.direction, 2)}, "
              f"material {s.material}")
