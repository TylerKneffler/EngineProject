"""Marks the objects of a scene static (Object::isStatic, `static="true"`).

Every top-level object is marked except those that hold a player or
character controller anywhere in their hierarchy; children are static
through their parent, so only top-level objects carry the attribute. The
engine reports objects whose components contradict the flag when play starts
(Engine/Core/Scene/StaticObjects.h), so run a scene once after marking it.

The terrain scene builders call mark_static() before they write. For a scene
file that is not generated, run this script on it: it adds the attribute to
the top-level <item> tags in place and leaves the rest of the file as it is.

Usage: python tools/scene_static.py <scene> [<scene> ...]
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

NAMESPACE = "urn:engine-components"
# Components that move their owner under player control.
CONTROLLERS = ("FirstPersonController", "CharacterController", "ThirdPersonController")


def is_player(item):
    """True when the object or a descendant has a controller component."""
    return any(element.tag == f"{{{NAMESPACE}}}{name}"
               for element in item.iter() for name in CONTROLLERS)


def mark_static(objects):
    """Sets static="true" on each object item directly under `objects`
    (the scene's <objects> element) unless it is the player. Returns the
    names of the objects left dynamic."""
    dynamic = []
    for item in objects.findall("item"):
        if is_player(item):
            item.attrib.pop("static", None)
            dynamic.append(item.get("name") or item.get("prefab") or "?")
        else:
            item.set("static", "true")
    return dynamic


def mark_file(path):
    # newline="" keeps the file's own line endings.
    with open(path, encoding="utf-8", newline="") as file:
        text = file.read()
    root = ET.fromstring(text[text.index("<Scene"):])
    items = root.find("objects").findall("item")
    # Top-level object tags: the scene files indent with two spaces, so the
    # children of <objects> open at four.
    pattern = re.compile(r'^(    <item (?:name|prefab)="[^>]*?)( static="(?:true|false)")?(\s*/?>)', re.M)
    tags = list(pattern.finditer(text))
    if len(tags) != len(items):
        raise SystemExit(f"{path}: found {len(tags)} top-level <item> tags for "
                         f"{len(items)} objects; not a two-space indented scene file")
    pieces, last, dynamic = [], 0, []
    for match, item in zip(tags, items):
        pieces.append(text[last:match.start()])
        if is_player(item):
            pieces.append(match.group(1) + match.group(3))
            dynamic.append(item.get("name") or item.get("prefab") or "?")
        else:
            pieces.append(match.group(1) + ' static="true"' + match.group(3))
        last = match.end()
    pieces.append(text[last:])
    with open(path, "w", encoding="utf-8", newline="") as file:
        file.write("".join(pieces))
    print(f"{path}: {len(items) - len(dynamic)} objects static; dynamic: {', '.join(dynamic) or 'none'}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for scene in sys.argv[1:]:
        mark_file(scene)
