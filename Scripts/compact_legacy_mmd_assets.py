"""Compact triangle-expanded MMD mesh/morph assets in one directory.

Dry run by default. --apply writes indexed version-7 meshes and remaps morphs.
Every shared vertex must have identical morph offsets before any file changes.
"""

import argparse
from pathlib import Path
import struct


def compact(directory: Path, apply: bool) -> None:
    replacements: list[tuple[Path, bytes]] = []
    old_vertices = new_vertices = 0
    for mesh_path in sorted(directory.glob("*.mesh")):
        mesh = mesh_path.read_bytes()
        magic, version, count, index_count = struct.unpack_from("<4I", mesh)
        if magic != 0x4853454D or version not in (5, 6, 7):
            raise ValueError(f"Unexpected mesh format: {mesh_path}")
        if index_count:
            continue
        stride = {5: 136, 6: 216, 7: 264}[version]
        if len(mesh) != 16 + count * stride:
            raise ValueError(f"Unexpected mesh size: {mesh_path}")
        lookup: dict[bytes, int] = {}
        vertices: list[bytes] = []
        representatives: list[int] = []
        indices: list[int] = []
        for old_index in range(count):
            start = 16 + old_index * stride
            vertex = mesh[start:start + stride]
            new_index = lookup.get(vertex)
            if new_index is None:
                new_index = len(vertices)
                lookup[vertex] = new_index
                representatives.append(old_index)
                if version == 5:
                    primary_uv = vertex[24:32]
                    secondary_uv = vertex[48:56]
                    extra_uv = primary_uv + bytes(8) + secondary_uv + bytes(8) + bytes(48)
                    vertex += extra_uv + bytes(48)
                elif version == 6:
                    vertex += bytes(48)
                vertices.append(vertex)
            indices.append(new_index)
        output = bytearray(struct.pack("<4I", magic, 7, len(vertices), count))
        output.extend(b"".join(vertices))
        output.extend(struct.pack(f"<{count}I", *indices))
        replacements.append((mesh_path, bytes(output)))
        old_vertices += count
        new_vertices += len(vertices)

        morph_path = mesh_path.with_suffix(".morph")
        if not morph_path.exists():
            continue
        morph = morph_path.read_bytes()
        morph_magic, morph_version, targets = struct.unpack_from("<3I", morph)
        if morph_magic != 0x46524F4D or morph_version not in (1, 2, 3):
            raise ValueError(f"Invalid morph asset: {morph_path}")
        output = bytearray(morph[:12])
        offset = 12
        field_count = 3 + (morph_version >= 2) + 5 * (morph_version >= 3)
        for _ in range(targets):
            name_length, = struct.unpack_from("<I", morph, offset)
            if name_length > 4096:
                raise ValueError(f"Invalid morph name in {morph_path}")
            output.extend(morph[offset:offset + 4 + name_length])
            offset += 4 + name_length
            for field in range(field_count):
                length, = struct.unpack_from("<I", morph, offset)
                offset += 4
                if length not in (0, count):
                    raise ValueError(f"Unexpected morph size in {morph_path}")
                size = 12 if field < 3 else 16
                data = memoryview(morph)[offset:offset + length * size]
                if length:
                    for old_index, new_index in enumerate(indices):
                        original = data[old_index * size:(old_index + 1) * size]
                        representative = representatives[new_index]
                        expected = data[representative * size:(representative + 1) * size]
                        if original != expected:
                            raise ValueError(f"Different morph offsets for a shared vertex in {morph_path}")
                    output.extend(struct.pack("<I", len(vertices)))
                    for representative in representatives:
                        output.extend(data[representative * size:(representative + 1) * size])
                else:
                    output.extend(struct.pack("<I", 0))
                offset += length * size
        if offset != len(morph):
            raise ValueError(f"Trailing morph data in {morph_path}")
        replacements.append((morph_path, bytes(output)))
        print(f"{mesh_path.name}: {count:,} -> {len(vertices):,} vertices, "
              f"{targets} morph targets")

    if not old_vertices:
        print("No unindexed meshes to compact")
        return
    print(f"Total: {old_vertices:,} -> {new_vertices:,} vertices "
          f"({new_vertices / old_vertices:.1%})")
    if apply:
        for path, data in replacements:
            path.write_bytes(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--apply", action="store_true")
    arguments = parser.parse_args()
    compact(arguments.directory, arguments.apply)
