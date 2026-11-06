"""Create an ignored forest camera-rail benchmark without changing authored assets."""
import argparse
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
NS = "urn:engine-components"
ET.register_namespace("components", NS)


def transform(node, position):
    group = ET.SubElement(node, "transform")
    for field, value in (("position", position), ("rotation", (0, 0, 0)), ("scale", (1, 1, 1))):
        ET.SubElement(group, "Transform." + field, {"_arrayEncoding": "spaceSeparatedNumbers"}).text = " ".join(map(str, value))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".local/forest-benchmark-route.scene")
    args = parser.parse_args()
    tree = ET.parse(ROOT / "Engine/Core/Assets/Scenes/terrain/oak_meadow.scene")
    root = tree.getroot()
    viewer = next(node for node in root.iter() if node.get("name") == "Meadow Explorer")
    components = viewer.find("components")
    for component in list(components):
        if component.tag.rsplit("}", 1)[-1] in ("FirstPersonController", "RigidBody", "PrimitiveObjectCollider"):
            components.remove(component)
    camera = next(component for component in components if component.tag.endswith("}Camera"))
    camera.set("Camera.useTransformRotation", "true")
    ET.SubElement(components, "{" + NS + "}CameraTrack", {
        "CameraTrack.pathObjectName": "Forest benchmark rail", "CameraTrack.durationSeconds": "18",
        "CameraTrack.playOnStart": "true", "CameraTrack.constantSpeed": "true", "CameraTrack.loop": "true",
        "CameraTrack.continuousLoop": "true", "CameraTrack.easing": "0", "CameraTrack.orientation": "1"})
    path = ET.SubElement(root.find("objects"), "item", {"name": "Forest benchmark rail", "enabled": "true"})
    transform(path, (0, 0, 0))
    path_components = ET.SubElement(path, "components")
    ET.SubElement(path_components, "{" + NS + "}SplinePath", {
        "SplinePath.interpolation": "1", "SplinePath.closed": "true", "SplinePath.arcLengthSamplesPerSegment": "32"})
    children = ET.SubElement(path, "children")
    for index, point in enumerate(((-.23, 1.5, -26), (1, 1.5, -12), (8, 1.5, 0), (10, 1.5, 14), (-4, 1.5, 17), (-10, 1.5, -4))):
        node = ET.SubElement(children, "item", {"name": f"Rail point {index + 1}", "enabled": "true"})
        transform(node, point)
        ET.SubElement(node, "components")
        ET.SubElement(node, "children")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    tree.write(args.output, encoding="utf-8", xml_declaration=True)
    print(args.output)


if __name__ == "__main__":
    main()
