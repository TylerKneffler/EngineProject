"""Converts non-indexed native woodland meshes to indexed ones (.md/foliage.md Phase 2).

The imported oak/bush pack meshes were written as plain triangle lists: every
triangle carries its own three vertices, so each leaf card corner is shaded by
the vertex shader once per triangle that uses it, with no post-transform cache
reuse. Across Models/Woodland only ~35 % of the vertices are unique (oak bark
LOD0 24 %, oak leaves 34 %). The foliage depth pre-pass, the colour pass and
every shadow cascade run the full object vertex shader, so this is vertex work
paid several times a frame.

Each mesh is rewritten with its byte-identical vertices merged (kept in order
of first use, so neighbouring triangles stay close for the vertex cache) and a
uint32 index buffer. Triangle order and vertex contents are unchanged.
Already-indexed meshes are skipped.

Usage: python tools/index_woodland_meshes.py [--dry-run] [dir ...]
Defaults to Engine/Core/Assets/Models/Woodland.
"""

import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from woodland_assets import NATIVE_MESH_MAGIC, VERTEX_FLOATS  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / "Engine" / "Core" / "Assets" / "Models" / "Woodland"
STRIDE = VERTEX_FLOATS * 4


def index_mesh(path: Path, dry_run: bool):
    raw = path.read_bytes()
    magic, version, count, index_count = struct.unpack_from("<4I", raw, 0)
    if magic != NATIVE_MESH_MAGIC or version != 7:
        return None
    if index_count or count % 3:
        return None
    rows = np.frombuffer(raw, np.uint8, count * STRIDE, 16).reshape(count, STRIDE)
    _, first, inverse = np.unique(rows, axis=0, return_index=True, return_inverse=True)
    # Renumber unique vertices in order of first use.
    order = np.argsort(first)
    rank = np.empty_like(order)
    rank[order] = np.arange(len(order))
    indices = rank[inverse.reshape(-1)].astype(np.uint32)
    vertices = rows[first[order]]
    if not dry_run:
        path.write_bytes(struct.pack("<4I", NATIVE_MESH_MAGIC, 7, len(vertices), len(indices)) +
                         vertices.tobytes() + indices.tobytes())
    return count, len(vertices)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dirs", nargs="*", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    before = after = files = 0
    for directory in args.dirs or [DEFAULT]:
        for path in sorted(Path(directory).rglob("*.mesh")):
            result = index_mesh(path, args.dry_run)
            if result:
                files += 1
                before += result[0]
                after += result[1]
    print(f"{files} meshes indexed: {before} -> {after} vertices "
          f"({after / max(before, 1):.0%})" + (" (dry run)" if args.dry_run else ""))


if __name__ == "__main__":
    main()
