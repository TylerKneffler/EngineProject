"""Cliffs for the landscape demo, placed when the map is built (.md/todo.md,
"Cliffs as terrain stamps", stage 2).

LandscapeDriver used to place the photo-scanned rock faces at runtime
(`GenerateCliffs`). The stamps that replace them (tools/bake_terrain_stamp.py,
drawn by the terrain as patches that close onto the ground) need the same
placements wherever they are drawn, so tools/build_landscape_demo.py now runs
the runtime rules here, ported (steep rock below the snow, slope rising behind,
rock paint, seeds growing along the contour, the same random stream per cell
and the same shape table), on the play area's 4 m heights, and writes them to
world.cliffs next to the archive. LandscapeDriver reads them for the cells they
cover instead of placing cliffs itself. The heights then conform to the
pieces (conform_heights, tools/terrain_stamp_morph.py): the ground rises to
each stamp's top edge and into an apron at its foot. Stamping the rock shape
itself into the 4 m height map was tried 2026-11-21: spikes, and no overhangs.

Cells outside the play area (16 m heights, 2.9 km and more from the start)
keep runtime placement.

world.cliffs, little endian:
  u64 magic "TRNRCLF1", u32 version 1, u64 archive signature,
  i32 extent (cells with |x| and |z| <= extent are covered), u32 count,
  then count records of 40 bytes, sorted by cell:
  i16 cell x, i16 cell z, u8 shape (index into bake_terrain_stamp.SHAPES),
  u8 0, u16 0, f32 x, y, z (cell-local, metres), f32 normal x, y, z,
  f32 yaw (degrees), f32 scale.
"""

import math
import struct

import numpy as np

import bake_terrain_stamp
import terrain_stamp_morph

CLIFFS_MAGIC = int.from_bytes(b"TRNRCLF1", "little")
# Keep in sync with the scene's LandscapeDriver (waterLevel, rockLine,
# snowLine, treeLine, altitudeBlend).
WATER, ROCK, SNOW, TREE, BLEND = -40.0, 1150.0, 1480.0, 950.0, 6.0
MAXIMUM_BLOCKS = 64   # per cell, as at runtime
ATTEMPTS = 220


# --- LandscapeDriver.cpp helpers, ported ------------------------------------

MASK32 = 0xFFFFFFFF


def hash32(x, z, seed):
    h = ((x & MASK32) * 0x9E3779B1 & MASK32) ^ ((z & MASK32) * 0x85EBCA77 & MASK32) ^ \
        (seed * 0xC2B2AE3D & MASK32)
    h ^= h >> 15
    h = h * 0x2C1B3C6D & MASK32
    h ^= h >> 12
    h = h * 0x297A2D39 & MASK32
    h ^= h >> 15
    return h


def value_noise(x, z, period, seed):
    x, z = x / period, z / period
    fx, fz = math.floor(x), math.floor(z)
    ix, iz = int(fx), int(fz)
    tx, tz = x - fx, z - fz
    tx, tz = tx * tx * (3 - 2 * tx), tz * tz * (3 - 2 * tz)
    corner = lambda dx, dz: (hash32(ix + dx, iz + dz, seed) & 0xFFFFFF) / float(0xFFFFFF)
    return ((corner(0, 0) * (1 - tx) + corner(1, 0) * tx) * (1 - tz) +
            (corner(0, 1) * (1 - tx) + corner(1, 1) * tx) * tz)


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def forest_band(height):
    return smoothstep(WATER + 4, WATER + 9, height) * (1 - smoothstep(TREE - 8 * BLEND, TREE, height))


def trail_mask(x, z):
    field = lambda px, pz: 0.7 * value_noise(px, pz, 520.0, 71) + 0.3 * value_noise(px, pz, 170.0, 73)
    e = 4.0
    gx = (field(x + e, z) - field(x - e, z)) / (2 * e)
    gz = (field(x, z + e) - field(x, z - e)) / (2 * e)
    distance = abs(field(x, z) - 0.5) / max(math.hypot(gx, gz), 0.0006)
    return 1.0 - smoothstep(1.6, 4.2, distance)


def rock_weight(height, rock_class, x, z):
    """PaintSample's Rock weight on dry land (the only ground cliffs stand on:
    25 m and more above the water), where the weights already sum to one."""
    detail = value_noise(x, z, 22.0, 51) - 0.5
    land = smoothstep(WATER + 1.5, WATER + 4.0, height)
    rock_slope = rock_class / 3.0
    rock_high = smoothstep(ROCK - 8 * BLEND, ROCK + 8 * BLEND, height + detail * 14 * BLEND)
    rock = land * max(rock_slope, rock_high)
    snow = smoothstep(SNOW - 6 * BLEND, SNOW + 6 * BLEND, height + detail * 10 * BLEND) * \
        (1 - 0.55 * rock_slope)
    weight = rock * (1 - snow)
    trail = trail_mask(x, z) * land * (1 - rock) * (1 - snow) * forest_band(height) \
        if rock < 1 else 0.0
    return weight * (1 - 0.9 * trail)


def sample_surface(heights, x, z, spacing):
    """Terrain::Rebuild's two triangles per quad (SampleTerrainSurface)."""
    cells = heights.shape[0] - 1
    sx = min(max(x / spacing, 0.0), float(cells))
    sz = min(max(z / spacing, 0.0), float(cells))
    ix, iz = min(int(sx), cells - 1), min(int(sz), cells - 1)
    tx, tz = sx - ix, sz - iz
    a, b = heights[iz, ix], heights[iz, ix + 1]
    c, d = heights[iz + 1, ix], heights[iz + 1, ix + 1]
    if tx + tz <= 1.0:
        dx, dz = b - a, c - a
        height = a + dx * tx + dz * tz
    else:
        dx, dz = d - c, d - b
        height = d - dx * (1 - tx) - dz * (1 - tz)
    n = np.array([-dx / spacing, 1.0, -dz / spacing])
    return height, n / np.linalg.norm(n)


# --- placement ---------------------------------------------------------------

def place_cell(cell_x, cell_z, heights, codes, spacing):
    """GenerateCliffs for one cell: heights and codes are its (33, 33) samples
    (rows along +z). Returns (shape, local position, normal, yaw, scale)."""
    cells = heights.shape[0] - 1
    width = cells * spacing
    origin_x, origin_z = cell_x * width, cell_z * width
    state = hash32(cell_x, cell_z, 0x636C6966) | 1

    def random():
        nonlocal state
        state ^= (state << 13) & MASK32
        state ^= state >> 17
        state ^= (state << 5) & MASK32
        return (state & 0xFFFFFF) / float(0x1000000)

    def steep_rock(x, z):
        if x < 2 or z < 2 or x > width - 2 or z > width - 2:
            return None
        height, normal = sample_surface(heights, x, z, spacing)
        if normal[1] > 0.79 or normal[1] < 0.08:
            return None
        if height < WATER + 25 or height > SNOW:
            return None
        slope = np.array([normal[0], normal[2]])
        uphill = -slope / max(np.linalg.norm(slope), 1e-4)
        for reach in (14.0, 28.0):
            px, pz = x + uphill[0] * reach, z + uphill[1] * reach
            # Off the cell: the runtime sampler clamps to the cell's edge.
            behind, _ = sample_surface(heights, px, pz, spacing)
            if behind < height + reach * 0.55:
                return None
        col = min(max(int(math.floor(x / spacing + 0.5)), 0), cells)
        row = min(max(int(math.floor(z / spacing + 0.5)), 0), cells)
        if rock_weight(heights[row, col], codes[row, col], origin_x + col * spacing,
                       origin_z + row * spacing) < 0.35:
            return None
        return height, normal

    placements = []

    def place_block(x, z, height, normal, size, pick, turn):
        shape = 0 if pick < 0.3 else 1 if pick < 0.55 else 2 if pick < 0.72 else 3 if pick < 0.88 else 4
        _, half_width, half_depth, base_scale = bake_terrain_stamp.SHAPES[shape]
        scale = base_scale * (0.75 + 0.6 * size)
        gradient = math.sqrt(max(0.0, 1 - normal[1] * normal[1])) / normal[1]
        sink = min(16.0, half_depth * scale * min(gradient, 1.6) * 0.8)
        lean = np.array([0.0, 1.0, 0.0]) * 0.65 + normal * 0.35
        lean /= np.linalg.norm(lean)
        yaw = math.degrees(math.atan2(normal[0], normal[2])) + (turn - 0.5) * 50.0
        placements.append((shape, (x, height - sink, z), tuple(lean), yaw, scale))
        return half_width * scale

    placed = 0
    for _ in range(ATTEMPTS):
        if placed >= MAXIMUM_BLOCKS:
            break
        x = (0.04 + 0.92 * random()) * width
        z = (0.04 + 0.92 * random()) * width
        yaw, size, pick = random(), random(), random()
        found = steep_rock(x, z)
        if found is None:
            continue
        height, normal = found
        seed_half_width = place_block(x, z, height, normal, size, pick, yaw)
        placed += 1
        slope = np.array([normal[0], normal[2]])
        contour = np.array([-slope[1], slope[0]])
        contour /= np.linalg.norm(contour)
        for direction in (1.0, -1.0):
            bx, bz, previous = x, z, seed_half_width
            for _ in range(3):
                if placed >= MAXIMUM_BLOCKS:
                    break
                next_size, next_pick, next_turn, drift = random(), random(), random(), random()
                stride = previous * 1.25 + 3.0
                bx += contour[0] * stride * direction + slope[0] * (drift - 0.5) * 8.0
                bz += contour[1] * stride * direction + slope[1] * (drift - 0.5) * 8.0
                found = steep_rock(bx, bz)
                if found is None:
                    break
                previous = place_block(bx, bz, found[0], found[1], next_size, next_pick, next_turn)
                placed += 1
    return placements


def place_cliffs(fine_q, scale_units, fine_extent, cells, spacing, rock_class):
    """Places cliffs on the play area (fine_q: quantized 4 m heights, rows along
    +z, scale_units stored units per metre). Returns the placements by cell and
    the extent they cover."""
    heights = fine_q / scale_units
    codes = rock_class(heights, spacing)
    gz, gx = np.gradient(heights, spacing)
    steep = np.hypot(gx, gz) > math.tan(math.acos(0.79))
    by_cell = {}
    for cell_z in range(-fine_extent, fine_extent + 1):
        for cell_x in range(-fine_extent, fine_extent + 1):
            r0, c0 = (cell_z + fine_extent) * cells, (cell_x + fine_extent) * cells
            if not steep[r0:r0 + cells + 1, c0:c0 + cells + 1].any():
                continue
            found = place_cell(cell_x, cell_z, heights[r0:r0 + cells + 1, c0:c0 + cells + 1],
                               codes[r0:r0 + cells + 1, c0:c0 + cells + 1], spacing)
            if found:
                by_cell[(cell_x, cell_z)] = found
    count = sum(len(v) for v in by_cell.values())
    print(f"cliffs: {count} pieces in {len(by_cell)} cells")
    return by_cell, fine_extent


# --- conforming the heights ----------------------------------------------------

def rotation(normal, yaw_degrees):
    """TerrainObjectLayer::SurfaceRotation: up turned onto normal (shortest
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


def conform_heights(fine_q, scale_units, fine_extent, cells, spacing, by_cell, lift=True):
    """The terrain conforms to its cliffs: tools/terrain_stamp_morph.py lifts
    the ground behind each stamp's top edge to meet it (no rock fins above the
    hill) and raises a low apron at its foot, both faded out like a brush, so
    the ground runs into the rock instead of meeting it at a seam. Heights are
    never lowered, and the play area's outer two cells are left alone (they
    blend into the coarse cells). Returns the new quantized heights and the
    samples under rock and apron (painted rock and scree).

    lift=False leaves the heights as they are (the volume terrain unions the
    pieces with the plain ground itself) and only marks the rock; no apron."""
    heights = fine_q / scale_units
    rows, cols = heights.shape
    origin = -fine_extent * cells * spacing
    placements = []
    under = np.zeros(heights.shape, bool)
    for (cell_x, cell_z), pieces in by_cell.items():
        for shape, (x, y, z), normal, yaw, scale in pieces:
            stamp = terrain_stamp_morph.load_stamp(bake_terrain_stamp.SHAPES[shape][0])
            position = (cell_x * cells * spacing + x, y, cell_z * cells * spacing + z)
            matrix = terrain_stamp_morph.placement_matrix(position, normal, yaw, scale)
            placements.append((stamp, matrix))
            inside = stamp.weight > terrain_stamp_morph.BORDER_WEIGHT
            world = stamp.position[inside] @ matrix[:3, :3].T + matrix[:3, 3]
            r = np.rint((world[:, 2] - origin) / spacing).astype(np.int64)
            c = np.rint((world[:, 0] - origin) / spacing).astype(np.int64)
            ok = (r >= 0) & (c >= 0) & (r < rows) & (c < cols)
            under[r[ok], c[ok]] = True
    span = rows - 1
    edge = np.minimum(np.arange(span + 1), span - np.arange(span + 1))
    inner = edge >= 2 * cells
    editable = inner[:, None] & inner[None, :]
    if not lift:
        rock = under & editable
        print(f"cliffs: heights left plain, {int(rock.sum())} samples painted rock, {len(placements)} stamps")
        return fine_q, rock, np.zeros(heights.shape, bool)
    raised, lift = terrain_stamp_morph.morph(heights, spacing, (origin, origin), placements,
                                             editable=editable)
    changed = lift > 0.5 / scale_units
    out = np.where(changed, np.clip(np.rint(raised * scale_units), -32768, 32767), fine_q)
    rock = under & editable
    apron = (lift > 0.2) & ~rock & editable
    lifted = lift > 0.03
    print(f"cliffs conform: {int(lifted.sum())} samples raised "
          f"(mean {lift[lifted].mean() if lifted.any() else 0:.1f} m, max {lift.max():.1f} m), "
          f"{len(placements)} stamps")
    return out.astype(fine_q.dtype), rock, apron


def write_cliffs(path, signature, by_cell, extent):
    records = []
    for (cell_x, cell_z) in sorted(by_cell, key=lambda cell: (cell[1], cell[0])):
        for shape, position, normal, yaw, scale in by_cell[(cell_x, cell_z)]:
            records.append(struct.pack("<hhBBH8f", cell_x, cell_z, shape, 0, 0, *position,
                                       *normal, yaw, scale))
    with open(path, "wb") as file:
        file.write(struct.pack("<QIQiI", CLIFFS_MAGIC, 1, signature, extent, len(records)))
        file.write(b"".join(records))
