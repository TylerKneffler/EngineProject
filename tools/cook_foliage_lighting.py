"""Bakes crown lighting data into tree and bush leaf meshes (.md/todo.md Foliage Phase 1).

Leaf cards lit by their own flat normals make a crown read as speckled flat
green. This cook gives every leaf vertex the lighting of the crown it sits in:

  * normal: bent 75% from the card normal toward the crown surface normal, so
    a crown shades as one rounded volume (lit side, dark side, soft terminator);
  * uv1.x: canopy occlusion (1 at the outer shell, ~0.2 deep inside, less on
    the underside), used for ambient/sky light and partly for direct light;
  * uv1.y: 4 + depth into the crown (0 at the shell, 1 at a lobe centre). The
    4.0 offset marks a cooked mesh: real UVs never reach it, so the shader
    (Object.hlsl, Material::foliageTranslucency) only trusts uv1 when
    uv1.y >= 3.5.

The crown is a sum of Gaussian lobes fitted to the LOD0 leaf vertices with
k-means (more lobes for bigger trees), so big oaks get a lumpy crown rather
than one egg. The same crown is applied to every LOD of a mesh chain, so LOD
transitions keep their lighting.

Leaf meshes are found through the woodland prefabs: every Mesh whose material
is alpha-masked and not an impostor. Cooked meshes are skipped (normals are
overwritten, so cooking twice would bend them twice); regenerate or restore a
mesh from git before cooking it again.

Usage: python tools/cook_foliage_lighting.py [--dry-run] [prefab-or-dir ...]
Defaults to Prefabs/Woodland/{Oaks,Conifers,Bushes}.
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from woodland_assets import NATIVE_MESH_MAGIC, VERTEX_FLOATS  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
DEFAULT_SOURCES = [ASSETS / "Prefabs" / "Woodland" / d for d in ("Oaks", "Conifers", "Bushes")]
COOKED_MARK = 4.0
CROWN_BEND = 0.75
LOBE_SIZE = 4.0  # metres of crown extent per k-means lobe
MAX_LOBES = 8
MIN_OCCLUSION = 0.2


def resolve(asset_path: str) -> Path:
    path = asset_path.replace("\\", "/")
    if path.startswith("Engine/Core/"):
        return ROOT / path
    if path.startswith("Assets/"):
        return ASSETS.parent / path
    return ROOT / path


def leaf_chains(prefab: Path):
    """(mesh, [lod meshes]) for each alpha-masked, non-impostor Mesh in a prefab."""
    text = prefab.read_text(encoding="utf-8")
    for match in re.finditer(
            r'Mesh\.file="([^"]+)"(.*?)(?:</components:Mesh>|/>)\s*'
            r'<components:Material[^>]*materialAsset="([^"]+)"', text, re.S):
        material = json.loads(resolve(match.group(3)).read_text(encoding="utf-8"))
        if material.get("alphaMode") != "Mask" or material.get("impostorBillboard"):
            continue
        lods = [m for m in re.findall(r"<item>([^<]+\.mesh)</item>", match.group(2))
                if "Impostor" not in m]
        yield resolve(match.group(1)), [resolve(m) for m in lods]


def read(path: Path):
    raw = bytearray(path.read_bytes())
    magic, _, count, index_count = struct.unpack_from("<4I", raw, 0)
    if magic != NATIVE_MESH_MAGIC:
        raise ValueError(f"{path}: not a native mesh")
    data = np.frombuffer(raw, np.float32, count * VERTEX_FLOATS, 16).reshape(count, VERTEX_FLOATS).copy()
    return raw, data


def write(path: Path, raw: bytearray, data: np.ndarray):
    raw[16:16 + data.nbytes] = data.astype(np.float32).tobytes()
    path.write_bytes(bytes(raw))


def fit_crown(points: np.ndarray):
    """Gaussian lobes (centres, radii) fitted to leaf vertex positions."""
    extent = float(np.max(points.max(0) - points.min(0)))
    k = int(np.clip(round(extent / LOBE_SIZE), 1, MAX_LOBES))
    rng = np.random.default_rng(7)
    sample = points[rng.choice(len(points), min(len(points), 6000), replace=False)]
    # k-means++ seeding, then Lloyd iterations.
    centres = [sample[0]]
    for _ in range(1, k):
        d = np.min([np.sum((sample - c) ** 2, 1) for c in centres], axis=0)
        centres.append(sample[int(np.argmax(d))])
    centres = np.array(centres)
    for _ in range(25):
        label = np.argmin(((sample[:, None] - centres[None]) ** 2).sum(-1), 1)
        centres = np.array([sample[label == i].mean(0) if np.any(label == i) else centres[i]
                            for i in range(k)])
    label = np.argmin(((sample[:, None] - centres[None]) ** 2).sum(-1), 1)
    floor = max(extent * 0.08, 0.15)
    radii = np.array([np.maximum(1.75 * sample[label == i].std(0), floor) if np.sum(label == i) > 3
                      else np.full(3, extent * 0.3) for i in range(k)])
    return centres, radii


def crown_field(points, centres, radii):
    """Field value F and outward normal at each point (metaball of lobes)."""
    q = (points[:, None, :] - centres[None]) / radii[None]       # n,k,3
    weight = np.exp(-np.sum(q * q, -1))                           # n,k
    field = weight.sum(1)
    gradient = np.sum(weight[..., None] * q / radii[None], 1)     # -dF/dp / 2
    length = np.linalg.norm(gradient, axis=1, keepdims=True)
    fallback = points - centres.mean(0)
    fallback /= np.maximum(np.linalg.norm(fallback, axis=1, keepdims=True), 1e-6)
    normal = np.where(length > 1e-6, gradient / np.maximum(length, 1e-12), fallback)
    return field, normal


def cook(data: np.ndarray, centres, radii):
    p, n, t = data[:, 0:3].astype(np.float64), data[:, 3:6].astype(np.float64), data[:, 8:12].astype(np.float64)
    field, crown = crown_field(p, centres, radii)
    depth = np.clip(1.0 + np.log(np.maximum(field, 1e-12)), 0.0, 1.0)
    # Interior darkening, plus less sky on the underside of the crown.
    shell = 1.0 - (1.0 - MIN_OCCLUSION) * depth ** 0.85
    sky = 0.4 + 0.6 * (0.5 + 0.5 * crown[:, 1])
    occlusion = np.clip(shell * sky, MIN_OCCLUSION * 0.8, 1.0)

    card = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    flip = np.sum(card * crown, 1) < 0.0
    card[flip] *= -1.0
    bent = (1.0 - CROWN_BEND) * card + CROWN_BEND * crown
    bent /= np.maximum(np.linalg.norm(bent, axis=1, keepdims=True), 1e-9)
    tangent = t[:, 0:3] - bent * np.sum(t[:, 0:3] * bent, 1, keepdims=True)
    tlen = np.linalg.norm(tangent, axis=1, keepdims=True)
    good = tlen[:, 0] > 1e-6
    tangent[good] /= tlen[good]
    handed = np.where(flip, -t[:, 3], t[:, 3])

    out = data.copy()
    out[:, 3:6] = bent
    out[good, 8:11] = tangent[good]
    out[:, 11] = handed
    out[:, 12] = occlusion
    out[:, 13] = COOKED_MARK + depth
    return out, occlusion, depth


def is_cooked(data: np.ndarray) -> bool:
    return bool(np.min(data[:, 13]) >= COOKED_MARK - 0.5)


def cook_prefabs(sources, dry_run=False):
    """Cook every uncooked leaf mesh referenced by the given prefabs / dirs.
    Generators call this after writing meshes (e.g. generate_conifers.py)."""
    prefabs = []
    for source in sources:
        source = Path(source)
        prefabs += sorted(source.glob("*.prefab")) if source.is_dir() else [source]
    chains = {}
    for prefab in prefabs:
        for mesh, lods in leaf_chains(prefab):
            chains.setdefault(mesh, lods)
    written = skipped = 0
    for mesh, lods in chains.items():
        meshes = {path: read(path) for path in [mesh] + lods}
        pending = [path for path, (_, data) in meshes.items() if not is_cooked(data)]
        skipped += len(meshes) - len(pending)
        if not pending:
            continue
        # Cooking leaves positions alone, so LOD0 fits the crown either way and
        # every LOD of the chain shares it.
        centres, radii = fit_crown(meshes[mesh][1][:, 0:3].astype(np.float64))
        for path in pending:
            raw, data = meshes[path]
            out, occlusion, depth = cook(data, centres, radii)
            print(f"{path.relative_to(ROOT)}: {len(data)} verts, {len(centres)} lobes, "
                  f"occlusion {occlusion.min():.2f}..{occlusion.max():.2f} "
                  f"(mean {occlusion.mean():.2f}), interior {np.mean(depth > 0.5):.0%}")
            if not dry_run:
                write(path, raw, out)
            written += 1
    print(f"{written} meshes cooked, {skipped} already cooked" + (" (dry run)" if dry_run else ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sources", nargs="*", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    cook_prefabs(args.sources or DEFAULT_SOURCES, args.dry_run)


if __name__ == "__main__":
    main()
