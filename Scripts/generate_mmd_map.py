"""Build Mococo's reviewed PMX-to-FBX node map from an FBX node audit.

Usage: python Scripts/generate_mmd_map.py PMX_PROFILE FBX_NODES_TSV VMD OUTPUT
The TSV is emitted by MmdImportStepsTests with MMD_DUMP_FBX_NODES=1.
The coordinate conversion is verified against 214 matching rest landmarks.
"""

import argparse
from collections import Counter
import json
import math
from pathlib import Path
import struct
import unicodedata


SPECIAL = {
    0: ("root.x", "scene-root"),
    1: ("root.x", "center"),
    2: ("root.x", "groove"),
    3: ("root.x", "waist"),
    44: ("head.x", "eye-helper"),
    155: ("arm_stretch.r", "twist"),
    156: ("arm_stretch.r", "distributed-twist"),
    157: ("arm_stretch.r", "distributed-twist"),
    158: ("arm_stretch.r", "distributed-twist"),
    160: ("forearm_twist.r", "twist"),
    161: ("forearm_twist.r", "distributed-twist"),
    162: ("forearm_twist.r", "distributed-twist"),
    163: ("forearm_twist.r", "distributed-twist"),
    189: ("arm_stretch.l", "twist"),
    190: ("arm_stretch.l", "distributed-twist"),
    191: ("arm_stretch.l", "distributed-twist"),
    192: ("arm_stretch.l", "distributed-twist"),
    194: ("forearm_twist.l", "twist"),
    195: ("forearm_twist.l", "distributed-twist"),
    196: ("forearm_twist.l", "distributed-twist"),
    197: ("forearm_twist.l", "distributed-twist"),
    222: ("root.x", "ik-parent"),
    225: ("root.x", "ik-parent"),
    233: ("toes_01.r", "toe-helper"),
    237: ("toes_01.l", "toe-helper"),
}

ROLES = {
    150: "shoulder-parent", 152: "shoulder-cancel",
    184: "shoulder-parent", 186: "shoulder-cancel",
    223: "ik-control", 224: "ik-control",
    226: "ik-control", 227: "ik-control",
    228: "waist-cancel", 229: "waist-cancel",
    230: "deform-helper", 231: "deform-helper", 232: "deform-helper",
    234: "deform-helper", 235: "deform-helper", 236: "deform-helper",
    238: "head-helper",
}

# VMD names from the declared "black" rig that correspond to Mococo controls
# with an ASCII PMX name, or to one typo in Mococo's PMX bone name.
MOTION_ALIASES = {
    "右小指０": 165, "右薬指０": 169, "右中指０": 173, "右人指０": 177,
    "左小指０": 199, "左薬指０": 203, "左中指０": 207, "左人指０": 211,
    "左薬指３": 206,
}


def source_position(bone):
    x, y, z = bone["position"]
    return (x * 8.0, y * 8.0, -z * 8.0)


def read_nodes(path):
    nodes = []
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        fields = line.split("\t")
        if len(fields) != 9:
            raise ValueError("Expected FBX node audit with local and world positions")
        nodes.append({"index": int(fields[0]), "parent": int(fields[1]),
                      "name": fields[2], "world": tuple(map(float, fields[6:9]))})
    if len(nodes) != 326 or nodes[56]["name"] != "Mococo_rig":
        raise ValueError("FBX node audit is not the expected Mococo import")
    return nodes


def read_vmd_names(path):
    data = Path(path).read_bytes()
    if not data.startswith(b"Vocaloid Motion Data 0002"):
        raise ValueError("Expected modern VMD")
    offset = 50
    count = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    names = Counter()
    for _ in range(count):
        name = data[offset:offset + 15].split(b"\0", 1)[0].decode("cp932")
        names[name] += 1
        offset += 111
    return names, data[30:50].split(b"\0", 1)[0].decode("cp932")


def generate(profile, nodes, vmd_names, vmd_model):
    candidates = [node for node in nodes if node["index"] >= 57 and
                  not node["name"].endswith("_end")]
    by_name = {node["name"]: node for node in candidates}
    entries = []
    for bone in profile["bones"]:
        index = bone["index"]
        expected = source_position(bone)
        nearest = min(candidates, key=lambda node: math.dist(expected, node["world"]))
        error = math.dist(expected, nearest["world"])
        if index in SPECIAL:
            target_name, role = SPECIAL[index]
            target = by_name[target_name]
            method = "manual"
        elif error <= 0.01:
            target = nearest
            role = ROLES.get(index, "joint")
            method = "rest-position"
        else:
            raise ValueError(f"Unreviewed PMX bone {index}: {bone['name']} ({error:.3f})")
        entries.append({"pmxIndex": index, "pmxName": bone["name"],
                        "fbxNode": target["index"], "fbxName": target["name"],
                        "role": role, "method": method,
                        "restErrorWorldUnits": round(math.dist(expected, target["world"]), 6)})

    mapped_names = {unicodedata.normalize("NFKC", bone["name"]) for bone in profile["bones"]}
    mapped_names.update(unicodedata.normalize("NFKC", name) for name in MOTION_ALIASES)
    unmatched = [{"name": name, "keyCount": count} for name, count in
                 sorted(vmd_names.items()) if unicodedata.normalize("NFKC", name) not in mapped_names]
    root = {
        "format": "mmd-retarget-map-v1",
        "sourceRig": {"profile": "Mococo.mmdrig.json", "boneCount": len(profile["bones"])},
        "targetRig": {"fbx": "Mococo (FBX No Outline).fbx", "nodeCount": len(nodes)},
        "motion": {"file": "../dance1.vmd", "declaredModelName": vmd_model,
                   "sourceRigIdentity": "unverified; standard MMD bone names overlap Mococo"},
        "coordinates": {"pmxToFbxWorldScale": 8.0, "axisOrder": ["x", "y", "z"],
                        "axisSigns": [1, 1, -1], "targetWorldSpace": "decoded FBX bind world"},
        "restPose": {"mode": "source delta applied around each mapped target rest transform",
                     "unanimatedBones": "preserve FBX rest pose",
                     "spatialMatchToleranceWorldUnits": 0.01},
        "rootMotion": {"mode": "scene-travel", "targetNode": 57,
                       "composePmxIndicesInOrder": [0, 1, 2, 3, 4]},
        "bones": entries,
        "motionAliases": [{"vmdName": name, "pmxIndex": index}
                          for name, index in sorted(MOTION_ALIASES.items())],
        "trackReport": {"totalBoneTracks": len(vmd_names),
                        "unmatchedBoneTracks": unmatched,
                        "unmatchedAnimatedTracks": [entry for entry in unmatched
                                                    if entry["keyCount"] > 2]},
    }
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("fbx_nodes", type=Path)
    parser.add_argument("vmd", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    profile = json.loads(args.profile.read_text(encoding="utf-8"))
    names, model = read_vmd_names(args.vmd)
    content = json.dumps(generate(profile, read_nodes(args.fbx_nodes), names, model),
                         ensure_ascii=False, indent=2) + "\n"
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != content:
            raise SystemExit("MMD map is missing or stale")
    else:
        args.output.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
