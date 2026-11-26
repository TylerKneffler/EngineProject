"""Trace the landscape demo's old noise trails into spline trail objects.

The landscape's trails used to be the 0.5 contour of a two-octave value noise
(TerrainManager::ContourPaths, set in LandscapeDriver code), kept to the
forest's height band. Trails are now scene objects: a SplinePath (child
objects are control points) with a TerrainTrail script that draws the trail
at startup. This tool traces that contour over the 4 m play area, cuts it
where the old height band hid it, simplifies each run into control points,
and writes the runs into landscape_demo.scene as children of a "Trails"
object (replacing an existing one), placed before the terrain so the trails
are published before its cells build.

Usage: python tools/landscape_trails.py [--half-extent METRES] [--dry-run]
"""

import argparse
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
ARCHIVE = ASSETS / "TerrainData" / "Landscape" / "world.heights"
SCENE = ASSETS / "Scenes" / "terrain" / "landscape_demo.scene"
NAMESPACE = "urn:engine-components"
ARCHIVE_MAGIC = 0x31544748524E5254
SPACING = 4.0          # archive metres per sample in the play area

# The old trail network (LandscapeDriver TrailDistance / ConfigureTerrain).
NOISE = ((520.0, 0.7, 71), (170.0, 0.3, 73))
CONTOUR = 0.5
WATER, TREE_LINE, BLEND = -40.0, 950.0, 6.0   # landscape_demo.scene LandscapeDriver
BAND_LOW = WATER + 6.5                         # middle of the old rise (water + 4 .. + 9)
BAND_HIGH = TREE_LINE - 4.0 * BLEND            # middle of the old fall (tree - 8 blend .. tree)

# The traced trail as the TerrainTrail script draws it.
TRAIL = {
    "terrainObjectName": "Landscape",
    "halfWidth": "1.6", "edgeHalfWidth": "4.2", "strength": "0.9", "carveDepth": "0.12",
    "layer": "1",                  # gravel
    "crossableLayers": "14",       # gravel, meadow, forest soil (not sand, rock, snow)
    "sampleSpacing": "1", "simplifyTolerance": "0.05",
}
GRID = 2.0             # contour tracing grid, metres
TOLERANCE = 0.6        # control point simplification, metres
MIN_RUN = 40.0         # shorter runs (band edges, map edge) are dropped


def hash32(x, z, seed):
    x = x.astype(np.int64).astype(np.uint32)
    z = z.astype(np.int64).astype(np.uint32)
    with np.errstate(over="ignore"):
        h = (x * np.uint32(0x9E3779B1)) ^ (z * np.uint32(0x85EBCA77)) ^ np.uint32((seed * 0xC2B2AE3D) & 0xFFFFFFFF)
        h ^= h >> np.uint32(15)
        h *= np.uint32(0x2C1B3C6D)
        h ^= h >> np.uint32(12)
        h *= np.uint32(0x297A2D39)
        h ^= h >> np.uint32(15)
    return h


def value_noise(x, z, period, seed):
    x = x / period
    z = z / period
    fx, fz = np.floor(x), np.floor(z)
    ix, iz = fx.astype(np.int64), fz.astype(np.int64)
    tx, tz = x - fx, z - fz
    tx = tx * tx * (3 - 2 * tx)
    tz = tz * tz * (3 - 2 * tz)

    def corner(dx, dz):
        return (hash32(ix + dx, iz + dz, seed) & np.uint32(0xFFFFFF)).astype(np.float64) / float(0xFFFFFF)

    return ((corner(0, 0) * (1 - tx) + corner(1, 0) * tx) * (1 - tz) +
            (corner(0, 1) * (1 - tx) + corner(1, 1) * tx) * tz)


def field(x, z):
    return sum(weight * value_noise(x, z, period, seed) for period, weight, seed in NOISE)


def read_play_heights():
    """Heights of the 4 m play area: (grid, origin x/z in metres)."""
    data = ARCHIVE.read_bytes()
    header = struct.Struct("<QIQiiifii")
    magic, version, _, extent, cells_x, cells_y, scale, fine, coarse = header.unpack_from(data, 0)
    if magic != ARCHIVE_MAGIC or version != 3 or cells_x != cells_y:
        raise SystemExit(f"unexpected archive {ARCHIVE}")
    cells = cells_x
    side_fine, side_coarse = cells + 1, cells // coarse + 1
    size = 2 * fine * cells + cells + 1
    heights = np.zeros((size, size), np.float32)
    offset = header.size
    for cell_z in range(-extent, extent + 1):
        for cell_x in range(-extent, extent + 1):
            in_play = max(abs(cell_x), abs(cell_z)) <= fine
            side = side_fine if in_play else side_coarse
            if in_play:
                block = np.frombuffer(data, "<i2", side * side, offset).reshape(side, side)
                r0, c0 = (cell_z + fine) * cells, (cell_x + fine) * cells
                heights[r0:r0 + side, c0:c0 + side] = block / scale
            offset += side * side * 2 + (side * side + 3) // 4
    origin = -fine * cells * SPACING
    return heights, origin


def sample_heights(heights, origin, x, z):
    gx = np.clip((x - origin) / SPACING, 0, heights.shape[1] - 1.001)
    gz = np.clip((z - origin) / SPACING, 0, heights.shape[0] - 1.001)
    c, r = np.floor(gx).astype(int), np.floor(gz).astype(int)
    tx, tz = gx - c, gz - r
    return ((heights[r, c] * (1 - tx) + heights[r, c + 1] * tx) * (1 - tz) +
            (heights[r + 1, c] * (1 - tx) + heights[r + 1, c + 1] * tx) * tz)


def trace(values, x0, z0, step):
    """Marching squares on values - CONTOUR: polylines of (x, z) and whether each is closed."""
    v = values - CONTOUR
    rows, cols = v.shape

    def crossing(key):
        kind, r, c = key
        if kind == 0:   # horizontal edge (r, c) - (r, c + 1)
            a, b = v[r, c], v[r, c + 1]
            t = a / (a - b)
            return x0 + (c + t) * step, z0 + r * step
        a, b = v[r, c], v[r + 1, c]   # vertical edge (r, c) - (r + 1, c)
        t = a / (a - b)
        return x0 + c * step, z0 + (r + t) * step

    above = v > 0
    # Case index per cell: corners (r,c), (r,c+1), (r+1,c+1), (r+1,c).
    case = (above[:-1, :-1].astype(np.uint8) | (above[:-1, 1:] << 1) |
            (above[1:, 1:] << 2) | (above[1:, :-1] << 3))
    links = {}

    def link(a, b):
        links.setdefault(a, []).append(b)
        links.setdefault(b, []).append(a)

    for r, c in zip(*np.nonzero((case != 0) & (case != 15))):
        k = int(case[r, c])
        bottom, right, top, left = (0, r, c), (1, r, c + 1), (0, r + 1, c), (1, r, c)
        edges = []
        if (k & 1) != ((k >> 1) & 1): edges.append(bottom)
        if ((k >> 1) & 1) != ((k >> 2) & 1): edges.append(right)
        if ((k >> 2) & 1) != ((k >> 3) & 1): edges.append(top)
        if ((k >> 3) & 1) != (k & 1): edges.append(left)
        if len(edges) == 2:
            link(edges[0], edges[1])
        else:   # saddle: decide by the centre
            centre = v[r:r + 2, c:c + 2].mean() > 0
            if (k == 5) == centre:
                link(bottom, right); link(top, left)
            else:
                link(bottom, left); link(right, top)

    lines = []
    seen = set()
    for start in list(links):
        if start in seen or len(links[start]) != 1:
            continue
        lines.append(walk(start, links, seen))
    for start in list(links):
        if start not in seen:
            chain = walk(start, links, seen)
            lines.append(chain + [chain[0]])
    return [(np.array([crossing(key) for key in chain]), chain[0] == chain[-1] and len(chain) > 2)
            for chain in lines if len(chain) >= 2]


def walk(start, links, seen):
    chain, previous, current = [start], None, start
    seen.add(start)
    while True:
        nxt = [n for n in links[current] if n != previous and n not in seen]
        if not nxt:
            return chain
        previous, current = current, nxt[0]
        seen.add(current)
        chain.append(current)


def simplify(points, tolerance):
    keep = np.zeros(len(points), bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        p, q = points[a], points[b]
        d = q - p
        length = np.dot(d, d)
        seg = points[a + 1:b]
        t = np.clip(((seg - p) @ d) / length, 0, 1) if length > 0 else np.zeros(len(seg))
        dist = np.linalg.norm(seg - (p + t[:, None] * d), axis=1)
        worst = int(np.argmax(dist))
        if dist[worst] > tolerance:
            keep[a + 1 + worst] = True
            stack += [(a, a + 1 + worst), (a + 1 + worst, b)]
    return points[keep]


def runs_in_band(points, closed, heights, origin):
    """Splits a polyline where the old height band hid the trail."""
    h = sample_heights(heights, origin, points[:, 0], points[:, 1])
    inside = (h > BAND_LOW) & (h < BAND_HIGH)
    if inside.all():
        return [(points, closed)]
    runs, start = [], None
    for i, ok in enumerate(inside):
        if ok and start is None:
            start = i
        if not ok and start is not None:
            runs.append(points[start:i])
            start = None
    if start is not None:
        runs.append(points[start:])
    # A closed loop cut once: join its two ends.
    if closed and len(runs) > 1 and inside[0] and inside[-1]:
        runs[0] = np.concatenate([runs.pop(), runs[0][1:]])
    return [(run, False) for run in runs]


def length_of(points):
    return float(np.linalg.norm(np.diff(points, axis=0), axis=1).sum())


def numbers(values):
    return " ".join(f"{v:.6g}" for v in values)


def add_transform(item, position):
    transform = ET.SubElement(item, "transform")
    for name, value in (("position", position), ("rotation", (0, 0, 0)), ("scale", (1, 1, 1))):
        element = ET.SubElement(transform, f"Transform.{name}", _arrayEncoding="spaceSeparatedNumbers")
        element.text = numbers(value)


def trail_item(name, points, closed, heights, origin):
    item = ET.Element("item", name=name, enabled="true", static="true")
    add_transform(item, (0, 0, 0))
    components = ET.SubElement(item, "components")
    ET.SubElement(components, f"{{{NAMESPACE}}}SplinePath", {
        "SplinePath.interpolation": "1", "SplinePath.closed": "true" if closed else "false",
        "SplinePath.tension": "0", "SplinePath.arcLengthSamplesPerSegment": "16",
        "SplinePath.includeDisabledPoints": "false"})
    script = ET.SubElement(components, f"{{{NAMESPACE}}}TerrainTrail",
                           {f"TerrainTrail.{key}": value for key, value in TRAIL.items()})
    far = ET.SubElement(script, "TerrainTrail.farColor", _arrayEncoding="spaceSeparatedNumbers")
    far.text = "-1 -1 -1"
    children = ET.SubElement(item, "children")
    if closed:
        points = points[:-1]
    ys = sample_heights(heights, origin, points[:, 0], points[:, 1])
    for index, ((x, z), y) in enumerate(zip(points, ys), 1):
        point = ET.SubElement(children, "item", name=f"Point {index}", enabled="true")
        add_transform(point, (round(float(x), 3), round(float(y), 3), round(float(z), 3)))
        ET.SubElement(point, "components")
        ET.SubElement(point, "children")
    return item


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--half-extent", type=float, default=None,
                        help="trace within +-METRES of the origin (default: the whole play area)")
    parser.add_argument("--dry-run", action="store_true", help="report, do not write the scene")
    args = parser.parse_args()

    heights, origin = read_play_heights()
    span = (heights.shape[0] - 1) * SPACING
    low, high = origin, origin + span
    if args.half_extent:
        low, high = max(low, -args.half_extent), min(high, args.half_extent)
    count = int((high - low) / GRID) + 1
    axis = low + np.arange(count) * GRID
    xs, zs = np.meshgrid(axis, axis)
    values = field(xs, zs)
    trails = []
    for points, closed in trace(values, low, low, GRID):
        for run, run_closed in runs_in_band(points, closed, heights, origin):
            if len(run) < 2 or length_of(run) < MIN_RUN:
                continue
            trails.append((simplify(run, TOLERANCE), run_closed, length_of(run)))
    trails.sort(key=lambda t: -t[2])
    total = sum(t[2] for t in trails)
    points = sum(len(t[0]) for t in trails)
    print(f"{len(trails)} trails, {total / 1000:.1f} km, {points} control points "
          f"({total / max(points, 1):.1f} m apart) over {low:.0f}..{high:.0f} m")
    if args.dry_run:
        return

    ET.register_namespace("components", NAMESPACE)
    text = SCENE.read_text(encoding="utf-8")
    tree = ET.ElementTree(ET.fromstring(text))
    objects = tree.getroot().find("objects")
    for existing in [item for item in objects.findall("item") if item.get("name") == "Trails"]:
        objects.remove(existing)
    group = ET.Element("item", name="Trails", enabled="true", static="true")
    add_transform(group, (0, 0, 0))
    ET.SubElement(group, "components")
    children = ET.SubElement(group, "children")
    for index, (run, closed, _) in enumerate(trails, 1):
        children.append(trail_item(f"Trail {index:03d}", run, closed, heights, origin))
    # Before the terrain: trails publish before its cells build.
    items = list(objects)
    landscape = next(i for i, item in enumerate(items) if item.get("name") == "Landscape")
    objects.insert(landscape, group)
    ET.indent(tree, "  ")
    body = ET.tostring(tree.getroot(), encoding="unicode")
    # Keep the file's header comment.
    head = text[:text.index("<Scene")]
    SCENE.write_text(head + body + "\n", encoding="utf-8")
    print(f"wrote {SCENE.relative_to(ROOT)}")


if __name__ == "__main__":
    sys.exit(main())
