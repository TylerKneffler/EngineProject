"""Builds the landscape demo map: eroded mountain ranges, forests and lake basins.

Writes
  Engine/Core/Assets/TerrainData/Landscape/world.heights      height archive (v3)
  Engine/Core/Assets/TerrainData/Landscape/world.fargrid      far silhouette grid (v3)
  Engine/Core/Assets/TerrainData/Landscape/world.flow         erosion flow map
  Engine/Core/Assets/TerrainData/Landscape/world.deposition   erosion deposition map
  Engine/Core/Assets/Scenes/terrain/landscape_demo.scene      the scene
and, with --preview <png>, top-down previews: <png> colored like the terrain
paint, plus <png stem>_flow.png and <png stem>_deposition.png.

Layout. The map is (2E+1)^2 cells of 32x32 quads at 4 m (128 m cells);
E = 94 gives 24.2 km on a side. Heights are stored in 1/16 m, so they span
+-2048 m. Cells within FINE_EXTENT of the origin (the play area, 6 km
square by default) store every 4 m sample; the rest store every fourth
(16 m) and the engine upsamples them on read with integer bilinear
interpolation (TerrainHeightArchive::Upsample). The play area blends into
that same reconstruction over its outer ring, so coarse and fine cells share
identical edge samples.

Pipeline. A macro height field at 16 m covers the whole map: a lowland valley
around the start, ringed by ridged ranges that reach ~1.9 km. A particle
hydraulic erosion pass (droplets, after Beyer 2015) carves it and records how
much water passed each texel (flow) and how much sediment settled
(deposition). Lakes are carved after erosion. Inside the play area the eroded
field is resampled to 4 m and gets fine detail.

Flow and deposition files share one header, little endian:
  u64 magic ("TRNRFLW1" / "TRNRDEP1"), u32 version 1, u64 archive signature,
  i32 width, i32 height, f32 spacing (m), f32 origin x, f32 origin z (m, the
  world position of sample 0), f32 value scale,
followed by width*height u16 samples, rows along +z. A sample s decodes to
  flow:       expm1(s / 65535 * scale)  droplet-steps of water through the texel
  deposition: s / 65535 * scale         metres of net sediment laid down
Both are on the 16 m macro grid, which spans the same area as the archive.

Lakes have no water mesh of their own: basins are carved below WATER_LEVEL and
the scene's LandscapeDriver paints them as sand beds with gravel shores and
fills them with its water plane. The archive's two-bit code per sample is a
rock class (0..3) from slope, computed over whole grids so the paint matches
across cell borders. Forests and grass are placed at runtime by
LandscapeDriver from the same heights.

Re-running replaces the generated files, and deletes sculpt edits
(*.terrainedit) in the data directory: they store absolute heights for the
map they were made on. The explorer and sun objects are copied from
the current landscape_demo.scene so their component settings stay current.
Requires numpy and scipy.
"""

import argparse
import hashlib
import struct
import time
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
DATA = ASSETS / "TerrainData" / "Landscape"
SCENE = ASSETS / "Scenes" / "terrain" / "landscape_demo.scene"
# The explorer, sun and render settings are carried over from the current
# scene, so edits made to them in the editor survive a rebuild.
SOURCE_SCENE = SCENE

EXTENT = 94            # half extent in cells (24.2 km map)
FINE_EXTENT = 23       # half extent of the 4 m play area in cells (6 km)
CELLS = 32             # quads per cell edge
SPACING = 4.0          # metres per quad
COARSE = 4             # stride of stored samples outside the play area
MACRO = SPACING * COARSE  # 16 m macro grid
WATER_LEVEL = -40.0    # keep in sync with LandscapeDriver.waterLevel
# Paint altitudes written to LandscapeDriver (see its properties).
ROCK_LINE, SNOW_LINE, TREE_LINE, ALTITUDE_BLEND = 1150.0, 1480.0, 950.0, 6.0
SCALE = 16.0           # stored units per metre
MAX_HEIGHT = 2040.0
ARCHIVE_MAGIC = 0x31544748524E5254
FARGRID_MAGIC = 0x314452474E525254
FLOW_MAGIC = int.from_bytes(b"TRNRFLW1", "little")
DEPOSITION_MAGIC = int.from_bytes(b"TRNRDEP1", "little")
NAMESPACE = "urn:engine-components"


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lattice(ix, iz, seed):
    h = (ix.astype(np.uint64) * np.uint64(0x9E3779B1) ^
         iz.astype(np.uint64) * np.uint64(0x85EBCA77) ^
         np.uint64(seed * 0xC2B2AE3D & 0xFFFFFFFF)) & np.uint64(0xFFFFFFFF)
    h ^= h >> np.uint64(15)
    h = (h * np.uint64(0x2C1B3C6D)) & np.uint64(0xFFFFFFFF)
    h ^= h >> np.uint64(12)
    h = (h * np.uint64(0x297A2D39)) & np.uint64(0xFFFFFFFF)
    h ^= h >> np.uint64(15)
    return (h & np.uint64(0xFFFFFF)).astype(np.float64) / float(0xFFFFFF)


def value_noise(x, z, period, seed):
    x = x / period
    z = z / period
    fx, fz = np.floor(x), np.floor(z)
    ix, iz = fx.astype(np.int64) + 100000, fz.astype(np.int64) + 100000
    tx, tz = x - fx, z - fz
    tx = tx * tx * (3 - 2 * tx)
    tz = tz * tz * (3 - 2 * tz)
    a, b = lattice(ix, iz, seed), lattice(ix + 1, iz, seed)
    c, d = lattice(ix, iz + 1, seed), lattice(ix + 1, iz + 1, seed)
    return (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz


def gradient_noise(x, z, period, seed):
    """Perlin gradient noise in about -0.7..0.7: gradients from the lattice
    hash, quintic fade. Unlike value noise it has no axis-aligned ridges."""
    x = x / period
    z = z / period
    fx, fz = np.floor(x), np.floor(z)
    ix, iz = fx.astype(np.int64) + 100000, fz.astype(np.int64) + 100000
    tx, tz = x - fx, z - fz

    def corner(dx, dz):
        angle = lattice(ix + dx, iz + dz, seed) * (2.0 * np.pi)
        return np.cos(angle) * (tx - dx) + np.sin(angle) * (tz - dz)

    ux = tx * tx * tx * (tx * (tx * 6 - 15) + 10)
    uz = tz * tz * tz * (tz * (tz * 6 - 15) + 10)
    bottom = corner(0, 0) * (1 - ux) + corner(1, 0) * ux
    top = corner(0, 1) * (1 - ux) + corner(1, 1) * ux
    return bottom * (1 - uz) + top * uz


def octave_frame(x, z, octave, seed):
    """Rotate and offset each octave so lattice directions do not line up."""
    angle = 0.61 + octave * 1.13 + seed * 0.37
    c, s = np.cos(angle), np.sin(angle)
    shift = 1000.0 * ((seed * 7 + octave * 13) % 17)
    return c * x - s * z + shift, s * x + c * z - shift


def fbm(x, z, period, seed, octaves=4):
    """Fractal gradient noise in 0..1 (mean 0.5)."""
    total, amplitude, norm = 0.0, 1.0, 0.0
    for octave in range(octaves):
        ox, oz = octave_frame(x, z, octave, seed)
        total = total + amplitude * gradient_noise(ox, oz, period / (2 ** octave), seed + octave * 17)
        norm += amplitude
        amplitude *= 0.5
    return np.clip(0.5 + total / norm, 0.0, 1.0)


def ridged(x, z, period, seed, octaves=5):
    """Ridged multifractal in 0..1: sharp crests, each octave weighted by the
    one above so detail gathers on the ridges rather than in the valleys."""
    total, amplitude, norm, weight = 0.0, 1.0, 0.0, 1.0
    for octave in range(octaves):
        ox, oz = octave_frame(x, z, octave, seed)
        n = gradient_noise(ox, oz, period / (2 ** octave), seed + octave * 31)
        r = np.clip(1.0 - np.abs(n) * 1.5, 0.0, 1.0) ** 2
        total = total + amplitude * r * weight
        weight = np.clip(r * 1.8, 0.0, 1.0)
        norm += amplitude
        amplitude *= 0.5
    return total / norm


def bump(x, z, cx, cz, radius):
    d = np.sqrt((x - cx) ** 2 + (z - cz) ** 2) / radius
    return 1.0 - smoothstep(0.15, 1.0, d)


# Lakes: (center x, center z, radius). All lie in the play area; the first
# sits in front of the start, the last is an alpine lake in the foothills.
LAKES = [(180.0, 560.0, 430.0), (-1500.0, -700.0, 560.0),
         (1350.0, -1300.0, 520.0), (1500.0, 900.0, 380.0),
         (-700.0, 1650.0, 300.0)]
START = (0.0, 40.0)
# Benchmark/demo vista (world eye, look target): above a peak east of the
# start, looking northwest over the valley. Saved as the inactive "Vista"
# camera; the editor's Viewpoints menu jumps to it and captures use
# `--viewpoint Vista`. Absolute heights: re-check it after changing relief.
# Test-shot pose (scene camera "Vista"): on a 410 m foothill at the valley's
# south-east rim, looking north-west across the valley and a lake at the
# backlit ranges (the late-afternoon sun sits behind them). Picked
# 2026-11-18 from the 24 km map; 2026-11-16's (1272, 123, -12) is now low in
# the valley.
VISTA_EYE = (2304.0, 463.0, -3072.0)
VISTA_TARGET = (283.0, 254.0, -865.0)


def grid(extent_cells, step):
    """World x, z of a sample grid spanning cells -extent..extent at step m."""
    count = int(round((2 * extent_cells + 1) * CELLS * SPACING / step)) + 1
    origin = -extent_cells * CELLS * SPACING
    axis = origin + np.arange(count) * step
    x, z = np.meshgrid(axis, axis)  # row = z, col = x
    return x, z, origin


def macro_heights(x, z):
    """Uneroded relief: a lowland valley around the start inside ranges."""
    # Gentle domain warp so ranges bend and branch.
    wx = x + (fbm(x, z, 9000.0, 7, 2) - 0.5) * 2400.0
    wz = z + (fbm(x, z, 9000.0, 19, 2) - 0.5) * 2400.0
    distance = np.sqrt((x - START[0]) ** 2 + ((z - START[1]) * 0.85) ** 2)
    # 0 in the valley, 1 in the mountains; the rim wanders so passes open.
    rim = 2700.0 + (fbm(x, z, 2600.0, 41, 3) - 0.5) * 2400.0
    open_land = smoothstep(rim - 700.0, rim + 1800.0, distance)
    # Ranges where a broad mask is high, broad valleys between them.
    ranges = smoothstep(0.40, 0.66, fbm(wx, wz, 9000.0, 101, 3))
    uplift = fbm(wx, wz, 14000.0, 113, 2)
    crests = ridged(wx, wz, 4200.0, 211, 5)
    peaks = 150.0 + 2100.0 * ranges * (0.6 + 0.4 * uplift)
    mountains = open_land * peaks * crests ** 1.6
    foothills = (smoothstep(rim - 1600.0, rim, distance) * (0.35 + 0.65 * ranges) *
                 180.0 * fbm(x, z, 1100.0, 307, 4))
    rolling = (fbm(x, z, 1400.0, 3, 4) - 0.5) * 2.0 * 26.0
    return -22.0 + rolling + foothills + mountains


def erode(height, spacing, seed=9, droplets_per_texel=0.2, batch=150000, max_steps=100,
          inertia=0.1, capacity=4.0, min_capacity=0.01, erode_speed=0.3,
          deposit_speed=0.3, evaporate=0.02, gravity=4.0, radius=2, unit=60.0):
    """Particle hydraulic erosion. Droplets run downhill, eroding through a
    radial brush when under capacity and depositing bilinearly when over it.
    Batches of droplets move in lockstep; changes apply after every step.
    The rules see heights in units of unit * spacing metres, i.e. the drop
    per texel step is a slope / unit; unit 60 keeps the steepest faces near
    the 0.02 per step the classic parameters were tuned for. Stronger
    settings fill the ranges' hollows into flat-topped mesas.
    Returns (height, flow, deposition)."""
    rows, cols = height.shape
    size = rows * cols
    scale = unit * spacing
    h = (height / scale).ravel().copy()
    start = h.copy()
    flow = np.zeros(size)
    # Brush: offsets within radius texels, weights falling off linearly.
    oy, ox = np.mgrid[-radius:radius + 1, -radius:radius + 1]
    dist = np.sqrt(ox ** 2 + oy ** 2)
    keep = dist < radius
    brush_offset = (oy[keep] * cols + ox[keep]).astype(np.int64)
    brush_weight = (radius - dist[keep])
    brush_weight /= brush_weight.sum()
    margin = radius + 1
    rng = np.random.default_rng(seed)
    total = int(size * droplets_per_texel)

    def bilinear(px, py):
        ix, iy = px.astype(np.int64), py.astype(np.int64)
        fx, fy = px - ix, py - iy
        i = iy * cols + ix
        h00, h10, h01, h11 = h[i], h[i + 1], h[i + cols], h[i + cols + 1]
        value = (h00 * (1 - fx) * (1 - fy) + h10 * fx * (1 - fy) +
                 h01 * (1 - fx) * fy + h11 * fx * fy)
        gx = (h10 - h00) * (1 - fy) + (h11 - h01) * fy
        gy = (h01 - h00) * (1 - fx) + (h11 - h10) * fx
        return value, gx, gy, i, fx, fy

    corners = np.array([0, 1, cols, cols + 1], np.int64)
    for first in range(0, total, batch):
        n = min(batch, total - first)
        px = rng.uniform(margin, cols - 1 - margin, n)
        py = rng.uniform(margin, rows - 1 - margin, n)
        dx = np.zeros(n)
        dy = np.zeros(n)
        speed = np.ones(n)
        water = np.ones(n)
        sediment = np.zeros(n)
        for _ in range(max_steps):
            if px.size == 0:
                break
            value, gx, gy, i, fx, fy = bilinear(px, py)
            dx = dx * inertia - gx * (1 - inertia)
            dy = dy * inertia - gy * (1 - inertia)
            length = np.hypot(dx, dy)
            moving = length > 1e-12
            length[~moving] = 1.0
            dx /= length
            dy /= length
            nx, ny = px + dx, py + dy
            inside = moving & (nx >= margin) & (nx < cols - 1 - margin) &                 (ny >= margin) & (ny < rows - 1 - margin)
            new_value = bilinear(np.where(inside, nx, px), np.where(inside, ny, py))[0]
            delta_h = np.where(inside, new_value - value, 0.0)
            cap = np.maximum(-delta_h * speed * water * capacity, min_capacity)
            depositing = (sediment > cap) | (delta_h > 0)
            deposit = np.where(delta_h > 0, np.minimum(delta_h, sediment),
                               (sediment - cap) * deposit_speed)
            deposit = np.where(depositing & inside, deposit, 0.0)
            take = np.where(~depositing & inside,
                            np.minimum((cap - sediment) * erode_speed, -delta_h), 0.0)
            # A droplet leaving the map or stalling drops what it carries.
            deposit = np.where(inside, deposit, sediment)
            sediment = sediment - deposit + take
            # One bincount per step: deposits on the four corners, erosion
            # through the brush, and water into the flow map (offset by size).
            weights = np.stack([(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy], 1)
            taken = np.nonzero(take > 0)[0]
            index = np.concatenate([
                (i[:, None] + corners[None, :]).ravel(),
                (i[taken, None] + brush_offset[None, :]).ravel(),
                i + size])
            amount = np.concatenate([
                (deposit[:, None] * weights).ravel(),
                (-take[taken, None] * brush_weight[None, :]).ravel(),
                water])
            change = np.bincount(index, amount, minlength=2 * size)
            h += change[:size]
            flow += change[size:]
            speed = np.sqrt(np.maximum(speed * speed - delta_h * gravity, 0.0))
            water *= 1.0 - evaporate
            alive = np.nonzero(inside)[0]
            px, py, dx, dy = nx[alive], ny[alive], dx[alive], dy[alive]
            speed, water, sediment = speed[alive], water[alive], sediment[alive]
    result = h.reshape(rows, cols) * scale
    deposition = np.maximum(result - start.reshape(rows, cols) * scale, 0.0)
    return result, flow.reshape(rows, cols), deposition


def carve(height, x, z):
    """Lowland around the start, lake basins and a dry start pad."""
    rolling = (fbm(x, z, 900.0, 3) - 0.5) * 2.0 * 24.0
    hills = (fbm(x, z, 330.0, 53, 3) - 0.5) * 2.0 * 13.0
    detail = (fbm(x, z, 60.0, 307, 3) - 0.5) * 3.0
    lowland = bump(x, z, START[0], START[1], 900.0)
    valley = -27.0 + rolling * 0.4 + hills * 0.6 + detail
    height = height * (1.0 - lowland) + valley * lowland
    for index, (cx, cz, radius) in enumerate(LAKES):
        # Irregular shorelines: warp the coordinates before measuring distance.
        wx = x + (fbm(x, z, 420.0, 401 + index * 7, 3) - 0.5) * 2.0 * radius * 0.55
        wz = z + (fbm(x, z, 420.0, 433 + index * 7, 3) - 0.5) * 2.0 * radius * 0.55
        d = np.sqrt((wx - cx) ** 2 + ((wz - cz) * 1.25) ** 2) / radius
        basin = 1.0 - smoothstep(0.45, 1.0, d)
        floor = WATER_LEVEL - 3.0 - 13.0 * (1.0 - smoothstep(0.0, 0.7, d))
        height = height * (1.0 - basin) + np.minimum(height, floor) * basin
    pad = bump(x, z, START[0], START[1], 45.0)
    return height * (1.0 - pad) + np.maximum(height, WATER_LEVEL + 7.0) * pad


def fine_detail(base, x, z):
    """4 m detail on the resampled macro field: small bumps everywhere, sharp
    spurs where the macro slope is steep (where rock shows)."""
    gz, gx = np.gradient(base, SPACING)
    steep = smoothstep(0.25, 0.9, np.sqrt(gx * gx + gz * gz))
    spurs = (1.0 - np.abs(2.0 * fbm(x, z, 170.0, 223, 3) - 1.0)) ** 1.5 - 0.35
    bumps = (fbm(x, z, 60.0, 311, 3) - 0.5) * 3.0
    return base + bumps + steep * spurs * 18.0


def upsample_quantized(coarse, stride):
    """Integer bilinear upsampling, identical to TerrainHeightArchive::Upsample
    (rounded half up) over a whole grid aligned to cell corners."""
    rows, cols = coarse.shape
    q = coarse.astype(np.int64)
    out_rows, out_cols = (rows - 1) * stride + 1, (cols - 1) * stride + 1
    r = np.arange(out_rows)
    c = np.arange(out_cols)
    r0 = np.minimum(r // stride, rows - 2)
    c0 = np.minimum(c // stride, cols - 2)
    fy = (r - r0 * stride)[:, None]
    fx = (c - c0 * stride)[None, :]
    a = q[r0][:, c0]
    b = q[r0][:, c0 + 1]
    cc = q[r0 + 1][:, c0]
    d = q[r0 + 1][:, c0 + 1]
    total = (a * (stride - fx) * (stride - fy) + b * fx * (stride - fy) +
             cc * (stride - fx) * fy + d * fx * fy)
    area = stride * stride
    return (2 * total + area) // (2 * area)


def quantize(height):
    return np.clip(np.rint(height * SCALE), -32768, 32767).astype(np.int64)


def rock_class(height, spacing):
    gz, gx = np.gradient(height, spacing)
    slope = 1.0 - 1.0 / np.sqrt(1.0 + gx * gx + gz * gz)
    cls = np.zeros(height.shape, np.uint8)
    cls[slope > 0.13] = 1
    cls[slope > 0.19] = 2
    cls[slope > 0.26] = 3
    cls[height < WATER_LEVEL + 1.0] = 0
    return cls


def build():
    t0 = time.time()
    mx, mz, origin = grid(EXTENT, MACRO)
    print(f"macro grid {mx.shape[1]}^2 at {MACRO:g} m")
    raw = macro_heights(mx, mz)
    print(f"  relief {raw.min():.0f}..{raw.max():.0f} m ({time.time() - t0:.0f} s)")
    eroded, flow, deposition = erode(raw, MACRO)
    print(f"  eroded: mean change {np.abs(eroded - raw).mean():.2f} m, "
          f"max cut {(raw - eroded).max():.0f} m, max fill {deposition.max():.0f} m "
          f"({time.time() - t0:.0f} s)")
    macro = np.clip(carve(eroded, mx, mz), -200.0, MAX_HEIGHT)
    macro_q = quantize(macro)

    # Play area at 4 m: bicubic resample of the eroded macro field (before
    # the lakes, which are carved again at full resolution) plus detail.
    span = (2 * FINE_EXTENT + 1) * CELLS
    first = (EXTENT - FINE_EXTENT) * CELLS // COARSE
    last = first + span // COARSE
    window = eroded[first:last + 1, first:last + 1]
    fx, fz, fine_origin = grid(FINE_EXTENT, SPACING)
    coords = np.arange(span + 1) / COARSE
    rows, cols = np.meshgrid(coords, coords, indexing="ij")
    base = ndimage.map_coordinates(window, [rows, cols], order=3, mode="nearest")
    fine = np.clip(carve(fine_detail(base, fx, fz), fx, fz), -200.0, MAX_HEIGHT)
    # Blend to the coarse reconstruction over the outer two cells so the
    # play area's border samples equal its coarse neighbors' samples.
    recon = upsample_quantized(macro_q[first:last + 1, first:last + 1], COARSE)
    edge = np.minimum(np.arange(span + 1), span - np.arange(span + 1))
    ramp = smoothstep(0.0, 2.0 * CELLS, edge.astype(np.float64))
    mask = np.minimum(ramp[:, None], ramp[None, :])
    fine_q = recon + np.rint((fine * SCALE - recon) * mask).astype(np.int64)
    fine_q = np.clip(fine_q, -32768, 32767)
    print(f"fine grid {fine_q.shape[1]}^2 at {SPACING:g} m ({time.time() - t0:.0f} s)")
    return {"macro": macro, "macro_q": macro_q, "fine_q": fine_q, "origin": origin,
            "fine_origin": fine_origin, "flow": flow, "deposition": deposition}


def pack_codes(codes):
    flat = codes.reshape(-1)
    padded = np.zeros((flat.size + 3) // 4 * 4, np.uint8)
    padded[:flat.size] = flat
    quads = padded.reshape(-1, 4)
    return (quads[:, 0] | (quads[:, 1] << 2) | (quads[:, 2] << 4) |
            (quads[:, 3] << 6)).astype(np.uint8).tobytes()


def archive_body(world):
    """Cell blocks in file order, the corner grid, and the content signature."""
    macro_q, fine_q = world["macro_q"], world["fine_q"]
    macro_codes = rock_class(macro_q / SCALE, MACRO)
    fine_codes = rock_class(fine_q / SCALE, SPACING)
    side = 2 * EXTENT + 1
    stride = CELLS // COARSE
    blocks = []
    corners = macro_q[::stride, ::stride].copy()
    for cell_z in range(-EXTENT, EXTENT + 1):
        for cell_x in range(-EXTENT, EXTENT + 1):
            if max(abs(cell_x), abs(cell_z)) <= FINE_EXTENT:
                r0 = (cell_z + FINE_EXTENT) * CELLS
                c0 = (cell_x + FINE_EXTENT) * CELLS
                block = fine_q[r0:r0 + CELLS + 1, c0:c0 + CELLS + 1]
                codes = fine_codes[r0:r0 + CELLS + 1, c0:c0 + CELLS + 1]
                corners[cell_z + EXTENT, cell_x + EXTENT] = block[0, 0]
                corners[cell_z + EXTENT + 1, cell_x + EXTENT + 1] = block[-1, -1]
                corners[cell_z + EXTENT, cell_x + EXTENT + 1] = block[0, -1]
                corners[cell_z + EXTENT + 1, cell_x + EXTENT] = block[-1, 0]
            else:
                r0 = (cell_z + EXTENT) * stride
                c0 = (cell_x + EXTENT) * stride
                block = macro_q[r0:r0 + stride + 1, c0:c0 + stride + 1]
                codes = macro_codes[r0:r0 + stride + 1, c0:c0 + stride + 1]
            blocks.append(block.astype("<i2").tobytes() + pack_codes(codes))
    assert corners.shape == (side + 1, side + 1)
    body = b"".join(blocks)
    signature = int.from_bytes(hashlib.blake2b(body, digest_size=8).digest(), "little") or 1
    return body, corners, signature


def write_archive(world):
    DATA.mkdir(parents=True, exist_ok=True)
    body, corners, signature = archive_body(world)
    with open(DATA / "world.heights", "wb") as file:
        file.write(struct.pack("<QIQiiifii", ARCHIVE_MAGIC, 3, signature, EXTENT,
                               CELLS, CELLS, SCALE, FINE_EXTENT, COARSE))
        file.write(body)
    side = 2 * EXTENT + 2
    with open(DATA / "world.fargrid", "wb") as file:
        file.write(struct.pack("<QIQiiiif", FARGRID_MAGIC, 3, signature, EXTENT,
                               side, CELLS, CELLS, SCALE))
        file.write(np.ascontiguousarray(corners.astype("<i2")).tobytes())
        file.write(struct.pack("<I", 0))
    return signature


def write_map(path, magic, signature, values, scale, origin):
    encoded = np.clip(np.rint(values / scale * 65535.0), 0, 65535).astype("<u2")
    with open(path, "wb") as file:
        file.write(struct.pack("<QIQiiffff", magic, 1, signature, values.shape[1],
                               values.shape[0], MACRO, origin, origin, scale))
        file.write(encoded.tobytes())


def write_hydrology(world, signature):
    flow = np.log1p(world["flow"])
    flow_scale = float(max(flow.max(), 1e-6))
    write_map(DATA / "world.flow", FLOW_MAGIC, signature, flow, flow_scale, world["origin"])
    deposition = world["deposition"]
    deposition_scale = float(max(deposition.max(), 1e-6))
    write_map(DATA / "world.deposition", DEPOSITION_MAGIC, signature, deposition,
              deposition_scale, world["origin"])


def remove_stale_edits():
    removed = sorted(DATA.glob("*.terrainedit"))
    for path in removed:
        path.unlink()
    if removed:
        print(f"removed {len(removed)} sculpt edits made on the previous map")


def numbers(values):
    return " ".join(f"{v:.6g}" for v in values)


def find_item(objects, name):
    for item in objects.findall("item"):
        if item.get("name") == name:
            return item
    raise SystemExit(f"{SOURCE_SCENE.name} has no object named {name!r}")


def set_vector(parent, tag, values):
    element = parent.find(tag)
    for child in list(element):
        element.remove(child)
    element.set("_arrayEncoding", "spaceSeparatedNumbers")
    element.text = numbers(values)


def fine_sample(world, x, z):
    col = int(round((x - world["fine_origin"]) / SPACING))
    row = int(round((z - world["fine_origin"]) / SPACING))
    return float(world["fine_q"][row, col]) / SCALE


def macro_sample(world, x, z):
    """Height (m) at world x, z from the 16 m macro grid (whole map)."""
    col = int(round((x - world["origin"]) / MACRO))
    row = int(round((z - world["origin"]) / MACRO))
    q = world["macro_q"]
    return float(q[min(max(row, 0), q.shape[0] - 1), min(max(col, 0), q.shape[1] - 1)]) / SCALE


# Low clouds clinging to summits on the far side of the valley, in the vista's
# view (Tools/generate_sky_sprites.py LowClouds, a 800 m box of 70 m puffs).
CLOUD_PEAKS = [(-3072.0, 2944.0), (-4096.0, 2304.0), (-2944.0, 3968.0), (1792.0, 3456.0)]


def atmosphere_items(height_at):
    """The "Atmosphere" object: mist over every lake and at the start, mist
    cards on the first lake, low clouds on the summits. Prefabs from
    Tools/generate_sky_sprites.py; height_at(x, z) gives ground metres."""
    prefabs = "Assets/Prefabs/Woodland/Atmosphere/"

    def item(name, prefab, position, scale=(1, 1, 1)):
        element = ET.Element("item", {"prefab": prefabs + prefab, "enabled": "true"})
        transform = ET.SubElement(element, "transform")
        for tag, values in (("Transform.position", position), ("Transform.rotation", (0, 0, 0)),
                            ("Transform.scale", scale)):
            ET.SubElement(transform, tag, {"_arrayEncoding": "spaceSeparatedNumbers"}).text = numbers(values)
        return element

    group = ET.Element("item", {"name": "Atmosphere", "enabled": "true"})
    transform = ET.SubElement(group, "transform")
    for tag, values in (("Transform.position", (0, 0, 0)), ("Transform.rotation", (0, 0, 0)),
                        ("Transform.scale", (1, 1, 1))):
        ET.SubElement(transform, tag, {"_arrayEncoding": "spaceSeparatedNumbers"}).text = numbers(values)
    ET.SubElement(group, "components")
    children = ET.SubElement(group, "children")
    # Lake mist hugs the water (the lakes all sit at WATER_LEVEL); its 500 m
    # box is scaled in x/z to each lake.
    for x, z, radius in LAKES:
        spread = radius / 250.0
        children.append(item("Lake mist", "LakeMist.prefab", (x, WATER_LEVEL + 4.0, z), (spread, 1, spread)))
    # Close-range mist and mist cards on the first lake, in front of the start.
    lake_x, lake_z, lake_r = LAKES[0]
    toward_start = np.array([START[0] - lake_x, START[1] - lake_z])
    toward_start /= np.linalg.norm(toward_start)
    near = np.array([lake_x, lake_z]) + toward_start * lake_r * 0.7
    children.append(item("Valley mist", "ValleyMist.prefab", (near[0], WATER_LEVEL + 2.5, near[1])))
    side = np.array([-toward_start[1], toward_start[0]])
    for k in (-1, 0, 1):
        p = np.array([lake_x, lake_z]) + toward_start * lake_r * (0.45 + 0.1 * abs(k)) + side * k * 70.0
        children.append(item("Mist card", "MistCard.prefab", (p[0], WATER_LEVEL, p[1])))
    for x, z in CLOUD_PEAKS:
        children.append(item("Low clouds", "LowClouds.prefab", (x, height_at(x, z) - 90.0, z)))
    return group


def write_scene(world):
    ET.register_namespace("components", NAMESPACE)
    source = ET.parse(SOURCE_SCENE).getroot()
    # Comments are dropped by the parser; the header is rewritten below.
    source_objects = source.find("objects")
    c = "{%s}" % NAMESPACE
    map_size = (2 * EXTENT + 1) * CELLS * SPACING

    scene = ET.Element("Scene", {"version": "1"})
    settings = source.find("settings")
    settings.set("showGrid", "false")
    # The source scene's distance texture LOD adds up to +4 mips of bias past
    # 48 m, which blurs every tree a little way off. Normal mip selection is
    # enough here.
    settings.set("textureLodEnabled", "false")
    scene.append(settings)
    objects = ET.SubElement(scene, "objects")

    ground = fine_sample(world, *START)
    explorer = find_item(source_objects, "Landscape Explorer")
    explorer.set("name", "Landscape Explorer")
    # Start in the air above the meadow pad, looking over the first lake.
    set_vector(explorer.find("transform"), "Transform.position",
               (START[0], ground + 25.0, START[1]))
    camera = explorer.find("components").find(c + "Camera")
    # Far enough to see the map edge from the play area.
    camera.set("Camera.far", f"{map_size:.0f}")
    # Look across the first lake toward the ranges.
    set_vector(camera, "Camera.target", (LAKES[0][0], ground - 20.0, LAKES[0][1]))
    # Free flight: WASD, Space up, Shift down, Ctrl boost.
    controller = explorer.find("components").find(c + "FirstPersonController")
    controller.set("FirstPersonController.moveSpeed", "25")
    controller.set("FirstPersonController.sprintMultiplier", "10")
    controller.set("FirstPersonController.flightMode", "true")
    controller.set("FirstPersonController.flightResponse", "3")
    controller.set("FirstPersonController.flightGroundClearance", "1.5")
    # Skim the lakes rather than dive to their beds.
    controller.set("FirstPersonController.flightMinimumHeight", str(WATER_LEVEL + 1.2))
    body = explorer.find("components").find(c + "RigidBody")
    body.set("RigidBody.useGravity", "false")
    objects.append(explorer)

    # Saved viewpoint: an inactive camera holds the pose without driving the
    # Game view.
    vista = ET.SubElement(objects, "item", {"name": "Vista", "enabled": "true"})
    vista_components = ET.SubElement(vista, "components")
    vista_camera = ET.SubElement(vista_components, c + "Camera", {
        "Camera.active": "false", "Camera.useTransformRotation": "false",
        "Camera.fov": camera.get("Camera.fov", "67"), "Camera.near": camera.get("Camera.near", "0.06"),
        "Camera.far": f"{map_size:.0f}", "Camera.orthographic": "false",
        "Camera.orthographicSize": "5"})
    ET.SubElement(vista_camera, "Camera.up", {"_arrayEncoding": "spaceSeparatedNumbers"}).text = "0 1 0"
    ET.SubElement(vista_camera, "Camera.target",
                  {"_arrayEncoding": "spaceSeparatedNumbers"}).text = numbers(VISTA_TARGET)
    ET.SubElement(vista, "children")
    vista_transform = ET.SubElement(vista, "transform")
    for tag, values in (("Transform.position", VISTA_EYE), ("Transform.rotation", (0, 0, 0)),
                        ("Transform.scale", (1, 1, 1))):
        ET.SubElement(vista_transform, tag,
                      {"_arrayEncoding": "spaceSeparatedNumbers"}).text = numbers(values)

    sun = find_item(source_objects, "Late afternoon sun")
    # The low sun grazes the meadows: the source scene's tiny normal bias
    # left them banded with shadow acne from about 20 m out.
    for light in sun.iter(c + "Light"):
        light.set("Light.shadowNormalBias", "0.06")
    objects.append(sun)

    objects.append(atmosphere_items(lambda x, z: macro_sample(world, x, z)))

    terrain = ET.SubElement(objects, "item", {"name": "Landscape", "enabled": "true"})
    transform = ET.SubElement(terrain, "transform")
    for tag, values in (("Transform.position", (0, 0, 0)), ("Transform.rotation", (0, 0, 0)),
                        ("Transform.scale", (1, 1, 1))):
        ET.SubElement(transform, tag, {"_arrayEncoding": "spaceSeparatedNumbers"}).text = numbers(values)
    components = ET.SubElement(terrain, "components")
    ET.SubElement(components, c + "Mesh", {"Mesh.file": "", "Mesh.useLightProbes": "false"})
    ET.SubElement(components, c + "Material", {
        "Material.useVertexColors": "true", "Material.specularModel": "MetallicRoughness",
        "Material.alphaMode": "Opaque", "Material.doubleSided": "false"})
    manager = ET.SubElement(components, c + "TerrainManager", {
        "TerrainManager." + key: value for key, value in {
            "cellsX": str(CELLS), "cellsY": str(CELLS),
            "cellWidth": numbers([SPACING]), "cellDepth": numbers([SPACING]),
            # Five cells (about 700 m) of real terrain and trees around the
            # viewer; mesh detail halves beyond two cells and again beyond three.
            # The far grid covers the rest of the map whatever farRadius is
            # (it only sizes the fallback ring, and is capped at 32).
            "loadedRadius": "5", "editorRadius": "3", "farRadius": str(min(EXTENT, 32)),
            "lod2Radius": "2", "lod4Radius": "3", "farTileScreenErrorPixels": "6",
            # Trees beyond the loaded cells, as impostor blocks, up to 8 km
            # (the engine's cap is 64 cells).
            "farObjectRadius": str(min(EXTENT, 64)),
            # Beyond 2.2 km, squares of 40 m holding five or more trees of
            # one kind draw as one baked forest-cluster impostor
            # (Tools/generate_forest_clusters.py) instead of one per tree.
            "farClusterDistance": "2200", "farClusterSize": "40",
            "farClusterMinimumTrees": "5",
            "maxLodChangesPerUpdate": "4", "maxLoadsPerUpdate": "6",
            "maxUnloadsPerUpdate": "4", "asyncStreaming": "true",
            "maxPreparedCells": "4", "maxUploadsPerUpdate": "4",
            "batchFarCells": "true", "fastRuntimeLighting": "true",
            "stochasticTerrainVariation": "true", "terrainMacroColorEnabled": "true",
            "reliefMapping": "true", "reliefShadows": "false",
            "measurementGrid": "false", "generateColliders": "true",
            "streamCollidersByDistance": "true", "cpuBudgetMiB": "512",
            "gpuBudgetMiB": "256", "cacheBudgetMiB": "1024",
            "viewerObjectName": "Landscape Explorer",
            "cellDataDirectory": "Engine/Core/Assets/TerrainData/Landscape",
            "runtimeBakeDirectory": "", "requireRuntimeBake": "false"}.items()})
    ET.SubElement(manager, "TerrainManager.cells")
    far_clusters = ET.SubElement(manager, "TerrainManager.farClusters")
    for kind in ("Oak_01", "Oak_02", "Conifer_01", "Conifer_02"):
        ET.SubElement(far_clusters, "item", {
            "asset": f"Assets/Prefabs/Woodland/Clusters/ForestCluster_{kind}.prefab",
            "match": "/Oaks/" if kind.startswith("Oak") else "/Conifers/", "trees": "14"})
    names = ET.SubElement(manager, "TerrainManager.objectLayerNames")
    for name in ("Landscape forest", "Procedural meadow grass", "Landscape props"):
        ET.SubElement(names, "item").text = name
    runs = ET.SubElement(manager, "TerrainManager.proceduralRuns")
    for y in range(-EXTENT, EXTENT + 1):
        ET.SubElement(runs, "item", {"y": str(y), "start": str(-EXTENT), "end": str(EXTENT)})
    ET.SubElement(manager, "TerrainManager.editedCells")
    ET.SubElement(components, c + "LandscapeDriver", {
        "LandscapeDriver.viewerObjectName": "Landscape Explorer",
        "LandscapeDriver.mapHalfExtentCells": str(EXTENT),
        "LandscapeDriver.heightArchive": "Assets/TerrainData/Landscape/world.heights",
        "LandscapeDriver.farGrid": "Assets/TerrainData/Landscape/world.fargrid",
        "LandscapeDriver.cacheDirectory": ".local/cache/landscape_demo",
        "LandscapeDriver.enableFarTerrain": "true",
        "LandscapeDriver.waterLevel": numbers([WATER_LEVEL]),
        "LandscapeDriver.rockLine": numbers([ROCK_LINE]),
        "LandscapeDriver.snowLine": numbers([SNOW_LINE]),
        "LandscapeDriver.treeLine": numbers([TREE_LINE]),
        "LandscapeDriver.altitudeBlend": numbers([ALTITUDE_BLEND]),
        "LandscapeDriver.treesPerCell": "220",
        "LandscapeDriver.forestLayerName": "Landscape forest",
        "LandscapeDriver.grassRadius": "26"})
    ET.SubElement(terrain, "children")

    ET.indent(scene, space="  ")
    header = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--\n"
              f"  Landscape demo: a {map_size / 1000:.1f} km streamed map of eroded ranges around a\n"
              "  lowland valley with oak forests and five lake basins.\n"
              "  Generated by tools/build_landscape_demo.py; re-running replaces this file.\n-->\n")
    SCENE.write_text(header + ET.tostring(scene, encoding="unicode") + "\n", encoding="utf-8")


def png(path, rgb):
    import zlib
    data = (np.clip(rgb[::-1], 0, 1) * 255).astype(np.uint8)  # +z up
    raw = b"".join(b"\x00" + row.tobytes() for row in data)
    chunk = lambda tag, body: (struct.pack(">I", len(body)) + tag + body +
                               struct.pack(">I", zlib.crc32(tag + body)))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(
        ">IIBBBBB", data.shape[1], data.shape[0], 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def write_previews(path, world):
    """Whole map at 16 m with the play area downsampled into it."""
    height = world["macro_q"] / SCALE
    first = (EXTENT - FINE_EXTENT) * CELLS // COARSE
    fine = world["fine_q"][::COARSE, ::COARSE] / SCALE
    height = height.copy()
    height[first:first + fine.shape[0], first:first + fine.shape[1]] = fine
    codes = rock_class(height, MACRO)
    blend = ALTITUDE_BLEND
    land = smoothstep(WATER_LEVEL + 1.5, WATER_LEVEL + 4.0, height)
    shore = smoothstep(WATER_LEVEL - 3.5, WATER_LEVEL - 1.0, height)
    rock = land * np.maximum(codes / 3.0, smoothstep(ROCK_LINE - 8 * blend, ROCK_LINE + 8 * blend, height))
    snow = land * smoothstep(SNOW_LINE - 6 * blend, SNOW_LINE + 6 * blend, height)
    forest = smoothstep(WATER_LEVEL + 4, WATER_LEVEL + 9, height) * \
        (1 - smoothstep(TREE_LINE - 8 * blend, TREE_LINE, height))
    colors = {"sand": (0.76, 0.66, 0.46), "gravel": (0.5, 0.5, 0.47), "grass": (0.3, 0.5, 0.2),
              "forest": (0.14, 0.27, 0.1), "rock": (0.42, 0.4, 0.38), "snow": (0.95, 0.96, 0.98)}
    grassy = land * (1 - rock) * (1 - snow)
    image = np.zeros(height.shape + (3,))
    for name, weight in (("sand", 1 - shore), ("gravel", shore * (1 - land)),
                         ("grass", grassy * (1 - 0.6 * forest)), ("forest", grassy * 0.6 * forest),
                         ("rock", rock * (1 - snow)), ("snow", snow)):
        image += weight[..., None] * np.array(colors[name])
    gz, gx = np.gradient(height, MACRO)
    shade = np.clip(0.8 + (gx - gz) * 0.5, 0.3, 1.3)
    png(path, image * shade[..., None])
    stem = Path(path)
    flow = np.log1p(world["flow"])
    flow = flow / max(flow.max(), 1e-6)
    png(stem.with_name(stem.stem + "_flow.png"),
        np.stack([flow * 0.3, flow * 0.6, 0.15 + flow * 0.85], -1) * shade[..., None] * 0.9)
    deposition = np.sqrt(world["deposition"] / max(world["deposition"].max(), 1e-6))
    png(stem.with_name(stem.stem + "_deposition.png"),
        np.stack([0.2 + deposition * 0.8, 0.18 + deposition * 0.6, 0.15 + deposition * 0.2], -1) *
        shade[..., None])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", help="also write top-down PNG previews")
    parser.add_argument("--no-write", action="store_true",
                        help="build and preview only; leave the map files alone")
    arguments = parser.parse_args()
    world = build()
    if not arguments.no_write:
        signature = write_archive(world)
        write_hydrology(world, signature)
        remove_stale_edits()
        write_scene(world)
    if arguments.preview:
        write_previews(arguments.preview, world)
    macro = world["macro_q"] / SCALE
    water = float((macro < WATER_LEVEL).mean() * 100)
    archive = DATA / "world.heights"
    size = f", archive {archive.stat().st_size / 2 ** 20:.1f} MiB" if not arguments.no_write else ""
    print(f"map {(2 * EXTENT + 1) * CELLS * SPACING / 1000:.1f} km, heights "
          f"{macro.min():.0f}..{macro.max():.0f} m, lake area {water:.2f}%, "
          f"start ground {fine_sample(world, *START):.1f} m{size}")


if __name__ == "__main__":
    main()
