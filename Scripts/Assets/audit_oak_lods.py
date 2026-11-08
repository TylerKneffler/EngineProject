"""Inventory native oak geometry and material costs without loading the editor."""
import argparse
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "Engine/Core/Assets"


def resolve(reference):
    reference = reference.replace("\\", "/")
    if reference.startswith("Assets/"):
        return ASSETS / reference[7:]
    return ROOT / reference


def inventory():
    result = []
    for prefab in sorted((ASSETS / "Prefabs/Woodland/Oaks").glob("*.prefab")):
        parts = []
        for component_group in ET.parse(prefab).getroot().iter("components"):
            mesh = next((e for e in component_group if e.tag.endswith("}Mesh")), None)
            material = next((e for e in component_group if e.tag.endswith("}Material")), None)
            if mesh is None:
                continue
            path = resolve(mesh.attrib["Mesh.file"])
            with path.open("rb") as stream:
                magic, version, vertices, indices = struct.unpack("<4I", stream.read(16))
            if magic != 0x4853454D or version != 7:
                raise ValueError(f"Unsupported native mesh: {path}")
            material_path = material.attrib.get("Material.materialAsset") if material is not None else None
            data = json.loads(resolve(material_path).read_text()) if material_path else {}
            parts.append(dict(mesh=mesh.attrib["Mesh.file"], triangles=(indices or vertices) // 3,
                              material=material_path, double_sided=data.get("doubleSided"),
                              alpha_cutoff=data.get("alphaCutoff"), shaders=data.get("shader") or data.get("shaders"),
                              specular_model=data.get("specularModel"),
                              # Source-level map fetch sites, not measured GPU reads.
                              # MR and AO remain separate reads even if they share an image.
                              object_map_reads_full=sum(bool(data.get(k)) for k in (
                                  "baseColorTexture", "metallicRoughnessTexture", "normalTexture",
                                  "occlusionTexture", "emissiveTexture")),
                              object_map_reads_far=sum(bool(data.get(k)) for k in (
                                  "baseColorTexture", "emissiveTexture")),
                              shadow_alpha_map_reads=int(bool(data.get("baseColorTexture")) and
                                  data.get("alphaMode")=="Mask" and data.get("alphaCutoff",0)>0),
                              extra_color_read_with_independent_alpha_mip=int(bool(data.get("baseColorTexture")) and
                                  data.get("alphaMode")=="Mask" and data.get("alphaCutoff",0)>0),
                              texture_maps={k:v for k,v in data.items() if k.endswith("Texture") and isinstance(v,str) and v}))
        result.append(dict(prefab=prefab.name, triangles=sum(p["triangles"] for p in parts),
                           material_count=len({p["material"] for p in parts}),
                           material_slots=len(parts),
                           source_color_draws_unbatched=len(parts),
                           shader_paths=sorted({p["shaders"] or "Engine/Core/Shaders/Object/Object.hlsl" for p in parts}),
                           parts=parts))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".local/forest-performance/asset-audit.json")
    args = parser.parse_args()
    rows = inventory()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print(f"Audited {len(rows)} oak prefabs: {args.output}")


if __name__ == "__main__":
    main()
