#!/usr/bin/env python3
"""Crown footprint of every woodland tree at scale 1, for the far-terrain
canopy that LandscapeDriver splats from the real tree placements.

Per prefab (Oaks, Conifers, Broadleaf), from its LOD0 parts: the crown radius
(95th percentile horizontal distance of the alpha-masked parts' vertices from
the trunk axis) and the area its leaves cover seen from straight above (m^2,
alpha-tested as drawn). Writes Prefabs/Woodland/tree_crowns.json, which the
driver reads at load (keyed by prefab path below Prefabs/Woodland, without
extension). Re-run when the tree models change.

Usage: python tools/measure_tree_crowns.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402
import match_lod_coverage as mlc  # noqa: E402

SCALE = 8.0
OUTPUT = mlc.PREFABS / "tree_crowns.json"


def main():
    crowns = {}
    for family in mlc.FAMILIES:
        for prefab in sorted((mlc.PREFABS / family).glob("*.prefab")):
            parts = mlc.parts_at(prefab, 0)
            if not parts:
                continue
            surfaces = [p[3] for p in parts]
            leaves = [p[3] for p in parts if p[4]] or surfaces
            pts = np.concatenate([s.p for s in leaves])
            radius = float(np.percentile(np.hypot(pts[:, 0], pts[:, 2]), 95))
            allp = np.concatenate([s.p for s in surfaces])
            extent = float(np.max(np.hypot(allp[:, 0], allp[:, 2]))) + 1.0
            size = int(2 * extent * SCALE) + 4
            f = np.array([0.0, 1.0, 0.0]); r = np.array([1.0, 0, 0]); u = np.cross(f, r)
            _, a, _, _ = wa.rasterize(surfaces, r, u, f, (-extent, -extent), SCALE, (size, size))
            area = float(a.sum()) / SCALE ** 2
            key = f"{family}/{prefab.stem}"
            crowns[key] = {"radius": round(radius, 2), "area": round(area, 1)}
            print(f"  {key}: radius {radius:.2f} m, area {area:.1f} m2", flush=True)
    document = {
        "description": "Crown radius (m) and leaf area seen from above (m^2) per tree at scale 1, "
                       "from LOD0 (tools/measure_tree_crowns.py). Read by LandscapeDriver for the far canopy.",
        "crowns": crowns,
    }
    OUTPUT.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {OUTPUT.relative_to(wa.ROOT)} ({len(crowns)} trees)")


if __name__ == "__main__":
    main()
