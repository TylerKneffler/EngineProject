"""Builds a small flicker test scene: one flat cell, trees round the edge, ground cover everywhere.

Writes
  Engine/Core/Assets/Scenes/terrain/spin_test.scene

One standalone 128 m terrain cell (no TerrainManager: nothing streams, no cell
loads, no near or far object blocks rebuild), flat, painted as meadow, with
  - a forest belt along every edge (10 rows by default, ~1,000 oaks and
    conifers, an instanced layer like the landscape's forests; no gap),
  - the meadow grass (tools/build_grass_test_scene.py) without its generated
    bushes, as the landscape runs it (LandscapeDriver, scanned bushes on),
  - "Procedural meadow bushes": the landscape's scanned bush field with its
    exact settings (MeadowGrass::ApplyScanned / ApplyBushes plus LandscapeDriver's
    per-plant detail, full reach and 16 m tiles), on the whole cell, so bushes
    stand between the player and the trees and under them,
  - the explorer at the cell centre, eye height, looking along +z.
Spin in place to look for flicker that is not caused by streaming:

  build/RelWithDebInfo/GameBackendCapture.exe --backend dx12 \
    --scene Engine/Core/Assets/Scenes/terrain/spin_test.scene --width 1920 --height 1080 \
    --warmup 200 --frames 1800 --max-fps 0 --look-yaw 12 --flag-button \
    --capture-every 1 --capture-async --out .local/transitions/spin

Deterministic. Re-running replaces the scene.
Usage: python tools/build_spin_test_scene.py [--ground-height Y] [--tree-rows N]
       [--row-spacing M] [--tree-step M]
"""

import argparse
import xml.etree.ElementTree as ET

import build_grass_test_scene as grass
import scene_static

OUTPUT = grass.ASSETS / "Scenes" / "terrain" / "spin_test.scene"
FOREST_SOIL = 3
BUSH_LAYER = "Procedural meadow bushes"


def scan(stem):
    return f"Assets/Mesh/Grass/{stem}.mesh"


EMPTY = "Assets/Mesh/Grass/far_grass_tuft.obj"
# MeadowGrass::ApplyScanned (reach 80, seed 18731) + ApplyBushes + LandscapeDriver.
BUSH_FIELD = {
    "shortMesh": EMPTY, "tallMesh": EMPTY, "shortMeshLod1": EMPTY, "shortMeshLod2": EMPTY,
    "tallMeshLod1": EMPTY, "tallMeshLod2": EMPTY,
    "shortPatchesPerSquareMeter": 0, "tallPatchesPerSquareMeter": 0,
    "materialPath": "Assets/Materials/Grass/scan_bush.material",
    "viewerObjectName": grass.VIEWER, "useEditorCamera": "true", "visibleRadius": 80,
    "tileSize": 16, "seed": 18731, "maxTileBuildsPerUpdate": 8, "castShadows": "false",
    "tileWorkBudgetMs": 1, "fadeDuration": .35, "distanceFadeFraction": .3,
    "retainDormantTiles": "false", "windStrength": .6, "groundTintStrength": 0,
    "allowProceduralGeneration": "true",
    "shrubMeshes": ";".join(scan(s) for s in ("scan_shrub", "scan_nettle", "scan_sapling", "scan_fern")),
    "shrubPatchesPerSquareMeter": .5, "shrubThicketSize": 9, "shrubCoverage": .25,
    "bushMeshes": ";".join(scan(s) for s in ("scan_bush", "scan_bush_tall")),
    "bushesPerSquareMeter": .08, "bushClusterSize": 16, "bushCoverage": .25,
    # LandscapeDriver overrides.
    "minimumRadiusScale": 1, "perPlantDetail": "true", "fineReachFraction": .5,
    "proceduralLayerMask": (1 << grass.GRASS_LAYER) | (1 << FOREST_SOIL),
}
BUSH_OBJECT_LAYER = {
    "layerName": BUSH_LAYER, "viewDistance": 80, "lod1Distance": 18, "lod2Distance": 40,
    "snapToGround": "true", "conformToGround": "true",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ground-height", type=float, default=0.0)
    parser.add_argument("--tree-rows", type=int, default=10,
                        help="tree rows along each edge (2 gives the original ~100 trees)")
    parser.add_argument("--row-spacing", type=float, default=3.0, help="metres between rows")
    parser.add_argument("--tree-step", type=float, default=3.2,
                        help="metres between trees along a row (8 in the grass test)")
    args = parser.parse_args()

    # Flat ground: the grass test builder reads the cell from the landscape
    # archive; hand it a level plane of the same size instead.
    real_read = grass.read_cell

    def flat_cell(cell_x, cell_z):
        heights, cells_x, cells_y = real_read(cell_x, cell_z)
        return [args.ground_height] * len(heights), cells_x, cells_y
    grass.read_cell = flat_cell
    # Tree rows along every edge, no gap (the player stands in the middle):
    # by default 10 rows 3 m apart, 3.2 m along the row (~1,000 trees, ten
    # times the first version's two rows at 7 m and 8 m).
    grass.EDGE_TREE_ROWS = tuple(4.0 + i * args.row_spacing for i in range(args.tree_rows))
    grass.EDGE_TREE_STEP = args.tree_step
    real_edge_trees = grass.edge_trees
    grass.edge_trees = lambda heights, side, spacing, width, clear: real_edge_trees(
        heights, side, spacing, width, (-1.0, -1.0, -1.0, -1.0))
    # The meadow grass without its generated bushes and shrubs, but for the
    # tall grass clumps at a quarter density (LandscapeDriver, scanned bushes on).
    grass.GRASS_FIELD = dict(grass.GRASS_FIELD, bushMeshes="", bushesPerSquareMeter=0,
                             shrubMeshes="Assets/Mesh/Grass/tall_grass_clump.obj",
                             shrubPatchesPerSquareMeter=grass.GRASS_FIELD["shrubPatchesPerSquareMeter"] / 4)
    grass.build(0, 0, output=OUTPUT, trees=True)

    ET.register_namespace("components", grass.NAMESPACE)
    scene = ET.parse(OUTPUT).getroot()
    objects = scene.find("objects")
    _, cells_x, _ = flat_cell(0, 0)
    width = cells_x * 4.0
    explorer = next(i for i in objects if i.get("name") == grass.VIEWER)
    position = explorer.find("transform").find("Transform.position")
    position.text = f"{width * .5:.6g} {args.ground_height + grass.EYE_HEIGHT:.6g} {width * .5:.6g}"
    cell = next(i for i in objects if (i.get("name") or "").startswith("Landscape Cell"))
    bushes = ET.SubElement(cell.find("children"), "item", {
        "name": BUSH_LAYER, "enabled": "true", "layer": str(grass.GRASS_DETAIL_LAYER)})
    bushes.append(grass.transform((0, 0, 0)))
    components = ET.SubElement(bushes, "components")
    layer = grass.component("TerrainObjectLayer", "TerrainObjectLayer", BUSH_OBJECT_LAYER)
    ET.SubElement(layer, "TerrainObjectLayer.placements")
    components.append(layer)
    components.append(grass.component("GrassField", "GrassField", BUSH_FIELD))
    ET.SubElement(bushes, "children")

    scene_static.mark_static(objects)
    ET.indent(scene, "  ")
    header = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--\n"
              "  Spin test: one flat cell, two tree rows round the edge, meadow grass and the\n"
              "  landscape's scanned bushes everywhere, the explorer in the middle.\n"
              "  Generated by tools/build_spin_test_scene.py; re-running replaces this file.\n"
              "-->\n")
    OUTPUT.write_text(header + ET.tostring(scene, encoding="unicode") + "\n", encoding="utf-8")
    print(f"{OUTPUT.relative_to(grass.ROOT)}: flat {width:.0f} m cell, explorer at the centre")


if __name__ == "__main__":
    main()
