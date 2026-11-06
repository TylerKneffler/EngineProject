"""Convert legacy material shader pass lists to a single surface shader."""
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET
from xml.sax.saxutils import quoteattr


def surface(entries):
    for entry in entries:
        if isinstance(entry, str):
            return entry
        if isinstance(entry, dict) and entry.get("enabled", True):
            return entry.get("path", "")
    return ""


def migrate_json(value):
    changed = 0
    if isinstance(value, dict):
        if isinstance(value.get("shaders"), list):
            value.setdefault("shader", surface(value.pop("shaders")))
            changed += 1
        for child in value.values():
            changed += migrate_json(child)
    elif isinstance(value, list):
        for child in value:
            changed += migrate_json(child)
    return changed


def migrate_xml(text):
    count = 0

    def material(match):
        nonlocal count
        block = match.group(0)
        legacy = re.search(r"<Material\.shaders\b[^>]*(?:/>|>.*?</Material\.shaders>)", block, re.S)
        if not legacy:
            return block
        node = ET.fromstring(legacy.group(0))
        entries = []
        for item in node:
            if not item.attrib and not len(item):
                entries.append(item.text or "")
                continue
            fields = {key.split(".")[-1]: value for key, value in item.attrib.items()}
            fields.update({child.tag.split(".")[-1]: child.text or "" for child in item})
            entries.append({"path": fields.get("path", ""),
                            "enabled": fields.get("enabled", "true").lower() != "false"})
        start, end = legacy.span()
        line_start = block.rfind("\n", 0, start) + 1
        line_end = block.find("\n", end)
        if not block[line_start:start].strip() and line_end >= 0 and not block[end:line_end].strip():
            start, end = line_start, line_end + 1
        block = block[:start] + block[end:]
        opening_end = block.index(">")
        if "Material.shader=" not in block[:opening_end]:
            block = block[:opening_end] + " Material.shader=" + quoteattr(surface(entries)) + block[opening_end:]
        count += 1
        return block

    result = re.sub(r"<components:Material\b[^>]*>.*?</components:Material>", material, text, flags=re.S)
    if count:
        ET.fromstring(result)
    return result, count


def main():
    root = Path(__file__).resolve().parents[1]
    files = lists = 0
    for folder in (root / "Engine/Core/Assets", root / "ProjectTemplate"):
        for path in folder.rglob("*"):
            if path.suffix not in (".material", ".scene", ".prefab"):
                continue
            original = path.read_text(encoding="utf-8-sig")
            if '"shaders"' not in original and "Material.shaders" not in original:
                continue
            if original.lstrip().startswith("<"):
                updated, count = migrate_xml(original)
            else:
                value = json.loads(original)
                count = migrate_json(value)
                updated = json.dumps(value, indent=2, ensure_ascii=False) + "\n"
            if count:
                # Preserve existing line endings for large prefab/scene files.
                raw = path.read_bytes()
                newline = "\r\n" if b"\r\n" in raw else "\n"
                with path.open("w", encoding="utf-8", newline=newline) as output:
                    output.write(updated)
                print(f"{path.relative_to(root)}: {count}")
                files += 1
                lists += count
    print(f"Migrated {lists} shader lists in {files} files.")


if __name__ == "__main__":
    main()
