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


if __name__ == "__main__":
    for path in sorted(STAMPS.glob("*.stamp")):
        s = read_stamp(path)
        inside = s.weight > BORDER_WEIGHT
        p = s.position[inside]
        print(f"{s.name}: {s.width}x{s.height} points, spacing {s.spacing:.2f}, "
              f"x {p[:, 0].min():.1f}..{p[:, 0].max():.1f}, y {p[:, 1].min():.1f}..{p[:, 1].max():.1f}, "
              f"z {p[:, 2].min():.1f}..{p[:, 2].max():.1f}, direction {np.round(s.direction, 2)}, "
              f"material {s.material}")
