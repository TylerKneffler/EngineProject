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

Forests are placed at runtime by LandscapeDriver from ForestMask (value noise
over world metres); ForestMask/ForestBand below replicate it exactly, and
ground heights come from the landscape height archive. Editor: Viewpoints
menu. Captures: GameBackendCapture --viewpoint ForestEdge.

Re-running replaces the viewpoints. build_landscape_demo.py calls this after
writing the scene.

Usage: python tools/add_landscape_viewpoints.py
"""

import math
import xml.etree.ElementTree as ET
from pathlib import Path

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


# --- LandscapeDriver.cpp ForestMask / ForestBand, bit for bit ---------------

def _hash(x, z, seed):
    m = 0xFFFFFFFF
    h = ((x & m) * 0x9E3779B1 & m) ^ ((z & m) * 0x85EBCA77 & m) ^ (seed * 0xC2B2AE3D & m)
    h ^= h >> 15
    h = h * 0x2C1B3C6D & m
    h ^= h >> 12
    h = h * 0x297A2D39 & m
    h ^= h >> 15
    return h


def value_noise(x, z, period, seed):
    x, z = x / period, z / period
    fx, fz = math.floor(x), math.floor(z)
    ix, iz = int(fx), int(fz)
    tx, tz = x - fx, z - fz
    tx, tz = tx * tx * (3 - 2 * tx), tz * tz * (3 - 2 * tz)
    corner = lambda dx, dz: (_hash(ix + dx, iz + dz, seed) & 0xFFFFFF) / float(0xFFFFFF)
    return ((corner(0, 0) * (1 - tx) + corner(1, 0) * tx) * (1 - tz) +
            (corner(0, 1) * (1 - tx) + corner(1, 1) * tx) * tz)


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def forest_mask(x, z, height=None):
    """height: ground metres at x, z (read from the archive when omitted)."""
    if height is None:
        height = ground(x, z)
    n = (0.6 * value_noise(x, z, 260.0, 11) + 0.28 * value_noise(x, z, 95.0, 23) +
         0.12 * value_noise(x, z, 37.0, 37) +
         0.06 * smoothstep(WATER + 30.0, WATER + 80.0, height))
    return smoothstep(0.46, 0.58, n)


def forest_band(height):
    return smoothstep(WATER + 4, WATER + 9, height) * (1 - smoothstep(TREE_LINE - 8 * BLEND, TREE_LINE, height))


def trail_field(x, z):
    return 0.7 * value_noise(x, z, 520.0, 71) + 0.3 * value_noise(x, z, 170.0, 73)


def trail_mask(x, z):
    """LandscapeDriver.cpp TrailMask: 1 on a track, 0 away from it."""
    e = 4.0
    gx = (trail_field(x + e, z) - trail_field(x - e, z)) / (2 * e)
    gz = (trail_field(x, z + e) - trail_field(x, z - e)) / (2 * e)
    distance = abs(trail_field(x, z) - 0.5) / max(math.hypot(gx, gz), 0.0006)
    return 1.0 - smoothstep(1.6, 4.2, distance), (gx, gz)


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
        mask, (gx, gz) = trail_mask(x, z)
        if mask < 0.98 or wooded(x, z) < 0.9 or not flat(x, z):
            continue
        length = math.hypot(gx, gz)
        if length < 1e-6:
            continue
        along = (-gz / length, gx / length)
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

    ET.register_namespace("components", NAMESPACE)
    text = SCENE.read_text(encoding="utf-8")
    header_end = text.index("<Scene")
    header = text[:header_end]
    root = ET.fromstring(text[header_end:])
    objects = root.find("objects")
    for item in list(objects):
        if item.get("name") in NAMES:
            objects.remove(item)
    vista_index = next((i for i, item in enumerate(objects) if item.get("name") == "Vista"), len(objects) - 1)
    for offset, name in enumerate(NAMES):
        eye, target = poses[name]
        objects.insert(vista_index + 1 + offset, viewpoint(name, eye, target))
        print(f"{name}: eye ({eye[0]:.0f}, {eye[1]:.1f}, {eye[2]:.0f}) -> "
              f"target ({target[0]:.0f}, {target[1]:.1f}, {target[2]:.0f})")
    # Everything but the explorer is static (tools/scene_static.py); this
    # also covers the objects build_landscape_demo.py just wrote.
    scene_static.mark_static(objects)
    ET.indent(root, "  ")
    SCENE.write_text(header + ET.tostring(root, encoding="unicode") + "\n", encoding="utf-8",
                     newline="\n")  # keep the scene's LF line endings


if __name__ == "__main__":
    main()
