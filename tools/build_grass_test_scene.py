"""Builds a small grass test scene from one cell of the landscape demo.

Writes
  Engine/Core/Assets/Scenes/terrain/grass_test.scene

The scene holds a single standalone 128 m terrain cell whose heights are read
from the landscape height archive (world.heights), painted with the landscape's
meadow layer, and a procedural meadow GrassField configured like the one
LandscapeDriver attaches to the demo's grass layer. Lighting, atmosphere and
the flying explorer are copied from landscape_demo.scene so the look matches.
It loads in seconds, so grass changes (range fade, ground tint, LODs) can be
checked without streaming the 24 km landscape:

  build/Debug/RelWithDebInfo/GameBackendCapture.exe --backend dx11 \
    --scene Engine/Core/Assets/Scenes/terrain/grass_test.scene \
    --width 1280 --height 720 --warmup 120 --frames 10 --out .local/grass/test

The camera starts low near one edge of the cell looking across it, so the
grass range edge (about 36 m) falls inside the view.

Usage: python tools/build_grass_test_scene.py [--cell X Z]
Defaults to cell (0, 0), the meadow around the demo's start position.
Re-running replaces the scene. No third-party modules are needed.
"""

import argparse
import copy
import math
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
ARCHIVE = ASSETS / "TerrainData" / "Landscape" / "world.heights"
SOURCE_SCENE = ASSETS / "Scenes" / "terrain" / "landscape_demo.scene"
CELL_PREFAB = ASSETS / "Prefabs" / "Terrain" / "GrassStressCell.prefab"
OUTPUT = ASSETS / "Scenes" / "terrain" / "grass_test.scene"
NAMESPACE = "urn:engine-components"
ARCHIVE_MAGIC = 0x31544748524E5254

# Keep in sync with LandscapeDriver.cpp (layer order and materials).
LAYERS = [
    ("Assets/Materials/Terrain/PBR/sand.material", (.62, .55, .42)),
    ("Assets/Materials/Terrain/PBR/gravel.material", (.45, .43, .40)),
    ("Assets/Materials/Terrain/PBR/grass.material", (.13, .18, .07)),
    ("Assets/Materials/Terrain/PBR/forest_soil.material", (.22, .15, .10)),
    ("Assets/Materials/Terrain/PBR/rock.material", (.40, .38, .34)),
    ("Assets/Materials/Terrain/PBR/snow.material", (.85, .88, .92)),
]
GRASS_LAYER = 2
GRASS_DETAIL_LAYER = 3  # "Grass" in the default project layers (ProjectLayers.h)
VIEWER = "Landscape Explorer"
EYE_HEIGHT = 1.7

# GrassField settings: MeadowGrass::Apply defaults for a procedural layer,
# with LandscapeDriver's overrides (grassRadius 26).
GRASS_RADIUS = 26.0
ACCENTS = ";".join(f"Assets/Mesh/Grass/{name}.obj" for name in (
    "meadow_daisy", "meadow_buttercup", "meadow_clover", "meadow_plantain",
    "meadow_knapweed", "meadow_dandelion"))
SHRUBS = ";".join(f"Assets/Mesh/Grass/{name}.obj" for name in (
    "meadow_shrub", "meadow_bramble", "meadow_fern", "tall_grass_clump"))
BUSHES = ";".join(f"Assets/Mesh/Grass/{name}.obj" for name in ("meadow_bush", "meadow_bush_tall"))
GRASS_FIELD = {
    "shortMesh": "Assets/Mesh/Grass/meadow_tussock.obj",
    "tallMesh": "Assets/Mesh/Grass/field_tussock.obj",
    "shortMeshLod1": "Assets/Mesh/Grass/meadow_tussock_lod1.obj",
    "shortMeshLod2": "Assets/Mesh/Grass/meadow_tussock_lod2.obj",
    "tallMeshLod1": "Assets/Mesh/Grass/field_tussock_lod1.obj",
    "tallMeshLod2": "Assets/Mesh/Grass/field_tussock_lod2.obj",
    "viewerObjectName": VIEWER,
    "useEditorCamera": "true",
    "visibleRadius": GRASS_RADIUS,
    "tileSize": 4,
    "shortPatchesPerSquareMeter": 3.5,
    "tallPatchesPerSquareMeter": .4,
    "seed": 18731,
    "maxTileBuildsPerUpdate": 8,
    "castShadows": "false",
    "tileWorkBudgetMs": 2,
    "fadeDuration": .35,
    "distanceFadeWidth": 2,
    "distanceFadeFraction": .3,
    "distanceThinning": .45,
    "groundTintStrength": .8,
    "groundRootShade": .7,
    "retainDormantTiles": "false",
    "allowProceduralGeneration": "true",
    "proceduralLayerMask": 1 << GRASS_LAYER,
    "accentMeshes": ACCENTS,
    "accentPatchesPerSquareMeter": .18,
    "accentDriftSize": 7,
    "shrubMeshes": SHRUBS,
    "shrubPatchesPerSquareMeter": .7,
    "shrubThicketSize": 11,
    "shrubCoverage": .22,
    "bushMeshes": BUSHES,
    "bushesPerSquareMeter": .12,
    "bushClusterSize": 16,
    "bushCoverage": .35,
}
DETAILED_RANGE = GRASS_RADIUS + 10
TUFT = "Assets/Mesh/Grass/far_grass_tuft.obj"
FAR_FIELD = {
    "shortMesh": TUFT, "tallMesh": TUFT, "shortMeshLod1": TUFT, "shortMeshLod2": TUFT,
    "tallMeshLod1": TUFT, "tallMeshLod2": TUFT, "viewerObjectName": VIEWER,
    "useEditorCamera": "true", "visibleRadius": 200, "tileSize": 64,
    "shortPatchesPerSquareMeter": .9, "tallPatchesPerSquareMeter": 0, "seed": 29311,
    "castShadows": "false", "tileWorkBudgetMs": 1, "fadeDuration": .35,
    "distanceFadeFraction": .25, "distanceThinning": 1, "innerRadius": DETAILED_RANGE * .5,
    "innerFadeWidth": DETAILED_RANGE * .2, "windStrength": .6, "groundTintStrength": 0,
    "allowProceduralGeneration": "true", "proceduralLayerMask": 1 << GRASS_LAYER,
}
OBJECT_LAYER = {
    "layerName": "Procedural meadow grass",
    "viewDistance": GRASS_RADIUS + 10,
    "lod1Distance": 7,
    "lod2Distance": 13,
    "lodHysteresis": .75,
    "shadowDistance": 8,
    "snapToGround": "true",
    "conformToGround": "true",
}


def read_cell(cell_x, cell_z):
    """Heights (metres) of one archive cell, rows along +z, (cells+1)^2."""
    data = ARCHIVE.read_bytes()
    header = struct.Struct("<QIQiiifii")
    magic, version, _, extent, cells_x, cells_y, scale, fine_extent, coarse = \
        header.unpack_from(data, 0)
    if magic != ARCHIVE_MAGIC or version != 3:
        raise SystemExit(f"{ARCHIVE}: not a v3 height archive")
    if max(abs(cell_x), abs(cell_z)) > fine_extent:
        raise SystemExit(f"cell ({cell_x}, {cell_z}) is outside the 4 m play area "
                         f"(|x|, |z| <= {fine_extent})")
    offset = header.size
    for z in range(-extent, extent + 1):
        for x in range(-extent, extent + 1):
            fine = max(abs(x), abs(z)) <= fine_extent
            samples = (cells_x + 1) * (cells_y + 1) if fine else \
                (cells_x // coarse + 1) * (cells_y // coarse + 1)
            size = samples * 2 + (samples + 3) // 4  # i16 heights + 2-bit codes
            if (x, z) == (cell_x, cell_z):
                values = struct.unpack_from(f"<{samples}h", data, offset)
                return [v / scale for v in values], cells_x, cells_y
            offset += size
    raise SystemExit("cell not found in archive")


def numbers(tag, values):
    element = ET.Element(tag, {"_arrayEncoding": "spaceSeparatedNumbers"})
    element.text = " ".join(f"{v:.6g}" for v in values)
    return element


def transform(position, rotation=(0, 0, 0)):
    element = ET.Element("transform")
    element.append(numbers("Transform.position", position))
    element.append(numbers("Transform.rotation", rotation))
    element.append(numbers("Transform.scale", (1, 1, 1)))
    return element


def component(name, prefix, attributes):
    return ET.Element(f"{{{NAMESPACE}}}{name}",
                      {f"{prefix}.{k}": str(v) for k, v in attributes.items()})


def as_item(element):
    """A prefab <object> as a scene <item>."""
    item = copy.deepcopy(element)
    item.tag = "item"
    return item


def build(cell_x, cell_z, output=OUTPUT, thinning=None, far=True):
    ET.register_namespace("components", NAMESPACE)
    heights, cells_x, cells_y = read_cell(cell_x, cell_z)
    spacing = 4.0
    width = cells_x * spacing
    side = cells_x + 1

    source = ET.parse(SOURCE_SCENE).getroot()
    scene = ET.Element("Scene", {"version": source.get("version", "1")})
    scene.append(copy.deepcopy(source.find("settings")))
    objects = ET.SubElement(scene, "objects")
    for item in source.find("objects"):
        name = item.get("name")
        if name in ("Late afternoon sun", "Atmosphere"):
            copied = copy.deepcopy(item)
            # Test scenes are for A/B captures at fixed poses: no drifting
            # cloud shadows (the landscape sun's cookie) over them.
            for light in copied.iter(f"{{{NAMESPACE}}}Light"):
                for key in ("Light.cookieTexture", "Light.cookieScrollX", "Light.cookieScrollY"):
                    if key in light.attrib:
                        light.set(key, "" if key.endswith("Texture") else "0")
            objects.append(copied)
        elif name == VIEWER:
            explorer = copy.deepcopy(item)
            # Low near the cell's -z edge, looking along +z across the cell.
            sx, sz = width * .5, 6.0
            ground = heights[int(round(sz / spacing)) * side + int(round(sx / spacing))]
            for old in explorer.findall("transform"):
                explorer.remove(old)
            explorer.insert(0, transform((cell_x * width + sx, ground + EYE_HEIGHT,
                                          cell_z * width + sz)))
            objects.append(explorer)

    prefab = ET.parse(CELL_PREFAB).getroot().find("object")
    cell = as_item(prefab)
    cell.set("name", f"Landscape Cell ({cell_x}, {cell_z})")
    for old in cell.findall("transform"):
        cell.remove(old)
    cell.insert(0, transform((cell_x * width, 0, cell_z * width)))
    components = cell.find("components")
    old = components.find(f"{{{NAMESPACE}}}Terrain")
    terrain = component("Terrain", "Terrain", {
        "cellsX": cells_x, "cellsY": cells_y, "cellWidth": spacing,
        "cellDepth": spacing, "textureTiling": 4, "bakedAssetPath": "",
        "reliefMapping": "true", "reliefShadows": "false"})
    terrain.append(numbers("Terrain.heights", heights))
    weights = []
    for _ in heights:
        weights.extend(1.0 if layer == GRASS_LAYER else 0.0 for layer in range(len(LAYERS)))
    terrain.append(numbers("Terrain.weights", weights))
    layers = ET.SubElement(terrain, "Terrain.layers")
    low, high = min(heights), max(heights)
    for material, color in LAYERS:
        layer = ET.SubElement(layers, "item", {
            "material": material, "minHeight": f"{low:.3f}", "maxHeight": f"{high:.3f}"})
        layer.append(numbers("color", color))
    components.insert(list(components).index(old), terrain)
    components.remove(old)

    children = cell.find("children")
    for child in list(children):
        children.remove(child)
    grass = ET.SubElement(children, "item", {
        "name": "Procedural meadow grass", "enabled": "true",
        "layer": str(GRASS_DETAIL_LAYER)})
    grass.append(transform((0, 0, 0)))
    grass_components = ET.SubElement(grass, "components")
    object_layer = component("TerrainObjectLayer", "TerrainObjectLayer", OBJECT_LAYER)
    ET.SubElement(object_layer, "TerrainObjectLayer.placements")
    grass_components.append(object_layer)
    field = dict(GRASS_FIELD)
    if thinning is not None:
        field["distanceThinning"] = thinning
    grass_components.append(component("GrassField", "GrassField", field))
    ET.SubElement(grass, "children")
    if far:
        # Far ring (MeadowGrass::ApplyFar): tufts on 16 m tiles beyond the
        # detailed grass, fading in where it fades out.
        ring = ET.SubElement(children, "item", {
            "name": "Procedural far grass", "enabled": "true", "layer": str(GRASS_DETAIL_LAYER)})
        ring.append(transform((0, 0, 0)))
        ring_components = ET.SubElement(ring, "components")
        ring_layer = component("TerrainObjectLayer", "TerrainObjectLayer", dict(
            OBJECT_LAYER, layerName="Procedural far grass", viewDistance=200, lod1Distance=200,
            lod2Distance=200))
        ET.SubElement(ring_layer, "TerrainObjectLayer.placements")
        ring_components.append(ring_layer)
        ring_components.append(component("GrassField", "GrassField", FAR_FIELD))
        ET.SubElement(ring, "children")
    objects.append(cell)

    ET.indent(scene, "  ")
    header = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--\n"
              f"  Grass test: landscape cell ({cell_x}, {cell_z}) with procedural meadow grass.\n"
              "  Generated by tools/build_grass_test_scene.py; re-running replaces this file.\n"
              "-->\n")
    output.write_text(header + ET.tostring(scene, encoding="unicode") + "\n", encoding="utf-8")
    print(f"{output.relative_to(ROOT)}: cell ({cell_x}, {cell_z}), heights "
          f"{low:.1f}..{high:.1f} m, {len(heights)} samples")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cell", nargs=2, type=int, default=(0, 0), metavar=("X", "Z"))
    parser.add_argument("--thinning", type=float, default=None,
                        help="GrassField.distanceThinning override (1 disables thinning)")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--no-far", action="store_true", help="omit the far grass ring")
    args = parser.parse_args()
    build(*args.cell, output=args.output.resolve(), thinning=args.thinning, far=not args.no_far)


if __name__ == "__main__":
    main()
