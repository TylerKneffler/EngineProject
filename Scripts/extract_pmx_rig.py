"""Extract the rig and IK profile from a PMX 2.0/2.1 file.

Usage: python Scripts/extract_pmx_rig.py model.pmx output.mmdrig.json [--check]
The output records PMX model-space rest positions without changing handedness.
Format layout: https://github.com/dskjal/OpenMMD/blob/main/docs/specification/pmx2.1-specification.md
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def read(self, size):
        if size < 0 or self.pos + size > len(self.data):
            raise ValueError(f"Truncated PMX at byte {self.pos}")
        result = self.data[self.pos:self.pos + size]
        self.pos += size
        return result

    def unpack(self, fmt):
        return struct.unpack("<" + fmt, self.read(struct.calcsize("<" + fmt)))

    def u8(self):
        return self.unpack("B")[0]

    def i32(self):
        return self.unpack("i")[0]

    def f32(self):
        return self.unpack("f")[0]

    def vec3(self):
        return list(self.unpack("fff"))

    def count(self, label, limit):
        value = self.i32()
        if not 0 <= value <= limit:
            raise ValueError(f"Invalid {label} count {value} at byte {self.pos - 4}")
        return value

    def index(self, size, signed=True):
        return self.unpack({(1, True): "b", (1, False): "B",
                            (2, True): "h", (2, False): "H",
                            (4, True): "i", (4, False): "I"}[(size, signed)])[0]

    def text(self, encoding):
        size = self.count("text byte", 4_000_000)
        return self.read(size).decode(encoding)


def extract(data):
    r = Reader(data)
    if r.read(4) != b"PMX ":
        raise ValueError("Not a PMX file")
    version = r.f32()
    if abs(version - 2.0) > 0.01 and abs(version - 2.1) > 0.01:
        raise ValueError(f"Unsupported PMX version {version}")
    if r.u8() != 8:
        raise ValueError("Unsupported PMX header size")
    encoding_flag, extra_uv, vertex_size, texture_size, material_size, bone_size, morph_size, rigid_size = r.read(8)
    if encoding_flag not in (0, 1) or extra_uv > 4 or any(
        size not in (1, 2, 4) for size in
        (vertex_size, texture_size, material_size, bone_size, morph_size, rigid_size)
    ):
        raise ValueError("Invalid PMX header settings")
    encoding = "utf-16-le" if encoding_flag == 0 else "utf-8"
    names = [r.text(encoding) for _ in range(4)]

    vertex_count = r.count("vertex", 5_000_000)
    for _ in range(vertex_count):
        r.read(32 + extra_uv * 16)  # position, normal, UV, extra UVs
        deform = r.u8()
        if deform == 0:  # BDEF1
            r.read(bone_size)
        elif deform == 1:  # BDEF2
            r.read(2 * bone_size + 4)
        elif deform == 2 or deform == 4:  # BDEF4 / QDEF
            r.read(4 * bone_size + 16)
        elif deform == 3:  # SDEF
            r.read(2 * bone_size + 4 + 36)
        else:
            raise ValueError(f"Unknown PMX vertex deform {deform}")
        r.read(4)  # outline scale

    surface_count = r.count("surface", 30_000_000)
    if surface_count % 3:
        raise ValueError("PMX surface count is not a multiple of three")
    r.read(surface_count * vertex_size)
    for _ in range(r.count("texture", 100_000)):
        r.text(encoding)
    for _ in range(r.count("material", 100_000)):
        r.text(encoding)
        r.text(encoding)
        r.read(16 + 12 + 4 + 12 + 1 + 16 + 4)
        r.read(2 * texture_size + 1)
        toon_reference = r.u8()
        if toon_reference not in (0, 1):
            raise ValueError("Invalid PMX toon reference")
        r.read(texture_size if toon_reference == 0 else 1)
        r.text(encoding)
        r.read(4)

    bones = []
    for index in range(r.count("bone", 100_000)):
        local_name = r.text(encoding)
        universal_name = r.text(encoding)
        position = r.vec3()
        parent = r.index(bone_size)
        layer = r.i32()
        flags = r.unpack("H")[0]
        tail = {"bone": r.index(bone_size)} if flags & 0x0001 else {"offset": r.vec3()}
        bone = {"index": index, "name": local_name, "universalName": universal_name,
                "position": position, "parent": parent, "layer": layer,
                "flags": flags, "tail": tail}
        if flags & 0x0300:
            bone["inherit"] = {"bone": r.index(bone_size), "weight": r.f32(),
                               "rotation": bool(flags & 0x0100),
                               "translation": bool(flags & 0x0200)}
        if flags & 0x0400:
            bone["fixedAxis"] = r.vec3()
        if flags & 0x0800:
            bone["localAxes"] = {"x": r.vec3(), "z": r.vec3()}
        if flags & 0x2000:
            bone["externalParent"] = r.i32()
        if flags & 0x0020:
            target = r.index(bone_size)
            loops = r.count("IK loop", 100_000)
            angle_limit = r.f32()
            links = []
            for _ in range(r.count("IK link", 1000)):
                link = {"bone": r.index(bone_size)}
                has_limits = r.u8()
                if has_limits not in (0, 1):
                    raise ValueError("Invalid PMX IK link limit flag")
                if has_limits:
                    link["minimum"] = r.vec3()
                    link["maximum"] = r.vec3()
                links.append(link)
            bone["ik"] = {"target": target, "iterations": loops,
                          "angleLimitRadians": angle_limit, "links": links}
        bones.append(bone)

    for bone in bones:
        parent = bone["parent"]
        if parent < -1 or parent >= len(bones) or parent == bone["index"]:
            raise ValueError(f"Invalid parent for PMX bone {bone['index']}")
        origin = bones[parent]["position"] if parent >= 0 else [0.0, 0.0, 0.0]
        bone["localTranslation"] = [p - o for p, o in zip(bone["position"], origin)]
        references = []
        if "bone" in bone["tail"]:
            references.append(bone["tail"]["bone"])
        if "inherit" in bone:
            references.append(bone["inherit"]["bone"])
        if "ik" in bone:
            references.append(bone["ik"]["target"])
            references.extend(link["bone"] for link in bone["ik"]["links"])
        if any(ref < -1 or ref >= len(bones) for ref in references):
            raise ValueError(f"Invalid bone reference in PMX bone {bone['index']}")

    return {"format": "mmd-rig-profile-v1", "sourceFormat": "PMX",
            "sourceSha256": hashlib.sha256(data).hexdigest(), "pmxVersion": version,
            "coordinateSystem": "PMX model space; source handedness and units unchanged",
            "modelName": names[0], "modelNameEnglish": names[1],
            "boneCount": len(bones), "ikChainCount": sum("ik" in bone for bone in bones),
            "bones": bones}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true", help="verify the saved profile is current")
    args = parser.parse_args()
    profile = extract(args.source.read_bytes())
    serialized = json.dumps(profile, ensure_ascii=False, indent=2) + "\n"
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != serialized:
            raise SystemExit("PMX rig profile is missing or stale")
        print(f"Verified {profile['boneCount']} bones and {profile['ikChainCount']} IK chains")
    else:
        args.output.write_text(serialized, encoding="utf-8")
        print(f"Wrote {profile['boneCount']} bones and {profile['ikChainCount']} IK chains")


if __name__ == "__main__":
    main()
