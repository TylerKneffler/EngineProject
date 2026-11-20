"""Builds a small foliage test scene: the grass test cell plus a hand-placed grove.

Writes
  Engine/Core/Assets/Scenes/terrain/foliage_test.scene

Starts from tools/build_grass_test_scene.py (one 128 m landscape cell, procedural
meadow grass, the demo's sun and atmosphere), then adds a grove of oaks with a
conifer edge and bushes, and saved viewpoint cameras for .md/todo.md's foliage
fixed capture poses:

  ForestFloor  under the oaks, looking across the floor (dappled light)
  Backlit      outside the grove, looking up through the crowns toward the sun
  Meadow       the grass test pose, the grove in the background

  build/RelWithDebInfo/GameBackendCapture.exe --backend dx12 \
    --scene Engine/Core/Assets/Scenes/terrain/foliage_test.scene \
    --width 1920 --height 1080 --warmup 300 --frames 120 --max-fps 0 \
    --viewpoint Backlit --out .local/foliage/backlit

Placement is deterministic (fixed seed). Re-running replaces the scene.
Usage: python tools/build_foliage_test_scene.py [--standalone]
"""

import argparse
import math
import random
import xml.etree.ElementTree as ET
from pathlib import Path

import build_grass_test_scene as grass

OUTPUT = grass.ASSETS / "Scenes" / "terrain" / "foliage_test.scene"
CELL = (0, 0)
SPACING = 4.0
SEED = 4127
EYE = 1.7

OAKS = [f"Assets/Prefabs/Woodland/Oaks/{name}.prefab" for name in (
    "Big_oak_tree_001", "Big_oak_tree_002", "Big_oak_tree_003", "Big_oak_tree_004",
    "Large_oak_tree_001", "Large_oak_tree_002", "Large_oak_tree_003", "Large_oak_tree_004",
    "Medium_oak_tree_001", "Medium_oak_tree_002", "Medium_oak_tree_003")]
SMALL_OAKS = [f"Assets/Prefabs/Woodland/Oaks/{name}.prefab" for name in (
    "Small_oak_tree_001", "Small_oak_tree_002", "Small_oak_tree_003",
    "Sapling_oak_tree_001", "Sapling_oak_tree_002", "Sapling_oak_tree_003")]
CONIFERS = [f"Assets/Prefabs/Woodland/Conifers/{name}.prefab" for name in (
    "Spruce_tall_01", "Spruce_young_01", "Pine_scots_01", "Fir_subalpine_01")]
BUSHES = [f"Assets/Prefabs/Woodland/Bushes/{name}.prefab" for name in (
    "Medium_bush_1", "Medium_bush_2", "Medium_bush_3", "Small_bush_1", "Small_bush_2",
    "tall_bush_1", "tall_bush_2")]

# Grove footprint (cell-local metres). The sun shines from -x/+z at ~40 deg,
# so Backlit stands on the +x/-z side looking toward it.
GROVE = (28.0, 54.0, 104.0, 122.0)  # x0, z0, x1, z1
CONIFER_EDGE_X = 96.0               # conifers east of this

# (name, eye xz, target xz, eye height above ground, target height above eye-ground)
VIEWPOINTS = [
    ("ForestFloor", (58.0, 74.0), (84.0, 104.0), 1.6, -0.6),
    ("Backlit", (108.0, 46.0), (52.0, 108.0), 1.7, 26.0),
    ("Meadow", (64.0, 6.0), (64.0, 120.0), 1.7, 0.3),
]


def height_at(heights, side, x, z):
    """Bilinear terrain height at cell-local (x, z)."""
    fx, fz = x / SPACING, z / SPACING
    ix, iz = min(int(fx), side - 2), min(int(fz), side - 2)
    tx, tz = fx - ix, fz - iz
    h = lambda i, j: heights[j * side + i]
    return ((h(ix, iz) * (1 - tx) + h(ix + 1, iz) * tx) * (1 - tz) +
            (h(ix, iz + 1) * (1 - tx) + h(ix + 1, iz + 1) * tx) * tz)


def placement_item(index, path, position, yaw, scale):
    item = ET.Element("item", {"asset": path, "id": str(index + 1),
                               "yawDegrees": f"{math.degrees(yaw):.3f}", "scale": f"{scale:.4f}"})
    item.append(grass.numbers("position", position))
    item.append(grass.numbers("normal", (0, 1, 0)))
    return item


def prefab_item(path, position, yaw, scale):
    item = ET.Element("item", {"prefab": path, "enabled": "true"})
    t = ET.SubElement(item, "transform")
    t.append(grass.numbers("Transform.position", position))
    t.append(grass.numbers("Transform.rotation", (0, yaw, 0)))
    t.append(grass.numbers("Transform.scale", (scale, scale, scale)))
    return item


def viewpoint(name, eye, target):
    item = ET.Element("item", {"name": name, "enabled": "true"})
    components = ET.SubElement(item, "components")
    camera = grass.component("Camera", "Camera", {
        "active": "false", "useTransformRotation": "false", "fov": 67, "near": 0.06,
        "far": 4000, "orthographic": "false", "orthographicSize": 5})
    camera.append(grass.numbers("Camera.up", (0, 1, 0)))
    camera.append(grass.numbers("Camera.target", target))
    components.append(camera)
    ET.SubElement(item, "children")
    item.append(grass.transform(eye))
    return item


def grove(heights, side, origin, make):
    """Jittered-grid trees: oaks, a conifer edge, understory saplings and bushes.
    make(index, prefab, position, yaw, scale) builds each scene element."""
    rng = random.Random(SEED)
    x0, z0, x1, z1 = GROVE
    items, step = [], 9.0
    z = z0
    while z <= z1:
        x = x0
        while x <= x1:
            px, pz = x + rng.uniform(-3.2, 3.2), z + rng.uniform(-3.2, 3.2)
            if rng.random() < 0.88:
                pool = CONIFERS if px > CONIFER_EDGE_X else OAKS
                scale = rng.uniform(0.85, 1.15)
                items.append((rng.choice(pool), px, pz, scale))
            x += step
        z += step
    # Understory: saplings and bushes in the gaps.
    for _ in range(26):
        px, pz = rng.uniform(x0, x1), rng.uniform(z0, z1)
        pool = SMALL_OAKS if rng.random() < 0.35 else BUSHES
        items.append((rng.choice(pool), px, pz, rng.uniform(0.8, 1.2)))
    # Keep the viewpoints' first few metres clear.
    clear = [(e[0], e[1]) for _, e, _, _, _ in VIEWPOINTS]
    out = []
    for path, px, pz, scale in items:
        if any(math.hypot(px - cx, pz - cz) < 3.5 for cx, cz in clear):
            continue
        y = height_at(heights, side, px, pz) - 0.15  # sink roots on slopes
        out.append(make(len(out), path, (origin[0] + px, y, origin[1] + pz),
                        rng.uniform(0, 2 * math.pi), scale))
    return out


# Instanced layer like the landscape's forest layers (OakMeadowStreamCell).
GROVE_LAYER = {"layerName": "Grove", "viewDistance": 400, "lod1Distance": 28,
               "lod2Distance": 90, "shadowDistance": 90, "shadowLod": 1}


FOREST_SOIL = 3
FOREST_FIELD = {
    # Scripts/Grass/MeadowGrass.cpp ApplyForestFloor, with LandscapeDriver's
    # radius and forest-soil mask.
    **{k: f"Assets/Mesh/Grass/{stem}{suffix}.obj" for k, stem, suffix in (
        ("shortMesh", "forest_litter", ""), ("shortMeshLod1", "forest_litter", "_lod1"),
        ("shortMeshLod2", "forest_litter", "_lod2"), ("tallMesh", "forest_herb", ""),
        ("tallMeshLod1", "forest_herb", "_lod1"), ("tallMeshLod2", "forest_herb", "_lod2"))},
    "viewerObjectName": grass.VIEWER, "useEditorCamera": "true", "visibleRadius": grass.GRASS_RADIUS,
    "tileSize": 4, "shortPatchesPerSquareMeter": 2.2, "tallPatchesPerSquareMeter": 1.0, "seed": 40507,
    "maxTileBuildsPerUpdate": 8, "castShadows": "false", "tileWorkBudgetMs": 2, "fadeDuration": .35,
    "distanceFadeFraction": .3, "distanceThinning": .5, "retainDormantTiles": "false",
    "allowProceduralGeneration": "true", "groundTintStrength": 0,
    "proceduralLayerMask": 1 << FOREST_SOIL,
    "accentMeshes": ";".join(f"Assets/Mesh/Grass/{n}.obj" for n in ("forest_seedling", "dry_grass_clump",
                                                                     "meadow_plantain", "forest_branch",
                                                                     "ground_ivy")),
    "accentPatchesPerSquareMeter": .2, "accentDriftSize": 9,
    "shrubMeshes": "Assets/Mesh/Grass/forest_fern.obj;Assets/Mesh/Grass/forest_herb.obj",
    "shrubPatchesPerSquareMeter": 1.0, "shrubThicketSize": 10, "shrubCoverage": .45,
    "bushMeshes": "Assets/Mesh/Grass/meadow_bush.obj;Assets/Mesh/Grass/meadow_bush_tall.obj",
    "bushesPerSquareMeter": .03, "bushClusterSize": 20, "bushCoverage": .2,
}


def paint_forest_floor(objects, side):
    """Forest soil under the grove (soft 8 m edge) and the forest floor field
    (litter, herbs, ferns) on it, as on the landscape's forest paint."""
    cell = next(i for i in objects if (i.get("name") or "").startswith("Landscape Cell"))
    terrain = cell.find("components").find(f"{{{grass.NAMESPACE}}}Terrain")
    weights_element = terrain.find("Terrain.weights")
    weights = [float(v) for v in weights_element.text.split()]
    layers = len(grass.LAYERS)
    x0, z0, x1, z1 = GROVE
    for j in range(side):
        for i in range(side):
            x, z = i * SPACING, j * SPACING
            outside = max(x0 - x, x - x1, z0 - z, z - z1, 0.0)
            forest = max(0.0, min(1.0, 1.0 - outside / 8.0))
            forest = forest * forest * (3 - 2 * forest)
            base = (j * side + i) * layers
            weights[base + grass.GRASS_LAYER] = 1.0 - forest
            weights[base + FOREST_SOIL] = forest
    weights_element.text = " ".join(f"{v:.6g}" for v in weights)
    item = ET.SubElement(cell.find("children"), "item", {
        "name": "Procedural forest floor", "enabled": "true", "layer": str(grass.GRASS_DETAIL_LAYER)})
    item.append(grass.transform((0, 0, 0)))
    components = ET.SubElement(item, "components")
    layer = grass.component("TerrainObjectLayer", "TerrainObjectLayer", dict(
        grass.OBJECT_LAYER, layerName="Procedural forest floor"))
    ET.SubElement(layer, "TerrainObjectLayer.placements")
    components.append(layer)
    components.append(grass.component("GrassField", "GrassField", FOREST_FIELD))
    ET.SubElement(item, "children")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--standalone", action="store_true",
                        help="place the grove as standalone prefab objects instead of an "
                             "instanced TerrainObjectLayer (slow shadow-submission path)")
    args = parser.parse_args()
    grass.build(*CELL, output=OUTPUT)
    heights, cells_x, _ = grass.read_cell(*CELL)
    side, width = cells_x + 1, cells_x * SPACING
    origin = (CELL[0] * width, CELL[1] * width)

    ET.register_namespace("components", grass.NAMESPACE)
    scene = ET.parse(OUTPUT).getroot()
    objects = scene.find("objects")
    if args.standalone:
        trees = grove(heights, side, origin, lambda i, *a: prefab_item(*a))
        group = ET.SubElement(objects, "item", {"name": "Grove", "enabled": "true"})
        ET.SubElement(group, "components")
        ET.SubElement(group, "children").extend(trees)
        group.append(grass.transform((0, 0, 0)))
    else:
        # Placements are cell-local; the cell sits at the origin offset.
        trees = grove(heights, side, (0.0, 0.0), placement_item)
        cell = next(i for i in objects if (i.get("name") or "").startswith("Landscape Cell"))
        layer_item = ET.SubElement(cell.find("children"), "item", {"name": "Grove", "enabled": "true"})
        layer_item.append(grass.transform((0, 0, 0)))
        components = ET.SubElement(layer_item, "components")
        layer = grass.component("TerrainObjectLayer", "TerrainObjectLayer", GROVE_LAYER)
        ET.SubElement(layer, "TerrainObjectLayer.placements").extend(trees)
        components.append(layer)
        ET.SubElement(layer_item, "children")
    paint_forest_floor(objects, side)
    for name, eye, target, eye_h, target_h in VIEWPOINTS:
        ground = height_at(heights, side, *eye)
        objects.append(viewpoint(
            name, (origin[0] + eye[0], ground + eye_h, origin[1] + eye[1]),
            (origin[0] + target[0], ground + target_h, origin[1] + target[1])))

    ET.indent(scene, "  ")
    header = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--\n"
              f"  Foliage test: landscape cell {CELL} with meadow grass and a grove.\n"
              "  Generated by tools/build_foliage_test_scene.py; re-running replaces this file.\n"
              "-->\n")
    OUTPUT.write_text(header + ET.tostring(scene, encoding="unicode") + "\n", encoding="utf-8")
    print(f"{OUTPUT.relative_to(grass.ROOT)}: {len(trees)} grove objects, "
          f"viewpoints {', '.join(v[0] for v in VIEWPOINTS)}")


if __name__ == "__main__":
    main()
