"""Low-limb reach of the landscape's tree species, for trail clearance.

usage: python tools/measure_tree_reach.py

For each tree prefab LandscapeDriver places (Oaks, Conifers, Broadleaf), reads
its LOD0 meshes and prints, per 0.25 m height slice from the ground to 4 m
(model units, scale 1), the largest horizontal distance of any vertex from
the trunk axis (x = z = 0). LandscapeDriver keeps the table (TreeReach) and
rejects trees whose limbs, at their scale, reach a trail's walking line in the
band a walker's eye sweeps (0.3-2.6 m above the ground). Re-run when tree
models change and paste the output over the table.
"""

import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np

from woodland_assets import ASSETS, read_native_mesh

SLICE = 0.25
SLICES = 16
GROUPS = {
    "Oaks": ["Big_oak_tree_001", "Big_oak_tree_002", "Large_oak_tree_001", "Large_oak_tree_002",
             "Medium_oak_tree_001", "Medium_oak_tree_002", "Small_oak_tree_001", "Sapling_oak_tree_001"],
    "Conifers": ["Spruce_tall_01", "Pine_scots_01", "Fir_subalpine_01", "Spruce_young_01"],
    "Broadleaf": ["Broadleaf_slender_01", "Broadleaf_upright_01"],
}


def numbers(node):
    return [float(item.text) for item in node.findall("item")] if node is not None else None


def matrix(transform):
    if transform is None:
        return np.eye(4)
    position = numbers(transform.find("Transform.position")) or [0, 0, 0]
    rotation = numbers(transform.find("Transform.rotation")) or [0, 0, 0]
    scale = numbers(transform.find("Transform.scale")) or [1, 1, 1]
    rx, ry, rz = np.radians(rotation)
    cx, sx, cy, sy, cz, sz = np.cos(rx), np.sin(rx), np.cos(ry), np.sin(ry), np.cos(rz), np.sin(rz)
    rot_x = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    rot_y = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rot_z = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    m = np.eye(4)
    m[:3, :3] = rot_y @ rot_x @ rot_z @ np.diag(scale)
    m[:3, 3] = position
    return m


def points(prefab):
    root = ET.parse(prefab).getroot()
    found = []

    def walk(node, parent):
        for item in node:
            if item.tag not in ("object", "item"):
                continue
            world = parent @ matrix(item.find("transform"))
            components = item.find("components")
            if components is not None:
                for component in components:
                    path = component.get("Mesh.file")
                    if path:
                        mesh = ASSETS / path.removeprefix("Assets/")
                        if mesh.exists():
                            p = read_native_mesh(mesh)[0]
                            found.append((np.c_[p, np.ones(len(p))] @ world.T)[:, :3])
            children = item.find("children")
            if children is not None:
                walk(children, world)

    walk(root, np.eye(4))
    return np.concatenate(found) if found else np.zeros((0, 3))


def main():
    for group, species in GROUPS.items():
        for name in species:
            p = points(ASSETS / "Prefabs" / "Woodland" / group / f"{name}.prefab")
            if not len(p):
                print(f"// {name}: no meshes found")
                continue
            radial = np.hypot(p[:, 0], p[:, 2])
            reach = []
            for k in range(SLICES):
                inside = (p[:, 1] >= k * SLICE) & (p[:, 1] < (k + 1) * SLICE)
                reach.append(float(radial[inside].max()) if inside.any() else 0.0)
            values = ", ".join(f"{r:.2f}f" for r in reach)
            print(f'    {{"{group}/{name}", {{{values}}}}},')


if __name__ == "__main__":
    main()
