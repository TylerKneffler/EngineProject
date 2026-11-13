"""Build a self-contained streamed oak map from the current authored meadow."""
from collections import Counter
from copy import deepcopy
from pathlib import Path
import math
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine/Core/Assets"
NS = "urn:engine-components"
ET.register_namespace("components", NS)


def component(name, **fields):
    return ET.Element(f"{{{NS}}}{name}", {f"{name}.{key}": str(value) for key, value in fields.items()})


def transform(x=0, y=0, z=0):
    result = ET.Element("transform")
    for key, value in (("position", f"{x} {y} {z}"), ("rotation", "0 0 0"), ("scale", "1 1 1")):
        ET.SubElement(result, f"Transform.{key}", {"_arrayEncoding": "spaceSeparatedNumbers"}).text = value
    return result


def numbers(element):
    return [float(v) for v in element.text.split()] if element.get("_arrayEncoding") else [float(v.text) for v in element]


def sample(heights, count_x, count_z, step_x, step_z, x, z):
    sx, sz = min(count_x, max(0, x / step_x)), min(count_z, max(0, z / step_z))
    ix, iz = min(int(sx), count_x - 1), min(int(sz), count_z - 1)
    tx, tz = sx - ix, sz - iz
    a, b = heights[iz * (count_x + 1) + ix:iz * (count_x + 1) + ix + 2]
    c, d = heights[(iz + 1) * (count_x + 1) + ix:(iz + 1) * (count_x + 1) + ix + 2]
    return a + (b-a)*tx + (c-a)*tz if tx+tz <= 1 else d-(d-c)*(1-tx)-(d-b)*(1-tz)


def write(root, path):
    ET.indent(root, space="  ")
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def build():
    scene = ET.parse(ASSETS / "Scenes/terrain/oak_meadow.scene").getroot()
    objects = scene.find("objects")
    ground = next(o for o in objects if o.get("name") == "Oak meadow ground")
    tile = deepcopy(ground)
    tile.tag = "object"
    tile.set("name", "Streamed oak meadow cell")
    tile.remove(tile.find("transform"))
    tile.append(transform())
    terrain = tile.find(f"components/{{{NS}}}Terrain")
    nx, nz = int(terrain.get("Terrain.cellsX")), int(terrain.get("Terrain.cellsY"))
    dx, dz = float(terrain.get("Terrain.cellWidth")), float(terrain.get("Terrain.cellDepth"))
    width, depth = nx * dx, nz * dz
    height_node = terrain.find("Terrain.heights")
    old = numbers(height_node)
    assert len(old) == (nx + 1) * (nz + 1)
    # Keep the meadow interior; flatten the outer two vertex rows and blend
    # over 24 m. Identical heights and zero edge slopes make translated tiles meet.
    heights, blends = [], []
    for z in range(nz + 1):
        for x in range(nx + 1):
            edge = min(x * dx, (nx-x) * dx, z * dz, (nz-z) * dz)
            t = min(1, max(0, (edge - max(dx, dz)) / 24))
            blend = t*t*(3-2*t)
            blends.append(blend)
            heights.append(old[z * (nx+1) + x] * blend)
    height_node.clear()
    height_node.set("_arrayEncoding", "spaceSeparatedNumbers")
    height_node.text = " ".join(f"{h:.7g}" for h in heights)
    weight_node = terrain.find("Terrain.weights")
    weights = numbers(weight_node)
    layer_count = len(terrain.find("Terrain.layers"))
    assert len(weights) == len(heights) * layer_count
    edge_weights = weights[:layer_count]
    weights = [w * blends[i // layer_count] + edge_weights[i % layer_count] * (1-blends[i // layer_count])
               for i, w in enumerate(weights)]
    weight_node.clear()
    weight_node.set("_arrayEncoding", "spaceSeparatedNumbers")
    weight_node.text = " ".join(f"{w:.7g}" for w in weights)
    children = tile.find("children")
    grass_reference = next(c for c in children if c.get("prefab", "").endswith("OakMeadowGrass.prefab"))
    children.remove(grass_reference)
    grass_source = ET.parse(ASSETS / "Prefabs/Woodland/OakMeadowGrass.prefab").getroot().find("object")
    layer_source = grass_source.find(f"components/{{{NS}}}TerrainObjectLayer")
    counts = Counter(p.get("asset") for p in layer_source.find("TerrainObjectLayer.placements"))
    grass = ET.SubElement(children, "item", {"name": "Surface-following meadow grass", "enabled": "true"})
    grass.append(transform())
    grass_components = ET.SubElement(grass, "components")
    layer = ET.SubElement(grass_components, f"{{{NS}}}TerrainObjectLayer", dict(layer_source.attrib))
    ET.SubElement(layer, "TerrainObjectLayer.placements")
    field = deepcopy(grass_source.find(f"components/{{{NS}}}GrassField"))
    field.set("GrassField.allowProceduralGeneration", "true")
    for kind in ("short", "tall"):
        mesh = field.get(f"GrassField.{kind}Mesh")
        field.set(f"GrassField.{kind}PatchesPerSquareMeter", f"{counts[mesh] / (width*depth):.8g}")
    grass_components.append(field)
    ET.SubElement(grass, "children")
    trees = tile.find(f".//{{{NS}}}TerrainObjectLayer/TerrainObjectLayer.placements")
    for placement in trees:
        position = placement.find("position")
        x, y, z = numbers(position)
        y += sample(heights, nx, nz, dx, dz, x, z) - sample(old, nx, nz, dx, dz, x, z)
        position.clear()
        position.set("_arrayEncoding", "spaceSeparatedNumbers")
        position.text = f"{x:.8g} {y:.8g} {z:.8g}"
    # Validate height and slope continuity along all repeated cell boundaries.
    assert all(heights[z*(nx+1)] == heights[z*(nx+1)+nx] == 0 for z in range(nz+1))
    assert all(heights[x] == heights[nz*(nx+1)+x] == 0 for x in range(nx+1))
    assert all(math.isfinite(h) for h in heights)
    prefab = ET.Element("Prefab", {"version": "1", "type": "prefab"})
    prefab.append(tile)
    write(prefab, ASSETS / "Prefabs/Terrain/OakMeadowStreamCell.prefab")
    objects.remove(ground)
    manager_object = ET.SubElement(objects, "item", {"name": "Large oak meadow - 1.8 km streamed grid", "enabled": "true"})
    manager_object.append(deepcopy(ground.find("transform")))
    components = ET.SubElement(manager_object, "components")
    components.append(component("TerrainManager", cellsX=nx, cellsY=nz, cellWidth=dx, cellDepth=dz,
        viewerObjectName="Meadow Explorer", generateColliders="true", streamCollidersByDistance="true",
        manageAuthoredCellColliders="true", colliderActivationDistance=8,
        streamedPrefabPath="Assets/Prefabs/Terrain/OakMeadowStreamCell.prefab",
        reuseStreamedPrefabCells="true", streamedPrefabCellName="Oak meadow cell",
        streamedPrefabHalfExtent=2, streamedPrefabRadius=1, maxLoadsPerUpdate=1))
    center = ET.SubElement(ET.SubElement(manager_object, "children"), "item",
        {"prefab": "Assets/Prefabs/Terrain/OakMeadowStreamCell.prefab", "enabled": "true"})
    center.append(transform())
    write(scene, ASSETS / "Scenes/terrain/oak_meadow_expanded.scene")
    print(f"Created {width*5:g} x {depth*5:g} m map, 25 cells, {len(trees)} trees/cell; "
          f"grass density {sum(counts.values()) / (width*depth):.4f}/m2. Source meadow unchanged.")


if __name__ == "__main__":
    build()
