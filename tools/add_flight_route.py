"""Writes the FlightTour route into landscape_demo.scene: a loop flown at about
CLEARANCE metres above the ground (smoothed so the flight does not follow every
bump), over meadow, forest, cliffs and lakes, for automated fast-flight runs
(.local/routes/fly.py). An ordinary SplinePath object, editable in the editor.
Replaces an existing FlightTour item, else inserts it before ForestTrailWalk.
build_landscape_demo.py runs it after the viewpoints, so a rebuild keeps it.

Usage: python tools/add_flight_route.py [clearance_m]"""
import math
import re
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
import add_landscape_viewpoints as v  # noqa: E402

NAME = "FlightTour"
CLEARANCE = 60.0
SPACING = 40.0      # metres between control points
LOW_CLEARANCE, LOW_WINDOW = 30.0, 120.0   # FlightTourLow
WINDOW = 400.0      # the height follows the highest ground within +-WINDOW/2 along the path
# Loop from above MeadowSlope: north over the valley to the cliffs, east, south
# along the far side and back west (inside the +-2.9 km play area).
WAYPOINTS = [(-368, -1032), (-250, 0), (-800, 1900), (600, 2350), (2000, 1300), (2350, -700),
             (1100, -2200), (-1200, -2350), (-2250, -1000), (-1900, 500), (-700, -400), (-368, -1032)]


def densify(points):
    out = []
    for (x0, z0), (x1, z1) in zip(points, points[1:]):
        steps = max(1, int(math.hypot(x1 - x0, z1 - z0) / SPACING))
        out += [(x0 + (x1 - x0) * i / steps, z0 + (z1 - z0) * i / steps) for i in range(steps)]
    out.append(points[-1])
    return out


def smooth_corners(points, passes=6):
    # Chaikin-style relaxation so the turns are gentle banks, not kinks.
    for _ in range(passes):
        points = [points[0]] + [((a[0] + 2 * b[0] + c[0]) / 4, (a[1] + 2 * b[1] + c[1]) / 4)
                                for a, b, c in zip(points, points[1:], points[2:])] + [points[-1]]
    return points


def write_route(name, clearance, window):
    path = smooth_corners(densify(WAYPOINTS))
    grounds = [v.ground(x, z) for x, z in path]
    reach = max(1, int(window / 2 / SPACING))
    heights = [max(grounds[max(0, i - reach): i + reach + 1]) + clearance for i in range(len(path))]
    # Ease the profile (moving average) so climbs and descents are gradual.
    eased = [sum(heights[max(0, i - 3): i + 4]) / len(heights[max(0, i - 3): i + 4]) for i in range(len(heights))]
    heights = [max(e, g + clearance * .6) for e, g in zip(eased, grounds)]
    route = [(x, y, z) for (x, z), y in zip(path, heights)]
    length = sum(math.dist(a, b) for a, b in zip(route, route[1:]))

    ET.register_namespace("components", v.NAMESPACE)
    item = v.route_object(name, route)
    item.set("static", "true")
    ET.indent(item, "  ")
    xml = ET.tostring(item, encoding="unicode").replace(f' xmlns:components="{v.NAMESPACE}"', "")
    text = v.SCENE.read_text(encoding="utf-8")
    existing = re.search(rf'(?ms)^(\s*)<item name="{name}".*?^\1</item>\n', text)
    if existing:
        indent = existing.group(1)
        block = "\n".join(indent + line for line in xml.split("\n")) + "\n"
        text = text.replace(existing.group(0), block)
    else:
        anchor = re.search(r'(?m)^(\s*)<item name="ForestTrailWalk"', text)
        if not anchor:
            raise SystemExit("ForestTrailWalk not found (insertion point)")
        indent = anchor.group(1)
        block = "\n".join(indent + line for line in xml.split("\n")) + "\n"
        text = text[:anchor.start()] + block + text[anchor.start():]
    v.SCENE.write_text(text, encoding="utf-8", newline="\n")
    print(f"{name}: {length / 1000:.1f} km, {len(route)} points, height above ground "
          f"{min(h - g for h, g in zip(heights, grounds)):.0f}-{max(h - g for h, g in zip(heights, grounds)):.0f} m")


def main(clearance=CLEARANCE):
    write_route(NAME, clearance, WINDOW)
    # Low pass: skimming the canopy (the near trees' LOD switches), hugging
    # the ground more closely.
    write_route(NAME + "Low", LOW_CLEARANCE, LOW_WINDOW)


if __name__ == "__main__":
    main(float(sys.argv[1]) if len(sys.argv) > 1 else CLEARANCE)
