"""Adds eye-level foliage viewpoints to the landscape demo scene.

The demo's start pose flies 25 m above the meadow, where grass and bushes
(drawn within ~26 m of the viewer) are out of view. This adds saved viewpoint
cameras near the start for checking foliage (.md/foliage.md), next to the
existing "Vista":

  ForestEdge   standing in the meadow, looking at an oak forest edge
  ForestFloor  inside a forest, looking across the forest floor
  Backlit      in the meadow with forest between the camera and the sun

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

SCENE = grass.ASSETS / "Scenes" / "terrain" / "landscape_demo.scene"
NAMESPACE = grass.NAMESPACE
START = (0.0, 40.0)
CELL = 128.0
SPACING = 4.0
EYE = 1.7
WATER, TREE_LINE, BLEND = -40.0, 950.0, 6.0
# Direction toward the "Late afternoon sun" (rotation 0.7, 2.4 rad: the light
# travels along the rotated +z axis), flattened onto the ground.
SUN_FLAT = (-0.517, 0.564)
NAMES = ("ForestEdge", "ForestFloor", "Backlit", "Trail", "Cliffs")


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


def forest_mask(x, z):
    n = (0.6 * value_noise(x, z, 260.0, 11) + 0.28 * value_noise(x, z, 95.0, 23) +
         0.12 * value_noise(x, z, 37.0, 37))
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
    return forest_mask(x, z) if forest_band(ground(x, z)) > 0.6 else 0.0


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
    stand = (cliff[0] + downhill[0] * 45.0, cliff[1] + downhill[1] * 45.0)
    stand_ground = ground(*stand)
    poses = {
        "Trail": pose(trail, trail_along, 30.0, 1.0),
        "Cliffs": ((stand[0], stand_ground + EYE, stand[1]),
                   (cliff[0], ground(*cliff) + 4.0, cliff[1])),
        "ForestEdge": pose(edge, (ex, ez), 45.0, 3.0),
        "ForestFloor": pose(floor, (ex, ez), 30.0, 0.8, 1.6),
        # Look up into the crowns toward the sun (~40 deg high).
        "Backlit": pose(back, (bx, bz), 40.0, 18.0),
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
    ET.indent(root, "  ")
    SCENE.write_text(header + ET.tostring(root, encoding="unicode") + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
