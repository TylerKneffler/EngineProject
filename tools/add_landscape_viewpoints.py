"""Adds eye-level foliage viewpoints to the landscape demo scene.

The demo's start pose flies 25 m above the meadow, where grass and bushes
(drawn within ~26 m of the viewer) are out of view. This adds saved viewpoint
cameras near the start for checking foliage (.md/todo.md, Foliage section), next to the
existing "Vista":

  ForestEdge   standing in the meadow, looking at an oak forest edge
  ForestFloor  inside a forest, looking across the forest floor
  Backlit      in the meadow with forest between the camera and the sun
  Trail        on a forest trail
  Cliffs       below the steepest slope within reach
  MeadowSlope  a sunlit meadow slope with a forest edge and a valley beyond
  Escarpment   in front of the escarpment (build_landscape_demo.py
               ESCARPMENT), hovering where its face is in sight over the forest
  ForestWall   in open meadow, a forest edge 60-90 m ahead, sun behind
  FieldWalk    the start of a straight 240 m line over open meadow, for
               judging grass while moving (--drive-forward 380 covers ~175 m
               of it; the gaze follows the ground's mean slope, so the eye
               stays 1-3 m above the grass the whole way)

and two walking routes along the trails, the way a player would go
(SplinePath objects at eye height; GameBackendCapture --route <name>):

  ForestTrailWalk  400 m on from the Trail viewpoint: forest, the forest
                   edge, meadow, and back into forest
  MeadowTrailWalk  400 m of the open-meadow trail nearest the start, walked
                   with the sun behind

The game starts on the flying Landscape Explorer. (A walk camera that
followed MeadowTrailWalk, insert_walk_camera, is no longer added: the user
wants the explorer's flight control.)

Where the forest is comes from the saved forest (the "Landscape forest"
layer of world.objects: crowns from Prefabs/Woodland/tree_crowns.json), and
trails are the scene's trail objects (the Trails group: SplinePath control
points), so viewpoints follow the map as edited. Ground heights come from the
landscape height archive. Editor: Viewpoints menu. Captures:
GameBackendCapture --viewpoint ForestEdge.

Re-running replaces the viewpoints. build_landscape_demo.py calls this after
writing the scene.

Usage: python tools/add_landscape_viewpoints.py
"""

import json
import math
import struct
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

import numpy as np

import build_grass_test_scene as grass
import scene_static
from build_landscape_demo import ESCARPMENT

SCENE = grass.ASSETS / "Scenes" / "terrain" / "landscape_demo.scene"
NAMESPACE = grass.NAMESPACE
START = (0.0, 40.0)
CELL = 128.0
SPACING = 4.0
EYE = 1.7
WATER, TREE_LINE, BLEND = -40.0, 950.0, 6.0
# Direction toward the sun (the scene's "Late afternoon sun" object, rotation
# 1.0, 2.4 rad since the 2026-11-20 look pass: ~57 degrees up; the light travels
# along the rotated +z axis), flattened onto the ground.
SUN_PITCH, SUN_YAW = 1.0, 2.4
SUN_FLAT = (-math.sin(SUN_YAW) * math.cos(SUN_PITCH), -math.cos(SUN_YAW) * math.cos(SUN_PITCH))
NAMES = ("ForestEdge", "ForestFloor", "Backlit", "Trail", "Cliffs", "MeadowSlope", "Escarpment", "ForestWall", "FieldWalk")


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def forest_band(height):
    """Heights forests grow at: off shores and below the tree line."""
    return smoothstep(WATER + 4, WATER + 9, height) * (1 - smoothstep(TREE_LINE - 8 * BLEND, TREE_LINE, height))


# --- the saved forest (world.objects) ----------------------------------------

OBJECTS = grass.ASSETS / "TerrainData" / "Landscape" / "world.objects"
CROWNS = grass.ASSETS / "Prefabs" / "Woodland" / "tree_crowns.json"
FOREST_LAYER = "Landscape forest"
COVER_STEP = 2.0      # crown cover grid, metres
COVER_REACH = 8.0     # cover is the crowned share within this radius
# Open meadow: under a quarter crowned (the saved forest has lone meadow
# trees, which the old noise mask never saw).
OPEN_COVER = 0.25
_archive = None
_trees = {}
_cover = {}


def _archive_index():
    """(header, layer names, asset paths, cell entries) of world.objects
    (TerrainObjectArchive.h, TRNROBJA v2)."""
    global _archive
    if _archive is None:
        data = OBJECTS.read_bytes()
        magic, version, extent, cells_x, cells_y, width, depth, layers = struct.unpack_from("<QIiiiffI", data, 0)
        if version != 2:
            raise SystemExit(f"unexpected object archive version {version}")
        at = struct.calcsize("<QIiiiffI")
        names = []
        for _ in range(layers):
            (n,) = struct.unpack_from("<I", data, at); at += 4
            names.append(data[at:at + n].decode()); at += n
            (n,) = struct.unpack_from("<I", data, at); at += 4 + n
        (count,) = struct.unpack_from("<I", data, at); at += 4
        assets = []
        for _ in range(count):
            (n,) = struct.unpack_from("<I", data, at); at += 4
            assets.append(data[at:at + n].decode()); at += n
        side = 2 * extent + 1
        entries = [struct.unpack_from("<QII", data, at + i * 16) for i in range(side * side)]
        crowns = json.loads(CROWNS.read_text(encoding="utf-8"))["crowns"] if CROWNS.exists() else {}
        radius = []
        for asset in assets:
            key = next((k for k in crowns if asset.endswith(k + ".prefab")), None)
            radius.append(crowns[key]["radius"] if key else 3.0)
        _archive = (data, extent, cells_x * width, cells_y * depth, names, np.array(radius), entries)
    return _archive


def forest_trees(cx, cz):
    """The forest layer's trees in a cell: world x, z and crown radius."""
    if (cx, cz) not in _trees:
        data, extent, span_x, span_z, names, radius, entries = _archive_index()
        trees = np.zeros((0, 3))
        if abs(cx) <= extent and abs(cz) <= extent:
            offset, stored, raw = entries[(cz + extent) * (2 * extent + 1) + cx + extent]
            if stored:
                block = zlib.decompress(data[offset:offset + stored])
                (count,) = struct.unpack_from("<H", block, 0)
                at = 2
                for _ in range(count):
                    index, n = struct.unpack_from("<HI", block, at)
                    at += 6
                    if names[index] == FOREST_LAYER:
                        asset = np.frombuffer(block, "<u2", n, at)
                        x = np.cumsum(np.frombuffer(block, "<u2", n, at + 2 * n).astype(np.int64)) % 65536
                        z = np.frombuffer(block, "<u2", n, at + 4 * n)
                        scale = np.frombuffer(block, "u1", n, at + 11 * n) / 40.0
                        trees = np.stack([cx * span_x + x / 65535.0 * span_x, cz * span_z + z / 65535.0 * span_z,
                                          radius[asset] * scale], axis=1)
                    at += n * 12   # columns: asset, x, z, y (u16 each), nx, nz, yaw, scale (u8)
        _trees[(cx, cz)] = trees
    return _trees[(cx, cz)]


def _cell_cover(cx, cz):
    """The crowned share within COVER_REACH of each 2 m sample of a cell."""
    if (cx, cz) not in _cover:
        margin = int(COVER_REACH / COVER_STEP)
        n = int(CELL / COVER_STEP) + 1 + 2 * margin
        x0, z0 = cx * CELL - margin * COVER_STEP, cz * CELL - margin * COVER_STEP
        crowned = np.zeros((n, n), bool)
        xs = x0 + np.arange(n) * COVER_STEP
        for dz in (-1, 0, 1):
            for dx in (-1, 0, 1):
                for tx, tz, r in forest_trees(cx + dx, cz + dz):
                    i0, i1 = np.searchsorted(xs, [tx - r, tx + r])
                    j0, j1 = np.searchsorted(xs - x0 + z0, [tz - r, tz + r])
                    if i0 >= i1 or j0 >= j1:
                        continue
                    gx, gz = np.meshgrid(xs[i0:i1], z0 + np.arange(j0, j1) * COVER_STEP)
                    crowned[j0:j1, i0:i1] |= (gx - tx) ** 2 + (gz - tz) ** 2 <= r * r
        # Mean over a square of COVER_REACH around each sample (summed area).
        k = margin
        total = np.pad(crowned.astype(np.float64), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
        size = 2 * k + 1
        box = (total[size:, size:] - total[:-size, size:] - total[size:, :-size] + total[:-size, :-size]) / size ** 2
        _cover[(cx, cz)] = box
    return _cover[(cx, cz)]


def forest_mask(x, z, height=None):
    """Forest cover 0..1 at a point: the crowned share around it."""
    cx, cz = math.floor(x / CELL), math.floor(z / CELL)
    cover = _cell_cover(cx, cz)
    fx, fz = (x - cx * CELL) / COVER_STEP, (z - cz * CELL) / COVER_STEP
    ix, iz = min(int(fx), cover.shape[1] - 2), min(int(fz), cover.shape[0] - 2)
    tx, tz = fx - ix, fz - iz
    return float((cover[iz, ix] * (1 - tx) + cover[iz, ix + 1] * tx) * (1 - tz) +
                 (cover[iz + 1, ix] * (1 - tx) + cover[iz + 1, ix + 1] * tx) * tz)


# --- the scene's trails (Trails group) ---------------------------------------

TRAIL_STEP = 1.0      # metres between samples of a trail
_trails = None


def _catmull_rom(points, closed):
    """The spline through the control points (uniform Catmull-Rom, as
    SplinePath), sampled every TRAIL_STEP metres."""
    p = np.array(points, float)
    if closed:
        ext = np.vstack([p[-1:], p, p[:2]])
    else:
        ext = np.vstack([p[:1], p, p[-1:]])
    dense = []
    for i in range(len(ext) - 3):
        p0, p1, p2, p3 = ext[i:i + 4]
        t = np.linspace(0.0, 1.0, 9, endpoint=False)[:, None]
        dense.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t ** 2 +
                            (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    dense.append(ext[-2][None, :])
    dense = np.vstack(dense)
    length = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(dense, axis=0).T))])
    at = np.arange(0.0, length[-1], TRAIL_STEP)
    return np.stack([np.interp(at, length, dense[:, 0]), np.interp(at, length, dense[:, 1])], axis=1)


def trails():
    """[(samples (n x 2), closed)] and a grid of (trail, sample) by 8 m square."""
    global _trails
    if _trails is None:
        root = ET.parse(SCENE).getroot()
        lines = []
        for item in root.find("objects").findall("item"):
            if item.get("name") != "Trails":
                continue
            for trail in item.find("children").findall("item"):
                spline = trail.find("components").find(f"{{{NAMESPACE}}}SplinePath")
                if spline is None or trail.get("enabled") == "false":
                    continue
                points = []
                for point in trail.find("children").findall("item"):
                    x, _, z = map(float, point.find("transform").find("Transform.position").text.split())
                    points.append((x, z))
                if len(points) >= 2:
                    lines.append((_catmull_rom(points, spline.get("SplinePath.closed") == "true"),
                                  spline.get("SplinePath.closed") == "true"))
        grid = {}
        for t, (samples, _) in enumerate(lines):
            for i, (x, z) in enumerate(samples):
                grid.setdefault((math.floor(x / 8.0), math.floor(z / 8.0)), []).append((t, i))
        if not lines:
            raise SystemExit(f"{SCENE.name} has no trail objects (a Trails group)")
        _trails = (lines, grid)
    return _trails


def nearest_trail(x, z, reach=12.0):
    """(distance, trail, sample) of the nearest trail sample within reach."""
    lines, grid = trails()
    best = None
    r = int(math.ceil(reach / 8.0))
    gx, gz = math.floor(x / 8.0), math.floor(z / 8.0)
    for i in range(gx - r, gx + r + 1):
        for j in range(gz - r, gz + r + 1):
            for t, k in grid.get((i, j), ()):
                px, pz = lines[t][0][k]
                d = math.hypot(px - x, pz - z)
                if d <= reach and (best is None or d < best[0]):
                    best = (d, t, k)
    return best


def trail_tangent(t, k):
    samples, closed = trails()[0][t]
    n = len(samples)
    a = samples[(k - 1) % n if closed else max(k - 1, 0)]
    b = samples[(k + 1) % n if closed else min(k + 1, n - 1)]
    d = math.hypot(*(b - a))
    return ((b[0] - a[0]) / d, (b[1] - a[1]) / d) if d > 1e-9 else (1.0, 0.0)


def trail_mask(x, z):
    """1 on a track, 0 away from it (its 1.6 m core, 4.2 m edge), and the
    track's direction there."""
    hit = nearest_trail(x, z, reach=6.0)
    if hit is None:
        return 0.0, (1.0, 0.0)
    d, t, k = hit
    return 1.0 - smoothstep(1.6, 4.2, d), trail_tangent(t, k)


# --- ground heights ----------------------------------------------------------

_cells = {}


def ground(x, z):
    cx, cz = math.floor(x / CELL), math.floor(z / CELL)
    if (cx, cz) not in _cells:
        heights, cells_x, _ = grass.read_cell(cx, cz)
        _cells[(cx, cz)] = (heights, cells_x + 1)
    heights, side = _cells[(cx, cz)]
    fx, fz = (x - cx * CELL) / SPACING, (z - cz * CELL) / SPACING
    ix, iz = min(int(fx), side - 2), min(int(fz), side - 2)
    tx, tz = fx - ix, fz - iz
    h = lambda i, j: heights[j * side + i]
    return ((h(ix, iz) * (1 - tx) + h(ix + 1, iz) * tx) * (1 - tz) +
            (h(ix, iz + 1) * (1 - tx) + h(ix + 1, iz + 1) * tx) * tz)


def wooded(x, z):
    """Forest cover: the mask where the height band lets trees grow (the
    valley floor sits near the band's lower edge, so the band only gates)."""
    height = ground(x, z)
    return forest_mask(x, z, height) if forest_band(height) > 0.6 else 0.0


def flat(x, z, radius=3.0):
    hs = [ground(x + dx, z + dz) for dx, dz in ((radius, 0), (-radius, 0), (0, radius), (0, -radius))]
    return max(hs) - min(hs) < 1.2


# --- search ------------------------------------------------------------------

def candidates(radius=420.0, step=8.0):
    """Grid points around the start, nearest first."""
    points = []
    n = int(radius / step)
    for i in range(-n, n + 1):
        for j in range(-n, n + 1):
            x, z = START[0] + i * step, START[1] + j * step
            if (x - START[0]) ** 2 + (z - START[1]) ** 2 <= radius * radius:
                points.append((x, z))
    points.sort(key=lambda p: (p[0] - START[0]) ** 2 + (p[1] - START[1]) ** 2)
    return points


def find_edge(direction=None):
    """Open meadow point with solid forest 22-40 m away along a direction."""
    if direction:
        # Within +-45 degrees of the requested heading, closest first.
        base = math.atan2(direction[1], direction[0])
        directions = [(math.cos(base + k * math.pi / 16), math.sin(base + k * math.pi / 16))
                      for k in (0, 1, -1, 2, -2, 3, -3, 4, -4)]
    else:
        directions = [(math.cos(a), math.sin(a)) for a in (k * math.pi / 8 for k in range(16))]
    for x, z in candidates():
        if forest_mask(x, z) > 0.02 or forest_band(ground(x, z)) < 0.6 or not flat(x, z):
            continue
        # Clear meadow around the camera so trees do not block the view.
        if any(wooded(x + dx, z + dz) > 0.05 for dx, dz in ((6, 0), (-6, 0), (0, 6), (0, -6))):
            continue
        for dx, dz in directions:
            if all(wooded(x + dx * d, z + dz * d) > 0.9 for d in (22, 30, 40)):
                return (x, z), (dx, dz)
    raise SystemExit("no forest edge found near the start")


def find_interior():
    for x, z in candidates():
        if not flat(x, z):
            continue
        ring = [(x + r * math.cos(a), z + r * math.sin(a))
                for r in (0, 12, 24, 36) for a in (k * math.pi / 4 for k in range(8))]
        if all(wooded(px, pz) > 0.97 for px, pz in ring):
            return x, z
    raise SystemExit("no forest interior found near the start")


def find_trail():
    """A point on a trail inside forest, and the trail's direction there."""
    for x, z in candidates(radius=700.0, step=4.0):
        mask, along = trail_mask(x, z)
        if mask < 0.98 or wooded(x, z) < 0.9 or not flat(x, z):
            continue
        # The track should carry on ahead through forest.
        if all(trail_mask(x + along[0] * d, z + along[1] * d)[0] > 0.3 and
               wooded(x + along[0] * d, z + along[1] * d) > 0.5 for d in (10, 20)):
            return (x, z), along
    raise SystemExit("no forest trail found near the start")


def find_cliff():
    """A viewing spot ~45 m downhill of the steepest slope within reach."""
    best = None
    for x, z in candidates(radius=4500.0, step=64.0):
        try:
            h = ground(x, z)
            dx = (ground(x + 6, z) - ground(x - 6, z)) / 12.0
            dz = (ground(x, z + 6) - ground(x, z - 6)) / 12.0
        except SystemExit:
            continue
        slope = math.hypot(dx, dz)
        # GenerateCliffs wants normal.y 0.42-0.76 (slope 0.86-2.2) and height > water + 25.
        if h < WATER + 25 or not 0.95 < slope < 2.0:
            continue
        # The viewer stands 45 m downhill, on dry ground.
        try:
            if ground(x - dx / slope * 45.0, z - dz / slope * 45.0) < WATER + 3:
                continue
        except SystemExit:
            continue
        score = slope - 0.0002 * math.hypot(x - START[0], z - START[1])
        if best is None or score > best[0]:
            best = (score, (x, z), (-dx / slope, -dz / slope))
    if best is None:
        raise SystemExit("no cliff found near the start")
    return best[1], best[2]


def find_meadow_slope():
    """Sunlit meadow on a slope with a forest edge 50-150 m ahead and to the
    side and lower ground beyond: the main look-check pose (.md/todo.md,
    Landscape demo expectations). The sun is behind or beside the camera."""
    sun_length = math.hypot(*SUN_FLAT)
    sun = (SUN_FLAT[0] / sun_length, SUN_FLAT[1] / sun_length)
    headings = [(math.cos(a), math.sin(a)) for a in (k * math.pi / 8 for k in range(16))]
    best = None
    for x, z in candidates(radius=2600.0, step=16.0):
        g = ground(x, z)
        if g < WATER + 4.0 or forest_band(g) < 0.6 or forest_mask(x, z, g) > 0.02:
            continue
        gx = (ground(x + 12, z) - ground(x - 12, z)) / 24.0
        gz = (ground(x, z + 12) - ground(x, z - 12)) / 24.0
        slope = math.degrees(math.atan(math.hypot(gx, gz)))
        if not 7.0 <= slope <= 22.0:
            continue
        down = (-gx / math.hypot(gx, gz), -gz / math.hypot(gx, gz))
        for dx, dz in headings:
            if dx * sun[0] + dz * sun[1] > 0.25:      # no looking into the sun
                continue
            along = dx * down[0] + dz * down[1]
            if not -0.2 <= along <= 0.85:              # across or down the slope
                continue
            # Open grass in front of the camera.
            if any(wooded(x + dx * d + sx * w, z + dz * d + sz * w) > 0.05
                   for d in (0, 12, 24, 40) for w in (-10, 0, 10)
                   for sx, sz in ((-dz, dx),)):
                continue
            # Sunlit: no forest within 30 m toward the sun of the camera or of
            # the grass ahead (the first pose stood in an edge tree's shade).
            if any(wooded(x + dx * d + sun[0] * s, z + dz * d + sun[1] * s) > 0.05
                   for d in (0, 15, 30) for s in (10, 20, 30)):
                continue
            # A forest edge 50-150 m away, within 55 degrees of the heading.
            edge = False
            for turn in (-0.95, -0.6, -0.3, 0.3, 0.6, 0.95):
                ex, ez = (dx * math.cos(turn) - dz * math.sin(turn), dx * math.sin(turn) + dz * math.cos(turn))
                if any(wooded(x + ex * d, z + ez * d) > 0.9 and wooded(x + ex * (d + 16), z + ez * (d + 16)) > 0.9
                       for d in (50, 70, 90, 110, 130, 150)):
                    edge = True
                    break
            if not edge:
                continue
            # Lower ground beyond: a valley behind the meadow.
            beyond = [ground(x + dx * d, z + dz * d) for d in (60, 120, 180, 250, 400, 550, 700)]
            if min(beyond) < WATER + 3.0:               # no lake in the view
                continue
            drop = g - min(beyond[3:])
            if drop < 25.0:
                continue
            score = drop + 2.0 * slope - 0.02 * math.hypot(x - START[0], z - START[1])
            if best is None or score > best[0]:
                best = (score, (x, z), (dx, dz), slope, drop)
        if best and math.hypot(x - START[0], z - START[1]) > 1500.0:
            break
    if not best:
        raise SystemExit("no meadow slope found near the start")
    print(f"MeadowSlope: slope {best[3]:.0f} deg, valley drop {best[4]:.0f} m")
    return best[1], best[2]


CANOPY = 22.0


def sight_blocked(eye, target, step=8.0):
    """True when ground or forest canopy stands between eye and target."""
    ex, ey, ez = eye
    tx, ty, tz = target
    length = math.hypot(tx - ex, tz - ez)
    count = max(2, int(length / step))
    for i in range(1, count - 1):
        t = i / count
        x, z = ex + (tx - ex) * t, ez + (tz - ez) * t
        top = ground(x, z) + (CANOPY if t * length > 4.0 and wooded(x, z) > 0.3 else 0.0)
        if top > ey + (ty - ey) * t:
            return True
    return False


def find_escarpment():
    """Eye and target for the escarpment: in front of the face, 150-500 m
    out, where the most of the face is in sight over the ground and the
    forest between (33 points on the face, ray-marched), looking at the
    middle of the face. The foot of the face is forest, so the ground poses
    see little of it; the eye is raised in 10 m steps, up to 50 m, until two
    thirds of the points are in sight (a hovering viewpoint, like the start
    pose). 2026-11-20: 19 of 32 at 50 m."""
    cx, cz = ESCARPMENT["centre"]
    fx, fz = ESCARPMENT["facing"]
    height = ESCARPMENT["height"]
    foot = ground(cx + fx * 60.0, cz + fz * 60.0)
    # Points on the face itself: the face line swings and is cut by gullies,
    # so each is found by walking in from the front until the ground reaches
    # that height, then stepping 4 m back out.
    samples = []
    for along in range(-300, 301, 60):
        for part in (0.15, 0.45, 0.8):
            level = foot + part * height
            for out in range(160, -200, -2):
                x, z = cx + fx * out - fz * along, cz + fz * out + fx * along
                if ground(x, z) >= level:
                    samples.append((x + fx * 4.0, level, z + fz * 4.0))
                    break
    best = None
    for lift in (0.0, 10.0, 20.0, 30.0, 40.0, 50.0):
        for out in range(150, 501, 25):
            for along in range(-200, 201, 40):
                x, z = cx + fx * out - fz * along, cz + fz * out + fx * along
                g = ground(x, z)
                if g < WATER + 3.0 or (lift == 0.0 and wooded(x, z) > 0.05):
                    continue
                eye = (x, g + EYE + lift, z)
                seen = sum(0 if sight_blocked(eye, sample) else 1 for sample in samples)
                score = seen - 0.004 * out - 0.004 * abs(along)
                if best is None or score > best[0]:
                    best = (score, eye, seen, lift)
        if best and best[2] >= len(samples) * 2 // 3:
            break
    if best is None:
        raise SystemExit("no viewpoint in front of the escarpment")
    _, eye, seen, lift = best
    print(f"Escarpment: {seen} of {len(samples)} face points in sight, eye {lift:.0f} m above the ground")
    return eye, (cx, foot + 0.45 * height, cz)


def find_forest_wall():
    """Open meadow with a forest edge 60-90 m ahead across the view and the
    sun behind the camera: the pose for the forest edge row of the look
    table (.md/todo.md), which is judged from 50-150 m."""
    sun_length = math.hypot(*SUN_FLAT)
    sun = (SUN_FLAT[0] / sun_length, SUN_FLAT[1] / sun_length)
    best = None
    for x, z in candidates(radius=1400.0, step=16.0):
        g = ground(x, z)
        if g < WATER + 4.0 or wooded(x, z) > 0.02:
            continue
        for k in range(16):
            dx, dz = math.cos(k * math.pi / 8), math.sin(k * math.pi / 8)
            if dx * sun[0] + dz * sun[1] > -0.3:
                continue
            if any(wooded(x + dx * d, z + dz * d) > 0.02 for d in range(0, 61, 10)):
                continue
            fan = total = 0
            for turn in (-0.5, -0.25, 0.0, 0.25, 0.5):
                ex, ez = dx * math.cos(turn) - dz * math.sin(turn), dx * math.sin(turn) + dz * math.cos(turn)
                for d in (100, 120, 140):
                    total += 1
                    fan += wooded(x + ex * d, z + ez * d) > 0.9
            first = next((d for d in range(60, 140, 5) if wooded(x + dx * d, z + dz * d) > 0.5), None)
            if first is None or fan < total - 2:
                continue
            edge_ground = ground(x + dx * first, z + dz * first)
            if abs(edge_ground - g) > 8.0:
                continue
            score = fan - 0.002 * math.hypot(x - START[0], z - START[1]) - 0.02 * abs(first - 80)
            if best is None or score > best[0]:
                best = (score, (x, g + EYE, z),
                        (x + dx * first, edge_ground + 5.0, z + dz * first), first)
    if best is None:
        raise SystemExit("no forest wall found near the start")
    print(f"ForestWall: forest starts {best[3]} m ahead")
    return best[1], best[2]


FIELD_WALK = 240.0


def find_field_walk():
    """Start and heading of a straight line over open meadow, FIELD_WALK
    metres long: no forest within 14 m either side of it, no water, not into
    the sun, and ground that stays close to the straight line between its
    ends. The benchmark's explorer flies along its gaze, so the gaze is laid
    along that line and the eye keeps 1-3 m above the ground."""
    sun_length = math.hypot(*SUN_FLAT)
    sun = (SUN_FLAT[0] / sun_length, SUN_FLAT[1] / sun_length)
    headings = [(math.cos(a), math.sin(a)) for a in (k * math.pi / 8 for k in range(16))]
    steps = [d * 10.0 for d in range(int(FIELD_WALK / 10) + 1)]
    best = None
    for x, z in candidates(radius=2600.0, step=16.0):
        if wooded(x, z) > 0.02 or ground(x, z) < WATER + 4.0:
            continue
        for dx, dz in headings:
            if dx * sun[0] + dz * sun[1] > 0.25:
                continue
            try:
                if any(wooded(x + dx * d - dz * w, z + dz * d + dx * w) > 0.02
                       for d in steps for w in (-14.0, 0.0, 14.0)):
                    continue
                heights = [ground(x + dx * d, z + dz * d) for d in steps]
            except SystemExit:
                continue
            if min(heights) < WATER + 4.0:
                continue
            # The eye's line: 1.7 m above the ground at the start, parallel to
            # the line between the ends, lifted so it clears every sample by 1 m.
            fall = (heights[-1] - heights[0]) / FIELD_WALK
            line = [heights[0] + fall * d for d in steps]
            lift = max(1.0, max(h - l for h, l in zip(heights, line)) + 1.0)
            clearance = [l + lift - h for h, l in zip(heights, line)]
            if max(clearance) > 3.0 or abs(fall) > 0.2:
                continue
            score = -max(clearance) - 0.0006 * math.hypot(x - START[0], z - START[1])
            if best is None or score > best[0]:
                best = (score, (x, heights[0] + lift, z),
                        (x + dx * 60.0, heights[0] + lift + fall * 60.0, z + dz * 60.0),
                        max(clearance), fall)
    if best is None:
        raise SystemExit("no open field walk found near the start")
    print(f"FieldWalk: eye at most {best[3]:.1f} m above the ground, slope {best[4] * 100:.0f} %")
    return best[1], best[2]


# --- trail walks -------------------------------------------------------------
#
# Routes a player would take: along the scene's trail objects, at eye
# height. Saved as SplinePath objects; GameBackendCapture --route <name>
# walks the game camera along one.

ROUTE_LENGTH = 400.0
ROUTE_SPACING = 6.0
ROUTES = ("ForestTrailWalk", "MeadowTrailWalk")


def onto_trail(x, z):
    """The nearest point on a trail's centre line: (x, z, trail, sample), or
    None away from every trail."""
    hit = nearest_trail(x, z)
    if hit is None:
        return None
    _, t, k = hit
    px, pz = trails()[0][t][0][k]
    return float(px), float(pz), t, k


def trail_heading(spot, sign):
    """Unit direction along the track at a spot; sign picks which way."""
    tx, tz = trail_tangent(spot[2], spot[3])
    return tx * sign, tz * sign


def follow_trail(spot, sign, length=ROUTE_LENGTH, step=2.0):
    """Points every `step` metres along the trail from a spot (to its end)."""
    samples, closed = trails()[0][spot[2]]
    k, stride = spot[3], max(1, int(round(step / TRAIL_STEP)))
    points = [(float(samples[k][0]), float(samples[k][1]))]
    while (len(points) - 1) * step < length:
        k += stride * sign
        if closed:
            k %= len(samples)
        elif not 0 <= k < len(samples):
            break
        points.append((float(samples[k][0]), float(samples[k][1])))
    return points


def slope_degrees(x, z, e=3.0):
    return math.degrees(math.atan(math.hypot((ground(x + e, z) - ground(x - e, z)) / (2 * e),
                                             (ground(x, z + e) - ground(x, z - e)) / (2 * e))))


def walkable(points):
    """Dry ground in the trail's height band (where it is drawn), no slope a
    walker would leave the track for."""
    return all(ground(x, z) > WATER + 6.0 and forest_band(ground(x, z)) > 0.9 and
               slope_degrees(x, z) < 16.0 for x, z in points[::3])


def route_points(points):
    """Every ROUTE_SPACING metres, at eye height (the track sinks 12 cm into
    the archive heights, which the eye ignores)."""
    every = max(1, int(round(ROUTE_SPACING / 2.0)))
    picked = points[::every]
    if picked[-1] != points[-1]:
        picked.append(points[-1])
    return [(x, ground(x, z) + EYE, z) for x, z in picked]


def find_forest_trail_walk(trail, along):
    """From the Trail viewpoint on along the track: forest, the forest edge,
    meadow, and forest again (2026-11-23)."""
    start = onto_trail(*trail)
    if start is None:
        raise SystemExit("the Trail viewpoint is not on a trail")
    hx, hz = trail_heading(start, 1)
    sign = 1 if hx * along[0] + hz * along[1] > 0 else -1
    points = follow_trail(start, sign)
    if not walkable(points):
        raise SystemExit("the forest trail from the Trail viewpoint is not walkable")
    cover = sum(wooded(x, z) > 0.5 for x, z in points) / len(points)
    print(f"ForestTrailWalk: {len(points) * 2 - 2} m, {cover * 100:.0f} % in forest")
    return route_points(points)


def find_meadow_trail_walk():
    """The open-meadow trail nearest the start, walked with the sun behind
    (sunlit grass ahead, as FieldWalk): open meadow 10 m either side of the
    track along at least 90 % of it."""
    sun_length = math.hypot(*SUN_FLAT)
    sun = (SUN_FLAT[0] / sun_length, SUN_FLAT[1] / sun_length)
    tried, best = [], None
    for x, z in candidates(radius=900.0, step=8.0):
        if trail_mask(x, z)[0] < 0.9:
            continue
        start = onto_trail(x, z)
        if start is None or any(math.hypot(start[0] - p[0], start[1] - p[1]) < 60.0 for p in tried):
            continue
        tried.append(start)
        for sign in (1, -1):
            try:
                points = follow_trail(start, sign)
                if len(points) < 20:
                    continue
                if not walkable(points):
                    continue
                open_share = 0.0
                for i, (px, pz) in enumerate(points):
                    nx, nz = points[min(i + 1, len(points) - 1)]
                    if (nx, nz) == (px, pz):
                        nx, nz = points[i - 1]
                        nx, nz = 2 * px - nx, 2 * pz - nz
                    dx, dz = nx - px, nz - pz
                    n = math.hypot(dx, dz)
                    side = (-dz / n, dx / n)
                    open_share += all(wooded(px + side[0] * w, pz + side[1] * w) < OPEN_COVER for w in (-10, 0, 10))
                open_share /= len(points)
            except SystemExit:
                continue
            if open_share < 0.9:
                continue
            # Either way along it: the one with the sun more behind.
            for route in (points, points[::-1]):
                facing = sum((route[i + 5][0] - route[i][0]) * sun[0] + (route[i + 5][1] - route[i][1]) * sun[1]
                             for i in range(0, len(route) - 5, 5)) / (ROUTE_LENGTH or 1.0)
                score = open_share - 0.5 * facing - 0.0005 * math.hypot(start[0] - START[0], start[1] - START[1])
                if best is None or score > best[0]:
                    best = (score, route, open_share, facing)
        if len(tried) >= 12:
            break
    if best is None:
        raise SystemExit("no open meadow trail near the start")
    print(f"MeadowTrailWalk: {best[2] * 100:.0f} % open meadow, sun {'behind' if best[3] < 0 else 'ahead'} "
          f"({best[3]:+.2f})")
    return route_points(best[1])


def route_object(name, points):
    """A SplinePath whose children are the control points."""
    item = ET.Element("item", {"name": name, "enabled": "true"})
    components = ET.SubElement(item, "components")
    ET.SubElement(components, f"{{{NAMESPACE}}}SplinePath", {
        "SplinePath.interpolation": "1", "SplinePath.closed": "false", "SplinePath.tension": "0",
        "SplinePath.arcLengthSamplesPerSegment": "16", "SplinePath.includeDisabledPoints": "false"})
    children = ET.SubElement(item, "children")
    for index, point in enumerate(points):
        child = ET.SubElement(children, "item", {"name": f"Point {index + 1}", "enabled": "true"})
        child.append(grass.transform(tuple(round(v, 3) for v in point)))
        ET.SubElement(child, "components")
        ET.SubElement(child, "children")
    item.append(grass.transform((0.0, 0.0, 0.0)))
    return item


# The camera the game starts with: walks MeadowTrailWalk at a walking pace,
# there and back (SplineFollower). It comes first in the scene, so the game
# picks it over the explorer (Scene::FindGameCamera takes the first active
# camera); GameBackendCapture runs switch path-following cameras off and use
# the explorer (GameApplication).
WALK_CAMERA = "MeadowWalkCamera"
WALK_ROUTE = "MeadowTrailWalk"
WALK_SPEED = 1.4  # m/s


def walk_camera(route_points_):
    length = sum(math.dist(a, b) for a, b in zip(route_points_[:-1], route_points_[1:]))
    item = ET.Element("item", {"name": WALK_CAMERA, "enabled": "true"})
    components = ET.SubElement(item, "components")
    camera = ET.SubElement(components, f"{{{NAMESPACE}}}Camera", {
        "Camera.active": "true", "Camera.useTransformRotation": "true", "Camera.fov": "67",
        "Camera.near": "0.06", "Camera.far": "24000", "Camera.orthographic": "false",
        "Camera.orthographicSize": "5"})
    ET.SubElement(camera, "Camera.up", {"_arrayEncoding": "spaceSeparatedNumbers"}).text = "0 1 0"
    ET.SubElement(components, f"{{{NAMESPACE}}}SplineFollower", {
        "SplineFollower.pathObjectName": WALK_ROUTE,
        "SplineFollower.durationSeconds": f"{length / WALK_SPEED:.1f}",
        "SplineFollower.startOffset": "0", "SplineFollower.playOnStart": "true",
        "SplineFollower.constantSpeed": "true", "SplineFollower.loop": "true",
        "SplineFollower.continuousLoop": "false", "SplineFollower.pingPong": "true",
        "SplineFollower.reverse": "false", "SplineFollower.easing": "0",
        "SplineFollower.orientation": "1"})
    ET.SubElement(item, "children")
    item.append(grass.transform(tuple(round(v, 3) for v in route_points_[0])))
    return item, length


def insert_walk_camera(objects, route_points_):
    for item in list(objects):
        if item.get("name") == WALK_CAMERA:
            objects.remove(item)
    item, length = walk_camera(route_points_)
    objects.insert(0, item)
    print(f"{WALK_CAMERA}: walks {WALK_ROUTE} ({length:.0f} m) at {WALK_SPEED} m/s, there and back")


def viewpoint(name, eye, target):
    item = ET.Element("item", {"name": name, "enabled": "true"})
    components = ET.SubElement(item, "components")
    camera = ET.SubElement(components, f"{{{NAMESPACE}}}Camera", {
        "Camera.active": "false", "Camera.useTransformRotation": "false", "Camera.fov": "67",
        "Camera.near": "0.06", "Camera.far": "24000", "Camera.orthographic": "false",
        "Camera.orthographicSize": "5"})
    ET.SubElement(camera, "Camera.up", {"_arrayEncoding": "spaceSeparatedNumbers"}).text = "0 1 0"
    ET.SubElement(camera, "Camera.target", {"_arrayEncoding": "spaceSeparatedNumbers"}).text = \
        " ".join(f"{v:.3f}" for v in target)
    ET.SubElement(item, "children")
    item.append(grass.transform(eye))
    return item


def main():
    edge, (ex, ez) = find_edge()
    floor = find_interior()
    back, (bx, bz) = find_edge(SUN_FLAT)

    def pose(point, direction, reach, rise, eye_height=EYE):
        x, z = point
        g = ground(x, z)
        tx, tz = x + direction[0] * reach, z + direction[1] * reach
        return (x, g + eye_height, z), (tx, ground(tx, tz) + rise, tz)

    # The forest floor looks toward the nearest open ground if there is one
    # within reach (light comes in from the edge), else along +x.
    trail, trail_along = find_trail()
    cliff, downhill = find_cliff()
    slope, slope_view = find_meadow_slope()
    stand = (cliff[0] + downhill[0] * 45.0, cliff[1] + downhill[1] * 45.0)
    stand_ground = ground(*stand)
    poses = {
        "Trail": pose(trail, trail_along, 30.0, 1.0),
        "Cliffs": ((stand[0], stand_ground + EYE, stand[1]),
                   (cliff[0], ground(*cliff) + 4.0, cliff[1])),
        "ForestEdge": pose(edge, (ex, ez), 45.0, 3.0),
        "ForestFloor": pose(floor, (ex, ez), 30.0, 0.8, 1.6),
        # Look up into the crowns toward the sun.
        "Backlit": pose(back, (bx, bz), 40.0, 18.0),
        # Level gaze across the slope: meadow below, forest edge and far hills above.
        "MeadowSlope": ((slope[0], ground(*slope) + EYE, slope[1]),
                        (slope[0] + slope_view[0] * 60.0, ground(*slope) + EYE - 2.0, slope[1] + slope_view[1] * 60.0)),
        "Escarpment": find_escarpment(),
        "ForestWall": find_forest_wall(),
        "FieldWalk": find_field_walk(),
    }
    routes = {
        "ForestTrailWalk": find_forest_trail_walk(trail, trail_along),
        "MeadowTrailWalk": find_meadow_trail_walk(),
    }

    ET.register_namespace("components", NAMESPACE)
    text = SCENE.read_text(encoding="utf-8")
    header_end = text.index("<Scene")
    header = text[:header_end]
    root = ET.fromstring(text[header_end:])
    objects = root.find("objects")
    for item in list(objects):
        if item.get("name") in NAMES or item.get("name") in ROUTES:
            objects.remove(item)
    vista_index = next((i for i, item in enumerate(objects) if item.get("name") == "Vista"), len(objects) - 1)
    for offset, name in enumerate(NAMES):
        eye, target = poses[name]
        objects.insert(vista_index + 1 + offset, viewpoint(name, eye, target))
        print(f"{name}: eye ({eye[0]:.0f}, {eye[1]:.1f}, {eye[2]:.0f}) -> "
              f"target ({target[0]:.0f}, {target[1]:.1f}, {target[2]:.0f})")
    for offset, name in enumerate(ROUTES):
        points = routes[name]
        objects.insert(vista_index + 1 + len(NAMES) + offset, route_object(name, points))
        print(f"{name}: {len(points)} points from ({points[0][0]:.0f}, {points[0][2]:.0f}) "
              f"to ({points[-1][0]:.0f}, {points[-1][2]:.0f})")
    # The game starts on the flying explorer (user, 2026-11-25): no walk
    # camera. insert_walk_camera(objects, routes[WALK_ROUTE]) brings it back.
    for item in list(objects):
        if item.get("name") == WALK_CAMERA:
            objects.remove(item)
    # Everything but the explorer is static (tools/scene_static.py); this
    # also covers the objects build_landscape_demo.py just wrote.
    scene_static.mark_static(objects)
    ET.indent(root, "  ")
    SCENE.write_text(header + ET.tostring(root, encoding="unicode") + "\n", encoding="utf-8",
                     newline="\n")  # keep the scene's LF line endings


if __name__ == "__main__":
    main()
