"""Derive one-cell grass fixtures from the authored 441-cell stress scene."""

from copy import deepcopy
from pathlib import Path
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SCENES = ROOT / "Engine/Core/Assets/Scenes/terrain"
SOURCE = SCENES / "grass_flat_test.scene"
NS = "urn:engine-components"
ET.register_namespace("components", NS)


def build(source: ET.Element, include_distant: bool) -> ET.Element:
    root = deepcopy(source)
    objects = root.find("objects")
    landscape = next((item for item in objects
                      if item.get("name") == "Grass Terrain Landscape"), None)
    if landscape is None:
        raise ValueError("missing grass landscape")
    components = landscape.find("components")
    manager = components.find(f"{{{NS}}}TerrainManager")
    if manager is None:
        raise ValueError("missing streamed terrain manager")
    components.remove(manager)
    landscape.set("name", "Grass Single Cell Landscape")
    children = landscape.find("children")
    center = [item for item in children if item.get("name") == "Flat Grass Terrain"]
    distant = [item for item in children if item.get("name") == "Grass Distant Terrain"]
    if len(center) != 1 or len(distant) != 1:
        raise ValueError("expected one detailed cell and one distant ground surface")
    if not include_distant:
        children.remove(distant[0])
    if len(root.findall(f".//{{{NS}}}GrassField")) != 1:
        raise ValueError("expected exactly one grass field")
    expected_terrain = 2 if include_distant else 1
    if len(root.findall(f".//{{{NS}}}Terrain")) != expected_terrain:
        raise ValueError("unexpected terrain component count")
    ET.indent(root, space="  ")
    return root


def main():
    source = ET.parse(SOURCE).getroot()
    for include_distant, name in ((False, "grass_single_cell_test.scene"),
                                  (True, "grass_single_cell_distant_test.scene")):
        target = SCENES / name
        root = build(source, include_distant)
        target.write_text(ET.tostring(root, encoding="unicode", xml_declaration=True) + "\n",
                          encoding="utf-8")
        print(target.relative_to(ROOT))


if __name__ == "__main__":
    main()
