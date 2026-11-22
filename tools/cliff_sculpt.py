"""Sculpts a terrain cell into the shape of the cliff scans placed in it (prototype).

The terrain stamps (tools/bake_terrain_stamp.py) drew each placed scan as its
own surface patch with only its outline blended into the ground: overlapping
pieces cut through each other, and their textures, baked looking along each
piece's outward direction, smeared wherever the rock turned away from it.
Here the cell's terrain itself takes the rock's shape, as one surface:

  1. A 0.5 m grid over the cell starts on a smoothed copy of the ground (6 m),
     whose normals lean every grid point out of the slope, so a wall has room
     on the grid for its face.
  2. Each point moves along its normal to the outermost surface of the ground
     and every scan piece reaching into the cell (a march against the ground
     heights and a KD-tree of points sampled over the scans), so overlapping
     pieces merge and buried parts drop out. Toward a border with a grid cell
     the normals turn vertical and the border points are the terrain's own, so
     the cell meets that neighbour's 4 m grid; across a border with another
     sculpted cell both compute the same surface (the grid is marched and
     filtered with a margin past the border, from world positions alone).
     Turned-over quads (crossing rays) are smoothed out, and recesses darken
     (a cavity term in the vertex tint).
  3. The grid is simplified (tools/stamp_simplify.py) into three detail levels,
     the 4 m border points locked.
  4. The rock is shaded with a seamless rock material cut from the scans
     (tools/cliff_rock_texture.py), projected from three sides in the terrain
     shader (Object.hlsl), so no surface direction smears it. Each point
     carries the scans' colour there, blurred, as a tint on that material, and
     a rock mask: its blend from the paint layers.

Writes Assets/TerrainData/Sculpts/cell_<x>_<z>.sculpt.

  python tools/cliff_sculpt.py -1,-6 0,-6 1,-6
"""

import json
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree

import bake_terrain_stamp
import stamp_simplify
from landscape_cliffs import rotation
from woodland_assets import ASSETS, read_native_mesh

DATA = ASSETS / "TerrainData" / "Landscape"
OUTPUT = ASSETS / "TerrainData" / "Sculpts"
QUAD = 4.0              # terrain grid spacing (m)
CELL = 128.0            # cell size (m)
DENSE = 0.25            # sculpt grid spacing (m)
REACH = 30.0            # how far a point may move along its normal, each way (m)
STEP = 0.15             # march step (m)
NEAR = 0.2              # within this of a scan sample counts as on the rock (m)
SAMPLE = 0.12           # spacing of the points sampled over the scans (m)
SMOOTH = 6.0            # smoothing of the ground the normals come from (m, sigma)
EDGE = 8.0              # border band over which normals turn vertical (m)
TINT_BLUR = 0.75        # the scans' colour, blurred over this (m, sigma)
MARGIN = 48             # grid points computed past each border (12 m), for the filters
FOLD_PASSES = 6         # median passes over turned-over quads
UNTANGLE_PASSES = 20    # then passes moving their points to their neighbours' mean (< MARGIN)
CAVITY_RADIUS = 2.5     # surroundings a point is compared with (m, sigma)
CAVITY_STRENGTH = 0.9   # darkening per metre a point sits below them
CAVITY_FLOOR = 0.45     # darkest a cavity gets
LEVELS = (0.08, 0.2, 0.5)  # detail levels: largest surface error (m)
MAGIC = int.from_bytes(b"TRNRSCL1", "little")
ROCK_MATERIAL = OUTPUT / "cliff_rock.material"  # tools/cliff_rock_texture.py


# --- inputs -------------------------------------------------------------------

def read_fine_heights():
    """Full-resolution ground heights of the play area (metres) and the world
    x/z of sample [0, 0]."""
    raw = (DATA / "world.heights").read_bytes()
    magic, version, signature, extent, cx, cy, scale, fine, coarse = struct.unpack_from("<QIQiiifii", raw, 0)
    at = struct.calcsize("<QIQiiifii")

    def cell_bytes(stride):
        n = (cx // stride + 1) * (cy // stride + 1)
        return n * 2 + (n + 3) // 4

    side = 2 * fine + 1
    heights = np.zeros((side * cy + 1, side * cx + 1), np.float64)
    for row in range(-extent, extent + 1):
        for col in range(-extent, extent + 1):
            stride = 1 if max(abs(col), abs(row)) <= fine else coarse
            if stride == 1:
                block = np.frombuffer(raw, "<i2", (cx + 1) * (cy + 1), at).reshape(cy + 1, cx + 1) / scale
                r0, c0 = (row + fine) * cy, (col + fine) * cx
                heights[r0:r0 + cy + 1, c0:c0 + cx + 1] = block
            at += cell_bytes(stride)
    return heights, -fine * cx * QUAD, signature


def read_pieces():
    raw = (DATA / "world.cliffs").read_bytes()
    count = struct.unpack_from("<QIQiI", raw, 0)[4]
    pieces = []
    for i in range(count):
        cx, cz, shape, _, _, x, y, z, nx, ny, nz, yaw, scale = struct.unpack_from("<hhBBH8f", raw, 28 + i * 40)
        if shape < len(bake_terrain_stamp.SHAPES):
            pieces.append((i, shape, np.array([cx * CELL + x, y, cz * CELL + z]),
                           rotation((nx, ny, nz), yaw) * scale))
    return pieces


def distance_to_region_edge(x, z, sculpted):
    """Distance from world x, z to the nearest border between a sculpted cell
    and a grid cell (0 on it): the same for every sculpted cell."""
    best = np.full(np.shape(x), np.inf)
    for cx, cz in sculpted:
        x0, z0 = cx * CELL, cz * CELL
        for (dx, dz), (a, b) in (((-1, 0), ((x0, z0), (x0, z0 + CELL))),
                                 ((1, 0), ((x0 + CELL, z0), (x0 + CELL, z0 + CELL))),
                                 ((0, -1), ((x0, z0), (x0 + CELL, z0))),
                                 ((0, 1), ((x0, z0 + CELL), (x0 + CELL, z0 + CELL)))):
            if (cx + dx, cz + dz) in sculpted:
                continue
            ex = np.clip(x, min(a[0], b[0]), max(a[0], b[0]))
            ez = np.clip(z, min(a[1], b[1]), max(a[1], b[1]))
            best = np.minimum(best, np.hypot(x - ex, z - ez))
    return best


def bilinear_grid(grid, origin, x, z):
    c = (x - origin) / QUAD
    r = (z - origin) / QUAD
    c0 = np.clip(np.floor(c).astype(np.int64), 0, grid.shape[1] - 2)
    r0 = np.clip(np.floor(r).astype(np.int64), 0, grid.shape[0] - 2)
    tx, tz = c - c0, r - r0
    return ((grid[r0, c0] * (1 - tx) + grid[r0, c0 + 1] * tx) * (1 - tz) +
            (grid[r0 + 1, c0] * (1 - tx) + grid[r0 + 1, c0 + 1] * tx) * tz)


def sample_image(image, uv):
    """Bilinear samples of a uint8 (h, w, c) image at uv (n, 2), wrapping; 0..1 floats."""
    h, w = image.shape[:2]
    x = (uv[:, 0] % 1.0) * w - 0.5
    y = (uv[:, 1] % 1.0) * h - 0.5
    x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    x0, y0 = x0 % w, y0 % h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    get = lambda yy, xx: image[yy, xx].astype(np.float32) / 255.0
    return ((get(y0, x0) * (1 - fx) + get(y0, x1) * fx) * (1 - fy) +
            (get(y1, x0) * (1 - fx) + get(y1, x1) * fx) * fy)


# --- the scans as points --------------------------------------------------------

class Scan:
    """One scan shape: its mesh (LOD1), textures and tint."""

    def __init__(self, name):
        positions, normals, texcoords, _, triangles = read_native_mesh(
            bake_terrain_stamp.MESHES / f"{name}_LOD1.mesh")
        self.positions, self.normals, self.texcoords, self.triangles = positions, normals, texcoords, triangles
        material = json.loads((Path(ASSETS).parent.parent.parent /
                               bake_terrain_stamp.scan_material(name)).read_text())
        resolve = lambda path: Path(ASSETS) / path.removeprefix("Assets/")
        load = lambda key: np.asarray(Image.open(resolve(material[key])).convert("RGB"))
        self.albedo, self.normal_map, self.rough = load("baseColorTexture"), load("normalTexture"), \
            load("metallicRoughnessTexture")
        self.tint = np.asarray(material.get("baseColor", [1.0, 1.0, 1.0]), np.float32)
        # Per-triangle tangent frame from the UVs (T along +u, B along +v).
        p0, p1, p2 = (positions[triangles[:, k]] for k in range(3))
        t0, t1, t2 = (texcoords[triangles[:, k]] for k in range(3))
        e1, e2, d1, d2 = p1 - p0, p2 - p0, t1 - t0, t2 - t0
        r = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
        r = np.where(np.abs(r) < 1e-12, 1e-12, r)[:, None]
        self.tangent = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) / r
        self.bitangent = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) / r
        self.area = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)


def sample_piece(scan, rotation_scale, position, rng):
    """Points over one placed scan: world position, linear albedo, world
    normal (its normal map applied), roughness."""
    scale = np.linalg.norm(rotation_scale[:, 0])
    counts = rng.poisson(scan.area * scale * scale / (SAMPLE * SAMPLE))
    tri = np.repeat(np.arange(len(scan.triangles)), counts)
    if not len(tri):
        return None
    a, b = rng.random(len(tri)), rng.random(len(tri))
    flip = a + b > 1.0
    a[flip], b[flip] = 1.0 - a[flip], 1.0 - b[flip]
    bary = np.stack([1.0 - a - b, a, b], 1)[..., None]
    corners = scan.triangles[tri]
    local = (scan.positions[corners] * bary).sum(1)
    uv = (scan.texcoords[corners] * bary).sum(1)
    smooth = (scan.normals[corners] * bary).sum(1)
    smooth /= np.maximum(np.linalg.norm(smooth, axis=1, keepdims=True), 1e-9)
    tangent = scan.tangent[tri] - smooth * (scan.tangent[tri] * smooth).sum(1, keepdims=True)
    tangent /= np.maximum(np.linalg.norm(tangent, axis=1, keepdims=True), 1e-9)
    bitangent = scan.bitangent[tri] - smooth * (scan.bitangent[tri] * smooth).sum(1, keepdims=True)
    bitangent -= tangent * (bitangent * tangent).sum(1, keepdims=True)
    bitangent /= np.maximum(np.linalg.norm(bitangent, axis=1, keepdims=True), 1e-9)
    mapped = sample_image(scan.normal_map, uv) * 2.0 - 1.0
    bent = tangent * mapped[:, 0:1] - bitangent * mapped[:, 1:2] + smooth * mapped[:, 2:3]
    bent /= np.maximum(np.linalg.norm(bent, axis=1, keepdims=True), 1e-9)
    srgb = sample_image(scan.albedo, uv)
    linear = np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4) * scan.tint
    rough = sample_image(scan.rough, uv)[:, 1]
    rotation_only = rotation_scale / scale
    return (local @ rotation_scale.T + position, linear.astype(np.float32),
            (bent @ rotation_only.T).astype(np.float32), rough.astype(np.float32))


# --- sculpting ------------------------------------------------------------------

def sculpt(cell_x, cell_z, sculpted=()):
    """sculpted: the cells that are (or will be) sculpted too. A border shared
    with one of them keeps the marched surface (both cells march it the same
    way: vertical rays at the border, the same scan points); a border with a
    grid cell is that grid's own ground."""
    started = time.time()
    heights, origin, signature = read_fine_heights()
    smooth_heights = ndimage.gaussian_filter(heights, SMOOTH / QUAD)
    x0, z0 = cell_x * CELL, cell_z * CELL
    margin = REACH + 25.0

    scans = {}
    points, albedo, normals, rough = [], [], [], []
    for piece, shape, position, rotation_scale in read_pieces():
        scale = np.linalg.norm(rotation_scale[:, 0])
        reach = scale * 12.0
        if not (x0 - margin - reach < position[0] < x0 + CELL + margin + reach and
                z0 - margin - reach < position[2] < z0 + CELL + margin + reach):
            continue
        name = bake_terrain_stamp.SHAPES[shape][0]
        if name not in scans:
            scans[name] = Scan(name)
        # Seeded per piece: neighbouring cells sample it identically.
        sampled = sample_piece(scans[name], rotation_scale, position, np.random.default_rng(piece))
        if sampled is None:
            continue
        keep = ((sampled[0][:, 0] > x0 - margin) & (sampled[0][:, 0] < x0 + CELL + margin) &
                (sampled[0][:, 2] > z0 - margin) & (sampled[0][:, 2] < z0 + CELL + margin))
        for out, value in zip((points, albedo, normals, rough), sampled):
            out.append(value[keep])
    if not points:
        raise SystemExit("no cliff pieces reach this cell")
    points = np.concatenate(points)
    albedo, normals, rough = np.concatenate(albedo), np.concatenate(normals), np.concatenate(rough)
    tree = cKDTree(points)
    print(f"  {len(points):,} scan points from {len(scans)} shapes ({time.time() - started:.0f} s)")

    # The grid, with a margin past the cell's borders: everything filtered
    # over the grid (folds, normals, mask, tint, cavities) is computed from
    # world positions alone, so a cell and its sculpted neighbour reach the
    # same values along their border.
    n = int(round(CELL / DENSE)) + 1
    full = n + 2 * MARGIN
    axis = (np.arange(full) - MARGIN) * DENSE
    lz, lx = np.meshgrid(axis, axis, indexing="ij")
    wx, wz = (x0 + lx).ravel(), (z0 + lz).ravel()
    ground = bilinear_grid(heights, origin, wx, wz)
    soft = bilinear_grid(smooth_heights, origin, wx, wz)
    slope_x = (bilinear_grid(smooth_heights, origin, wx + 1.0, wz) -
               bilinear_grid(smooth_heights, origin, wx - 1.0, wz)) / 2.0
    slope_z = (bilinear_grid(smooth_heights, origin, wx, wz + 1.0) -
               bilinear_grid(smooth_heights, origin, wx, wz - 1.0)) / 2.0
    lean = np.stack([-slope_x, np.ones_like(slope_x), -slope_z], 1)
    lean /= np.linalg.norm(lean, axis=1, keepdims=True)
    # Directions turn vertical only toward the sculpted region's edge (a
    # border with a grid cell); across a border between sculpted cells they
    # keep leaning, and the rock is not streaked there.
    border = distance_to_region_edge(wx, wz, sculpted)
    blend = np.clip(border / EDGE, 0.0, 1.0)
    blend = blend * blend * (3.0 - 2.0 * blend)
    direction = lean * blend[:, None] + np.array([0.0, 1.0, 0.0]) * (1.0 - blend[:, None])
    direction /= np.linalg.norm(direction, axis=1, keepdims=True)
    start = np.stack([wx, ground + (soft - ground) * blend, wz], 1)

    # March in from REACH: the first step under the ground or on the rock.
    steps = np.arange(REACH, -REACH - STEP, -STEP)
    hit_t = np.full(len(start), -REACH)
    on_rock = np.zeros(len(start), bool)
    near_rock = tree.query(start, distance_upper_bound=REACH + 2.0, workers=-1)[0] < np.inf
    pending = np.ones(len(start), bool)
    for t in steps:
        index = np.nonzero(pending)[0]
        if not len(index):
            break
        p = start[index] + direction[index] * t
        under = p[:, 1] < bilinear_grid(heights, origin, p[:, 0], p[:, 2])
        rock = np.zeros(len(index), bool)
        check = near_rock[index] & ~under
        if check.any():
            rock[check] = tree.query(p[check], distance_upper_bound=NEAR, workers=-1)[0] < np.inf
        done = under | rock
        hit_t[index[done]] = t
        on_rock[index[rock]] = True
        pending[index[done]] = False

    # Folds: where neighbouring rays cross, a quad turns over (a bright
    # sliver). Their points take the median distance of their neighbours
    # until none is turned over.
    hit_t = hit_t.reshape(full, full)
    start_grid, direction_grid = start.reshape(full, full, 3), direction.reshape(full, full, 3)
    folds = 0
    for _ in range(FOLD_PASSES):
        grid = start_grid + direction_grid * hit_t[..., None]
        quad = np.cross(grid[1:, :-1] - grid[:-1, :-1], grid[:-1, 1:] - grid[:-1, :-1])
        folded = (quad * direction_grid[:-1, :-1]).sum(-1) < 0.0
        folds = int(folded.sum())
        if not folds:
            break
        touched = np.zeros((full, full), bool)
        touched[:-1, :-1] |= folded
        touched[1:, :-1] |= folded
        touched[:-1, 1:] |= folded
        touched[1:, 1:] |= folded
        hit_t = np.where(touched, ndimage.median_filter(hit_t, size=3, mode="nearest"), hit_t)
    grid = start_grid + direction_grid * hit_t[..., None]
    # Folds the medians leave (rays converging into a narrow crevice) are
    # untangled in 3D: their points move to the mean of their neighbours,
    # off their rays, until no quad is turned over. A turned-over quad faces
    # away and is culled: a hole.
    for _ in range(UNTANGLE_PASSES):
        quad = np.cross(grid[1:, :-1] - grid[:-1, :-1], grid[:-1, 1:] - grid[:-1, :-1])
        folded = (quad * direction_grid[:-1, :-1]).sum(-1) < 0.0
        folds = int(folded.sum())
        if not folds:
            break
        touched = np.zeros((full, full), bool)
        touched[:-1, :-1] |= folded
        touched[1:, :-1] |= folded
        touched[:-1, 1:] |= folded
        touched[1:, 1:] |= folded
        touched = ndimage.binary_dilation(touched)
        mean = np.stack([ndimage.uniform_filter(grid[..., k], 3, mode="nearest") for k in range(3)], -1)
        grid = np.where(touched[..., None], mean, grid)
    rock_grid = on_rock.reshape(full, full)
    # The region's edge is the grid cells' own ground.
    edge = (border < 1e-6).reshape(full, full)
    grid = np.where(edge[..., None], np.stack([wx, ground, wz], 1).reshape(full, full, 3), grid)
    rock_grid = rock_grid & ~edge

    mask = ndimage.gaussian_filter(rock_grid.astype(np.float64), 1.0)
    mask[edge] = 0.0
    du = np.gradient(grid, axis=1)
    dv = np.gradient(grid, axis=0)
    vertex_normal = np.cross(dv, du)
    vertex_normal /= np.maximum(np.linalg.norm(vertex_normal, axis=2, keepdims=True), 1e-9)
    # Cavities: how far a point sits below the surface around it, along its
    # normal; recesses between blocks darken, as the gaps between the scans
    # did.
    around = np.stack([ndimage.gaussian_filter(grid[..., k], CAVITY_RADIUS / DENSE) for k in range(3)], -1)
    depth = ((around - grid) * vertex_normal).sum(-1)
    occlusion = np.clip(1.0 - CAVITY_STRENGTH * np.maximum(depth, 0.0), CAVITY_FLOOR, 1.0)
    # The scans' colour, lightly blurred, as a tint on the rock material
    # (its mean colour = 1), times the cavities.
    rock_mean = np.asarray(json.loads(ROCK_MATERIAL.read_text())["terrainMacroColor"], np.float64)
    colour = albedo[tree.query(grid.reshape(-1, 3), workers=-1)[1]].reshape(full, full, 3).astype(np.float64)
    colour = np.stack([ndimage.gaussian_filter(colour[..., k], TINT_BLUR / DENSE) for k in range(3)], -1)
    tint = np.clip(colour / rock_mean, 0.4, 2.5) * occlusion[..., None]

    # The cell's own part.
    inner = slice(MARGIN, MARGIN + n)
    grid = grid[inner, inner]
    vertex_normal = vertex_normal[inner, inner]
    mask = mask[inner, inner]
    tint = tint[inner, inner]
    rock_share = rock_grid[inner, inner].mean()
    print(f"  marched {full * full:,} points, {rock_share * 100:.1f}% on rock, {folds} quads still folded "
          f"({time.time() - started:.0f} s)")

    # Triangles, wound up (as the terrain grid: a, next row, next column).
    index = np.arange(n * n).reshape(n, n)
    a, b, c, d = index[:-1, :-1], index[:-1, 1:], index[1:, :-1], index[1:, 1:]
    triangles = np.concatenate([np.stack([a.ravel(), c.ravel(), b.ravel()], 1),
                                np.stack([b.ravel(), c.ravel(), d.ravel()], 1)])
    # Borders with grid cells keep their 4 m points (the grid's own); borders
    # with sculpted cells keep every point, so both sides hold the same ones
    # at every level (a point one side merged away would leave a crack).
    step = int(round(QUAD / DENSE))
    locked = np.zeros((n, n), bool)
    for side, (dx, dz) in (((slice(None), 0), (-1, 0)), ((slice(None), -1), (1, 0)),
                           ((0, slice(None)), (0, -1)), ((-1, slice(None)), (0, 1))):
        locked[side] = (np.arange(n) % step == 0) | ((cell_x + dx, cell_z + dz) in sculpted)
    levels = stamp_simplify.simplify_levels(grid.reshape(-1, 3), mask.ravel(), triangles,
                                            LEVELS, locked=locked.ravel())
    print(f"  levels {[len(level) for level in levels]} triangles ({time.time() - started:.0f} s)")
    return grid, vertex_normal, mask, levels, tint


def write(cell_x, cell_z, grid, vertex_normal, mask, levels, tint):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    name = f"cell_{cell_x}_{cell_z}"
    n = grid.shape[0]
    material = json.loads(ROCK_MATERIAL.read_text())
    material_path = "Assets/" + ROCK_MATERIAL.resolve().relative_to(Path(ASSETS).resolve()).as_posix()
    # Only the points the finest level uses, renumbered.
    used = np.unique(levels[0])
    remap = np.full(n * n, -1, np.int64)
    remap[used] = np.arange(len(used))
    positions = grid.reshape(-1, 3)[used] - np.array([cell_x * CELL, 0.0, cell_z * CELL])
    vertices = np.concatenate([positions, vertex_normal.reshape(-1, 3)[used], tint.reshape(-1, 3)[used],
                               mask.ravel()[used, None]], 1).astype("<f4")
    encoded_material = material_path.encode("utf-8")
    with open(OUTPUT / f"{name}.sculpt", "wb") as file:
        file.write(struct.pack("<QIiiIIfI", MAGIC, 2, cell_x, cell_z, len(used), len(levels),
                               float(material["terrainUvScale"]), len(encoded_material)))
        file.write(encoded_material)
        file.write(vertices.tobytes())
        for level in levels:
            renumbered = remap[level]
            assert (renumbered >= 0).all()
            file.write(struct.pack("<I", renumbered.size))
            file.write(renumbered.astype("<u4").tobytes())
    print(f"  wrote {name}: {len(used):,} vertices, rock material {material_path}")


def main():
    """python tools/cliff_sculpt.py X,Z [X,Z ...]: sculpts each listed cell,
    treating all of them (and cells already sculpted) as sculpted neighbours.
    --only X,Z sculpts just that one of the list (to run cells in parallel)."""
    arguments = sys.argv[1:]
    only = None
    if "--only" in arguments:
        at = arguments.index("--only")
        only = tuple(int(v) for v in arguments[at + 1].split(","))
        del arguments[at:at + 2]
    cells = [tuple(int(v) for v in argument.split(",")) for argument in arguments] or [(0, -6)]
    sculpted = set(cells)
    for path in OUTPUT.glob("cell_*.sculpt"):
        x, z = path.stem.split("_")[1:3]
        sculpted.add((int(x), int(z)))
    for cell_x, cell_z in cells:
        if only and (cell_x, cell_z) != only:
            continue
        print(f"sculpting cell {cell_x},{cell_z}")
        write(cell_x, cell_z, *sculpt(cell_x, cell_z, sculpted))


if __name__ == "__main__":
    main()
