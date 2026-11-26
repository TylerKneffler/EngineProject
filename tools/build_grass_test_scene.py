"""Builds a small grass test scene from one cell of the landscape demo.

Writes
  Engine/Core/Assets/Scenes/terrain/grass_test.scene

The scene holds one standalone terrain made of a block of 128 m landscape
cells (7 x 7 by default, --block; 896 x 896 m) whose heights are read from the
landscape height archive (world.heights), painted with the landscape's meadow
layer, with a procedural meadow GrassField configured like the one
the demo's grass layer carries (its TerrainManager layer settings), the far grass ring, and a
tree belt round the outside. Lighting, atmosphere and
the flying explorer are copied from landscape_demo.scene so the look matches.
It loads in seconds, so grass changes (range fade, ground tint, LODs) can be
checked without streaming the 24 km landscape:

  build/Debug/RelWithDebInfo/GameBackendCapture.exe --backend dx11 \
    --scene Engine/Core/Assets/Scenes/terrain/grass_test.scene \
    --width 1280 --height 720 --warmup 120 --frames 10 --out .local/grass/test

The camera starts 8 m up (--eye) near the -z edge of the centre cell, looking
along +z down the block, so the detailed grass edge (about 36 m) and the far
ring's edge (200 m) both fall inside the view.

Usage: python tools/build_grass_test_scene.py [--cell X Z] [--block W S E N] [--eye M]
Defaults to cell (0, 0), the meadow around the demo's start position;
build()'s own defaults (one cell, eye height, the demo's look target) give the
original one-cell scene, which the foliage/spin/stamp test generators build on.
Re-running replaces the scene. No third-party modules are needed.
"""

import argparse
import copy
import math
import random
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

import scene_static

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
ARCHIVE = ASSETS / "TerrainData" / "Landscape" / "world.heights"
SOURCE_SCENE = ASSETS / "Scenes" / "terrain" / "landscape_demo.scene"
CELL_PREFAB = ASSETS / "Prefabs" / "Terrain" / "GrassStressCell.prefab"
OUTPUT = ASSETS / "Scenes" / "terrain" / "grass_test.scene"
NAMESPACE = "urn:engine-components"
ARCHIVE_MAGIC = 0x31544748524E5254

# Keep in sync with landscape_demo.scene's TerrainManager terrainLayers.
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
# grass_test.scene defaults: cells around the centre one (west, south, east,
# north) so there is ground out past the far ring's reach (~500 m ahead,
# ~400 m to each side), and a raised explorer looking at the ground 200 m
# ahead, down the block. Square, as GPU grass blades need a square terrain
# (GrassField::SyncGpuBlades).
BLOCK = (3, 3, 3, 3)
CAMERA_HEIGHT = 8.0
LOOK_AHEAD = 200.0  # metres along +z to the ground point the camera faces

# The meadow as the landscape demo grows it: MeadowGrass::Apply / ApplyFar /
# ApplyBushes with LandscapeDriver::ConfigureTerrain's overrides (defaults:
# GPU blades, tall grass fields, scanned bushes). Keep in sync with both.
GRASS_RADIUS = 50.0  # landscape_demo.scene: the meadow GrassField's visibleRadius
DETAILED_RANGE = GRASS_RADIUS + 10
COVER_REACH = max(DETAILED_RANGE, 80.0)  # shrubs and flowers (ENGINE_LANDSCAPE_SHRUB_REACH)
ACCENTS = ";".join(f"Assets/Mesh/Grass/{name}.obj" for name in (
    "meadow_daisy", "meadow_buttercup", "meadow_clover", "meadow_plantain",
    "meadow_knapweed", "meadow_dandelion"))
GRASS_FIELD = {
    "shortMesh": "Assets/Mesh/Grass/meadow_tussock.obj",
    "shortMeshLod1": "Assets/Mesh/Grass/meadow_tussock_lod1.obj",
    "shortMeshLod2": "Assets/Mesh/Grass/meadow_tussock_lod2.obj",
    # Tall grass fields: knee- to waist-high grass as one mass over most of
    # the meadow, the short sward thinning under it.
    "tallMesh": "Assets/Mesh/Grass/meadow_tall_grass.obj",
    "tallMeshLod1": "Assets/Mesh/Grass/meadow_tall_grass_lod1.obj",
    "tallMeshLod2": "Assets/Mesh/Grass/meadow_tall_grass_lod2.obj",
    "viewerObjectName": VIEWER,
    "useEditorCamera": "true",
    "visibleRadius": GRASS_RADIUS,
    "minimumRadiusScale": 1,
    "perPlantDetail": "true",
    "fineReachFraction": DETAILED_RANGE / COVER_REACH,
    # The fine and tall grass are GPU blades (mesh grass where they are off).
    "gpuBlades": "true",
    "tileSize": 16,
    "shortPatchesPerSquareMeter": 3.5,
    "tallPatchesPerSquareMeter": .25,
    "tallFieldDensity": 2.2,
    "tallFieldSize": 28,
    "tallFieldCoverage": .85,
    "shortUnderTallShare": .25,
    "seed": 18731,
    "maxTileBuildsPerUpdate": 8,
    "castShadows": "false",
    "tileWorkBudgetMs": 2,
    "fadeDuration": .35,
    "distanceFadeWidth": 2,
    "distanceFadeFraction": .3,
    "distanceThinning": .45,
    "groundTintStrength": .8,
    "groundRootShade": .85,
    "retainDormantTiles": "false",
    "allowProceduralGeneration": "true",
    "proceduralLayerMask": 1 << GRASS_LAYER,
    # Wildflower drifts; shrubs and bushes come from the scanned bush layer.
    "accentMeshes": ACCENTS,
    "accentPatchesPerSquareMeter": .18,
    "accentDriftSize": 7,
}
TUFT = "Assets/Mesh/Grass/far_grass_tuft"
TALL_TUFT = "Assets/Mesh/Grass/far_tall_tuft"
FAR_FIELD = {
    "shortMesh": f"{TUFT}.obj", "shortMeshLod1": f"{TUFT}_lod1.obj",
    "shortMeshLod2": f"{TUFT}_lod2.obj",
    # Tall tufts where the detailed grass grows its tall fields (same seed
    # and field noise, so the fields line up across the hand-off).
    "tallMesh": f"{TALL_TUFT}.obj", "tallMeshLod1": f"{TALL_TUFT}_lod1.obj",
    "tallMeshLod2": f"{TALL_TUFT}_lod2.obj", "viewerObjectName": VIEWER,
    "useEditorCamera": "true", "visibleRadius": 200, "tileSize": 64,
    "shortPatchesPerSquareMeter": 1.1, "tallPatchesPerSquareMeter": 0, "seed": 18731,
    "tallFieldDensity": .9, "tallFieldSize": 28, "tallFieldCoverage": .85,
    "shortUnderTallShare": .3, "perPlantDetail": "true",
    "castShadows": "false", "tileWorkBudgetMs": 1, "fadeDuration": .35,
    "distanceFadeFraction": .25, "distanceThinning": 1,
    # Fades in over the blades' last 10 m (GrassBlades fadeBand).
    "innerRadius": DETAILED_RANGE - 10, "innerFadeWidth": 10,
    "windStrength": .6, "groundTintStrength": 0,
    "allowProceduralGeneration": "true", "proceduralLayerMask": 1 << GRASS_LAYER,
}

def scan_mesh(stem):
    return f"Assets/Mesh/Grass/{stem}.mesh"


# Scanned bushes, shrubs, nettles, saplings and ferns (MeadowGrass::ApplyBushes).
BUSH_FIELD = {
    "shortMesh": f"{TUFT}.obj", "tallMesh": f"{TUFT}.obj", "shortMeshLod1": f"{TUFT}.obj",
    "shortMeshLod2": f"{TUFT}.obj", "tallMeshLod1": f"{TUFT}.obj", "tallMeshLod2": f"{TUFT}.obj",
    "shortPatchesPerSquareMeter": 0, "tallPatchesPerSquareMeter": 0,
    "materialPath": "Assets/Materials/Grass/scan_bush.material",
    "viewerObjectName": VIEWER, "useEditorCamera": "true", "visibleRadius": 80,
    "minimumRadiusScale": 1, "perPlantDetail": "true", "fineReachFraction": .5,
    "tileSize": 16, "seed": 18731, "maxTileBuildsPerUpdate": 8, "castShadows": "false",
    "tileWorkBudgetMs": 1, "fadeDuration": .35, "distanceFadeFraction": .3,
    "retainDormantTiles": "false", "windStrength": .6, "groundTintStrength": 0,
    "allowProceduralGeneration": "true", "proceduralLayerMask": 1 << GRASS_LAYER,
    "shrubMeshes": ";".join(scan_mesh(s) for s in ("scan_shrub", "scan_nettle", "scan_sapling", "scan_fern")),
    "shrubPatchesPerSquareMeter": .5, "shrubThicketSize": 9, "shrubCoverage": .25,
    "bushMeshes": ";".join(scan_mesh(s) for s in ("scan_bush", "scan_bush_tall")),
    "bushesPerSquareMeter": .08, "bushClusterSize": 16, "bushCoverage": .25,
}
OBJECT_LAYER = {
    "layerName": "Procedural meadow grass",
    "viewDistance": COVER_REACH,
    "lod1Distance": 7,
    "lod2Distance": 13,
    "lodHysteresis": .75,
    "shadowDistance": 8,
    "snapToGround": "true",
    "conformToGround": "true",
}


# Tree belt along the cell edges (on by default; foliage_test turns it off for
# its own grove): an instanced layer like the landscape's forest layers.
EDGE_TREE_LAYER = {"layerName": "Edge trees", "viewDistance": 400, "lod1Distance": 28,
                   "lod2Distance": 90, "shadowDistance": 90, "shadowLod": 1}
EDGE_TREE_ROWS = (4.0, 11.0, 18.0)  # metres in from each edge
EDGE_TREE_STEP = 8.0                # along the edge
EDGE_TREE_SEED = 6151
EDGE_OAKS = [f"Assets/Prefabs/Woodland/Oaks/{name}.prefab" for name in (
    "Big_oak_tree_001", "Big_oak_tree_002", "Large_oak_tree_001", "Large_oak_tree_003",
    "Medium_oak_tree_001", "Medium_oak_tree_002", "Medium_oak_tree_003")]
EDGE_CONIFERS = [f"Assets/Prefabs/Woodland/Conifers/{name}.prefab" for name in (
    "Spruce_tall_01", "Spruce_young_01", "Pine_scots_01", "Fir_subalpine_01")]


def height_at(heights, side, spacing, x, z, rows=None):
    """Bilinear terrain height at terrain-local (x, z); `side` samples per row,
    `rows` rows (default square)."""
    fx, fz = x / spacing, z / spacing
    ix, iz = min(max(int(fx), 0), side - 2), min(max(int(fz), 0), (rows or side) - 2)
    tx, tz = fx - ix, fz - iz
    h = lambda i, j: heights[j * side + i]
    return ((h(ix, iz) * (1 - tx) + h(ix + 1, iz) * tx) * (1 - tz) +
            (h(ix, iz + 1) * (1 - tx) + h(ix + 1, iz + 1) * tx) * tz)


def edge_trees(height, width, depth, clear):
    """Placements (x, z, y, item) of 2-3 jittered tree rows along every edge
    of a width x depth area, a mix of oaks and conifers. `height(x, z)` is the
    ground; `clear` is (x0, z0, x1, z1) left open, the gap the explorer starts
    in. Positions are area-local."""
    rng = random.Random(EDGE_TREE_SEED)
    spots = []
    for inset in EDGE_TREE_ROWS:
        count = int((max(width, depth) - 2 * inset) // EDGE_TREE_STEP) + 1
        for i in range(count):
            t = inset + i * EDGE_TREE_STEP
            # -z, +z, -x, +x edges; corners come from the -z/+z rows.
            if t <= width - inset:
                spots += [(t, inset), (t, depth - inset)]
            if inset + EDGE_TREE_STEP <= t <= depth - inset - EDGE_TREE_STEP:
                spots += [(inset, t), (width - inset, t)]
    trees = []
    for x, z in spots:
        if rng.random() > 0.85:
            continue
        x = min(max(x + rng.uniform(-2.5, 2.5), 1.0), width - 1.0)
        z = min(max(z + rng.uniform(-2.5, 2.5), 1.0), depth - 1.0)
        if clear[0] <= x <= clear[2] and clear[1] <= z <= clear[3]:
            continue
        path = rng.choice(EDGE_CONIFERS if rng.random() < 0.4 else EDGE_OAKS)
        scale = rng.uniform(0.85, 1.15)
        y = height(x, z) - 0.15  # sink roots on slopes
        item = ET.Element("item", {"asset": path, "id": "0",  # numbered by the caller
                                   "yawDegrees": f"{rng.uniform(0, 360):.3f}",
                                   "scale": f"{scale:.4f}"})
        trees.append((x, z, y, item))
    return trees


def read_cells(wanted):
    """Heights (metres) of the archive cells in `wanted` ((x, z) pairs), as
    {(x, z): heights} with rows along +z, (cells+1)^2 each, plus cells_x, cells_y."""
    data = ARCHIVE.read_bytes()
    header = struct.Struct("<QIQiiifii")
    magic, version, _, extent, cells_x, cells_y, scale, fine_extent, coarse = \
        header.unpack_from(data, 0)
    if magic != ARCHIVE_MAGIC or version != 3:
        raise SystemExit(f"{ARCHIVE}: not a v3 height archive")
    for cell_x, cell_z in wanted:
        if max(abs(cell_x), abs(cell_z)) > fine_extent:
            raise SystemExit(f"cell ({cell_x}, {cell_z}) is outside the 4 m play area "
                             f"(|x|, |z| <= {fine_extent})")
    found = {}
    offset = header.size
    for z in range(-extent, extent + 1):
        for x in range(-extent, extent + 1):
            fine = max(abs(x), abs(z)) <= fine_extent
            samples = (cells_x + 1) * (cells_y + 1) if fine else \
                (cells_x // coarse + 1) * (cells_y // coarse + 1)
            size = samples * 2 + (samples + 3) // 4  # i16 heights + 2-bit codes
            if (x, z) in wanted:
                values = struct.unpack_from(f"<{samples}h", data, offset)
                found[(x, z)] = [v / scale for v in values]
            offset += size
    if len(found) != len(set(wanted)):
        raise SystemExit("cell not found in archive")
    return found, cells_x, cells_y


def read_cell(cell_x, cell_z):
    """Heights (metres) of one archive cell, rows along +z, (cells+1)^2."""
    found, cells_x, cells_y = read_cells({(cell_x, cell_z)})
    return found[(cell_x, cell_z)], cells_x, cells_y


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


def terrain_item(prefab, name, position, spacing, cells_x, cells_y, heights,
                 thinning, far, placements):
    """One terrain: heights, procedural grass, far ring and tree belt
    (`placements`, terrain-local; None for no belt)."""
    cell = as_item(prefab)
    cell.set("name", name)
    for old in cell.findall("transform"):
        cell.remove(old)
    cell.insert(0, transform(position))
    low, high = min(heights), max(heights)
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
            OBJECT_LAYER, layerName="Procedural far grass", viewDistance=200, lod1Distance=45,
            lod2Distance=90))
        ET.SubElement(ring_layer, "TerrainObjectLayer.placements")
        ring_components.append(ring_layer)
        ring_components.append(component("GrassField", "GrassField", FAR_FIELD))
        ET.SubElement(ring, "children")
    bushes = ET.SubElement(children, "item", {
        "name": "Procedural meadow bushes", "enabled": "true", "layer": str(GRASS_DETAIL_LAYER)})
    bushes.append(transform((0, 0, 0)))
    bush_components = ET.SubElement(bushes, "components")
    bush_layer = component("TerrainObjectLayer", "TerrainObjectLayer", dict(
        OBJECT_LAYER, layerName="Procedural meadow bushes", viewDistance=80, lod1Distance=18,
        lod2Distance=40))
    ET.SubElement(bush_layer, "TerrainObjectLayer.placements")
    bush_components.append(bush_layer)
    bush_components.append(component("GrassField", "GrassField", BUSH_FIELD))
    ET.SubElement(bushes, "children")
    if placements is not None:
        belt = ET.SubElement(children, "item", {"name": "Edge trees", "enabled": "true"})
        belt.append(transform((0, 0, 0)))
        belt_components = ET.SubElement(belt, "components")
        belt_layer = component("TerrainObjectLayer", "TerrainObjectLayer", EDGE_TREE_LAYER)
        ET.SubElement(belt_layer, "TerrainObjectLayer.placements").extend(placements)
        belt_components.append(belt_layer)
        ET.SubElement(belt, "children")
    return cell


def build(cell_x, cell_z, output=OUTPUT, thinning=None, far=True, trees=True,
          block=(0, 0, 0, 0), eye=EYE_HEIGHT, look=None):
    """Writes the scene around cell (cell_x, cell_z). `block` is how many more
    cells to add (west, south, east, north), so the far grass ring has ground
    to cover. The block is merged into one terrain (separate Terrain objects
    are not seam-stitched, so the sky shows through cracks between them) with
    one grass field and far ring, and the tree belt runs round its outside.
    The explorer stands `eye` metres above the ground; with `look` (metres) it
    looks at the ground that far ahead along +z instead of the demo's target."""
    ET.register_namespace("components", NAMESPACE)
    west, south, east, north = block
    origin_x, origin_z = cell_x - west, cell_z - south
    across, along = west + east + 1, south + north + 1
    cells, per_x, per_y = read_cells({(origin_x + i, origin_z + j)
                                      for i in range(across) for j in range(along)})
    spacing = 4.0
    width = per_x * spacing
    # One height grid over the block, rows along +z, origin at its (-x, -z)
    # corner; neighbouring cells share their edge samples.
    cells_x, cells_y = across * per_x, along * per_y
    side, rows = cells_x + 1, cells_y + 1
    heights = [0.0] * (side * rows)
    for (x, z), values in cells.items():
        col, row = (x - origin_x) * per_x, (z - origin_z) * per_y
        for j in range(per_y + 1):
            start = (row + j) * side + col
            heights[start:start + per_x + 1] = values[j * (per_x + 1):(j + 1) * (per_x + 1)]

    def ground_at(x, z):
        return height_at(heights, side, spacing, x, z, rows)

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
            # Near the centre cell's -z edge, looking along +z across it.
            sx, sz = west * width + width * .5, south * width + 6.0  # block-local
            ground = heights[int(round(sz / spacing)) * side + int(round(sx / spacing))]
            for old in explorer.findall("transform"):
                explorer.remove(old)
            position = (origin_x * width + sx, ground + eye, origin_z * width + sz)
            explorer.insert(0, transform(position))
            if look is not None:
                target = (position[0], ground_at(sx, sz + look), position[2] + look)
                for camera in explorer.iter(f"{{{NAMESPACE}}}Camera"):
                    old_target = camera.find("Camera.target")
                    camera.insert(list(camera).index(old_target), numbers("Camera.target", target))
                    camera.remove(old_target)
            objects.append(explorer)

    placements = None
    if trees:
        # The explorer starts at (width / 2, 6) in the centre cell: leave it
        # a gap to stand in and look out across the meadow.
        sx, sz = west * width + width * .5, south * width
        placements = []
        for x, z, y, item in edge_trees(ground_at, across * width, along * width,
                                        (sx - 10.0, sz, sx + 10.0, sz + 26.0)):
            item.set("id", str(len(placements) + 1))
            item.append(numbers("position", (x, y, z)))
            item.append(numbers("normal", (0, 1, 0)))
            placements.append(item)

    prefab = ET.parse(CELL_PREFAB).getroot().find("object")
    single = len(cells) == 1
    name = f"Landscape Cell ({cell_x}, {cell_z})" if single else \
        f"Landscape Cells ({origin_x}, {origin_z}) to ({cell_x + east}, {cell_z + north})"
    objects.append(terrain_item(prefab, name, (origin_x * width, 0, origin_z * width), spacing,
                                cells_x, cells_y, heights, thinning, far, placements))

    # Everything but the explorer is static (tools/scene_static.py).
    scene_static.mark_static(objects)
    ET.indent(scene, "  ")
    span = "" if single else (
        f" and the {len(cells) - 1} cells around it, merged"
        f" ({across * width:g} x {along * width:g} m)")
    header = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--\n"
              f"  Grass test: landscape cell ({cell_x}, {cell_z}){span} with procedural meadow grass.\n"
              "  Generated by tools/build_grass_test_scene.py; re-running replaces this file.\n"
              "-->\n")
    output.write_text(header + ET.tostring(scene, encoding="unicode") + "\n", encoding="utf-8")
    shown = output.relative_to(ROOT) if output.is_relative_to(ROOT) else output
    print(f"{shown}: {len(cells)} cell(s) around ({cell_x}, {cell_z}), heights "
          f"{min(heights):.1f}..{max(heights):.1f} m, {len(placements or [])} edge trees")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cell", nargs=2, type=int, default=(0, 0), metavar=("X", "Z"))
    parser.add_argument("--block", nargs=4, type=int, default=BLOCK,
                        metavar=("WEST", "SOUTH", "EAST", "NORTH"),
                        help="extra cells around the centre cell (0 0 0 0 for one cell)")
    parser.add_argument("--eye", type=float, default=CAMERA_HEIGHT,
                        help="explorer height above the ground, metres")
    parser.add_argument("--thinning", type=float, default=None,
                        help="GrassField.distanceThinning override (1 disables thinning)")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--no-far", action="store_true", help="omit the far grass ring")
    parser.add_argument("--no-trees", action="store_true", help="omit the tree belt along the edges")
    args = parser.parse_args()
    build(*args.cell, output=args.output.resolve(), thinning=args.thinning, far=not args.no_far,
          trees=not args.no_trees, block=tuple(args.block), eye=args.eye, look=LOOK_AHEAD)


if __name__ == "__main__":
    main()
