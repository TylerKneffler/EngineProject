"""Expand the grass scene around its existing cell using a shared prefab."""

from __future__ import annotations

import argparse
from copy import deepcopy
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SCENE = ROOT / "Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene"
PREFAB = ROOT / "Engine/Core/Assets/Prefabs/Terrain/GrassStressCell.prefab"
PREFAB_ASSET = "Assets/Prefabs/Terrain/GrassStressCell.prefab"
NS = "urn:engine-components"
ET.register_namespace("components", NS)


def position(node: ET.Element) -> list[ET.Element]:
    return list(node.find("transform/Transform.position"))


def expand_scene(source: str, radius: int = 10) -> str:
    if radius < 0:
        raise ValueError("radius must be nonnegative")
    root = ET.fromstring(source)
    objects = root.find("objects")
    center = next(o for o in root.iter("item") if o.get("name") == "Flat Grass Terrain")
    old_landscape = next((o for o in objects if o.get("name") == "Grass Terrain Landscape"), None)
    terrain = center.find(f"components/{{{NS}}}Terrain")
    width = int(terrain.get("Terrain.cellsX")) * float(terrain.get("Terrain.cellWidth"))
    depth = int(terrain.get("Terrain.cellsY")) * float(terrain.get("Terrain.cellDepth"))
    origin = [float(v.text) for v in position(center)]
    if old_landscape is not None:
        origin = [a + float(b.text) for a, b in zip(origin, position(old_landscape))]
        objects.remove(old_landscape)
    elif center in list(objects):
        objects.remove(center)
    # The landscape owns the coordinate grid and collision residency policy.
    landscape = ET.SubElement(objects, "item", {"name": "Grass Terrain Landscape", "enabled": "true"})
    landscape.append(deepcopy(center.find("transform")))
    for value, coordinate in zip(position(landscape), origin):
        value.text = f"{coordinate:g}"
    components = ET.SubElement(landscape, "components")
    ET.SubElement(components, f"{{{NS}}}TerrainManager", {
        "TerrainManager.cellsX": terrain.get("Terrain.cellsX"),
        "TerrainManager.cellsY": terrain.get("Terrain.cellsY"),
        "TerrainManager.cellWidth": terrain.get("Terrain.cellWidth"),
        "TerrainManager.cellDepth": terrain.get("Terrain.cellDepth"),
        "TerrainManager.viewerObjectName": "Grass Test Capsule",
        "TerrainManager.generateColliders": "true",
        "TerrainManager.streamCollidersByDistance": "true",
        "TerrainManager.manageAuthoredCellColliders": "true",
        "TerrainManager.colliderActivationDistance": "1",
        "TerrainManager.streamedPrefabPath": PREFAB_ASSET,
        "TerrainManager.reuseStreamedPrefabCells": "true",
        "TerrainManager.streamedPrefabCellName": "Grass Terrain Cell",
        "TerrainManager.streamedPrefabHalfExtent": str(radius),
        "TerrainManager.streamedPrefabRadius": "1",
        "TerrainManager.maxLoadsPerUpdate": "2",
    })
    grid = ET.SubElement(landscape, "children")
    # One coarse surface preserves the full view while detailed grass cells
    # load only around the camera. Keep it below the cell surfaces to avoid
    # overlapping depth values and omit all physics and grass components.
    distant = ET.SubElement(grid, "item", {"name": "Grass Distant Terrain", "enabled": "true"})
    distant.append(deepcopy(center.find("transform")))
    for value, coordinate in zip(position(distant),
                                 (-radius * width, -0.03, -radius * depth)):
        value.text = f"{coordinate:g}"
    distant_components = ET.SubElement(distant, "components")
    for component_name in ("Mesh", "Terrain", "Material"):
        component = center.find(f"components/{{{NS}}}{component_name}")
        if component is not None:
            distant_components.append(deepcopy(component))
    distant_terrain = distant.find(f"components/{{{NS}}}Terrain")
    distant_terrain.set("Terrain.cellsX", "1")
    distant_terrain.set("Terrain.cellsY", "1")
    distant_terrain.set("Terrain.cellWidth", f"{(radius * 2 + 1) * width:g}")
    distant_terrain.set("Terrain.cellDepth", f"{(radius * 2 + 1) * depth:g}")
    distant_terrain.set("Terrain.textureTiling",
                        f"{float(terrain.get('Terrain.textureTiling', '4')) * (radius * 2 + 1):g}")
    for property_name in ("Terrain.heights", "Terrain.weights"):
        property_node = distant_terrain.find(property_name)
        if property_node is not None:
            distant_terrain.remove(property_node)
    for value in position(center):
        value.text = "0"
    # The manager activates the initial player cells before the first physics step.
    collider = center.find(f"components/{{{NS}}}MeshObjectCollider")
    if collider is not None:
        collider.set("MeshObjectCollider.collisionEnabled", "false")
    grid.append(center)
    # Existing saved scenes can contain obsolete generated tile meshes and
    # per-placement preview objects. Retain only the authored object layer.
    for layer in center.findall("children/item"):
        field = layer.find(f"components/{{{NS}}}GrassField")
        if field is not None:
            field.attrib.setdefault("GrassField.fadeDuration", ".35")
            field.attrib.setdefault("GrassField.distanceFadeWidth", "2")
            field.set("GrassField.retainDormantTiles", "true")
            children = layer.find("children")
            if children is not None:
                children.clear()
    for old in list(objects):
        if old.get("prefab") == PREFAB_ASSET:
            objects.remove(old)
    template = deepcopy(center)
    template.tag = "object"
    template.set("name", "Grass Terrain Cell")
    for v in position(template):
        v.text = "0"
    prefab = ET.Element("Prefab", {"version": "1", "type": "prefab"})
    prefab.append(template)
    ET.indent(prefab, space="  ")
    PREFAB.parent.mkdir(parents=True, exist_ok=True)
    PREFAB.write_text(ET.tostring(prefab, encoding="unicode", xml_declaration=True) + "\n", encoding="utf-8")
    # Make more of the enlarged terrain visible from the player camera.
    for camera in root.iter(f"{{{NS}}}Camera"):
        camera.set("Camera.far", f"{max(80, (radius + 1) * max(width, depth) * 2):g}")
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="unicode", xml_declaration=True) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--radius", type=int, default=10)
    args = parser.parse_args()
    SCENE.write_text(expand_scene(SCENE.read_text(encoding="utf-8"), args.radius), encoding="utf-8")
    print(f"Expanded {SCENE.name}: {(2 * args.radius + 1) ** 2} terrain cells; prefab {PREFAB_ASSET}")


if __name__ == "__main__":
    main()
