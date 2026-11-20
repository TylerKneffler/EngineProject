"""Fetches CC0 scanned props from Poly Haven and cooks them into game-ready prefabs.

Poly Haven (https://polyhaven.com) models are CC0. For each asset in ASSETS this
  1. downloads the FBX and the 2k diffuse / normal (GL) / roughness JPGs into
     .local/polyhaven/<asset>/ (ignored by git; re-used on later runs),
  2. cooks a LOD chain with tools/PropCook (quadric simplification that keeps
     UV seams; ground pivot) into Models/Woodland/Scans/Meshes,
  3. copies the textures to Models/Woodland/Scans/Textures and writes a
     material and a prefab (Prefabs/Woodland/Scans/<Name>.prefab) carrying
     every level, placeable by TerrainObjectLayer.

Build PropCook first:
  cmake --build build --config RelWithDebInfo --target PropCook

Usage: python tools/fetch_polyhaven_props.py [--only name,...] [--list]
"""

import argparse
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".local" / "polyhaven"
MODELS = wa.ASSETS / "Models" / "Woodland" / "Scans"
MESHES, TEXTURES, MATERIALS = MODELS / "Meshes", MODELS / "Textures", MODELS / "Materials"
PREFABS = wa.ASSETS / "Prefabs" / "Woodland" / "Scans"
API = "https://api.polyhaven.com/files/"
AGENT = {"User-Agent": "asset-fetch/1.0"}
RESOLUTION = "2k"

# asset id -> (prefab name, LOD triangle targets, LOD distances, alpha-masked[, node]).
# node: for sources that ship their own LOD nodes, the one mesh node to cook
# (PropCook --split --only); the prefab is then named <name>_<node>.
ASSETS = {
    "rock_face_01": ("RockFace_01", (8000, 2500, 500), (90, 300), False),
    "rock_face_02": ("RockFace_02", (10000, 3000, 600), (90, 300), False),
    "mountainside": ("Mountainside", (20000, 6000, 1200), (160, 500), False, 1),
    "namaqualand_cliff_01": ("Cliff_Scan_01", (16000, 5000, 1000), (140, 450), False),
    "namaqualand_cliff_02": ("Cliff_Scan_02", (20000, 6000, 1200), (140, 450), False),
    # Forest floor props.
    "tree_stump_01": ("Stump_01", (3000, 900, 250), (35, 100), False),
    "tree_stump_02": ("Stump_02", (3000, 900, 250), (35, 100), False),
    # Sets: one prop per listed mesh node (<name>_<node>).
    "rock_moss_set_01": ("MossRock", (4000, 1200, 300), (35, 100), False, (0, 1, 2, 3, 4, 5)),
    "dry_branches_medium_01": ("DryBranch", (1500, 500, 150), (25, 70), False, (0, 1, 2)),
    # Leafy plants: alpha-masked cards (the Alpha map is merged into the colour).
    "fern_02": ("FernScan", (2400, 900, 300), (20, 50), True, (0, 1, 2, 3)),
}


def download(url: str, path: Path):
    if path.exists() and path.stat().st_size > 0:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"  downloading {path.name}")
    request = urllib.request.Request(url, headers=AGENT)
    with urllib.request.urlopen(request, timeout=300) as response, open(path, "wb") as out:
        shutil.copyfileobj(response, out)


def find_prop_cook():
    for config in ("RelWithDebInfo", "Release", "Debug"):
        path = ROOT / "build" / config / "PropCook.exe"
        if path.exists():
            return path
    raise SystemExit("PropCook is not built: cmake --build build --config RelWithDebInfo --target PropCook")


def fetch(asset: str):
    folder = CACHE / asset
    listing = folder / "files.json"
    if not listing.exists():
        download(API + asset, listing)
    files = json.loads(listing.read_text(encoding="utf-8"))
    fbx = folder / f"{asset}.fbx"
    download(files["fbx"][RESOLUTION]["fbx"]["url"], fbx)
    maps = {}
    masked = "Alpha" in files
    for key, tag in (("Diffuse", "diff"), ("nor_gl", "nor_gl"), ("Rough", "rough"), ("Alpha", "alpha")):
        entry = files.get(key, {}).get(RESOLUTION, {})
        source = entry.get("jpg") or entry.get("png")
        if not source:
            continue
        extension = Path(source["url"]).suffix
        maps[tag] = folder / f"{asset}_{tag}{extension}"
        download(source["url"], maps[tag])
    return fbx, maps


def drop_fragments(path: Path, keep_share=0.03):
    """Removes small disconnected pieces from a cooked mesh: scans carry loose
    shards (debris, plant scraps) that float in the air once the prop is
    scaled up. Components are joined by shared positions; those with under
    keep_share of the largest one's triangles go."""
    import struct
    import numpy as np
    raw = path.read_bytes()
    magic, version, count, index_count = struct.unpack_from("<4I", raw, 0)
    stride = wa.VERTEX_FLOATS * 4
    vertices = np.frombuffer(raw, np.uint8, count * stride, 16).reshape(count, stride)
    indices = np.frombuffer(raw, np.uint32, index_count, 16 + count * stride).reshape(-1, 3)
    positions = np.frombuffer(vertices[:, :12].tobytes(), np.float32).reshape(count, 3)
    _, weld = np.unique(np.round(positions, 4), axis=0, return_inverse=True)
    weld = weld.reshape(-1)
    parent = np.arange(weld.max() + 1)

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for a, b, c in weld[indices]:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    roots = np.array([find(i) for i in weld[indices[:, 0]]])
    ids, sizes = np.unique(roots, return_counts=True)
    keep = np.isin(roots, ids[sizes >= keep_share * sizes.max()])
    if keep.all():
        return 0
    kept = indices[keep]
    used, remap = np.unique(kept.reshape(-1), return_inverse=True)
    path.write_bytes(struct.pack("<4I", magic, version, len(used), kept.size) +
                     vertices[used].tobytes() + remap.astype(np.uint32).tobytes())
    return int((~keep).sum())


def to_png8(source: Path) -> Path:
    """An 8-bit PNG copy of a JPG (or deep PNG) via Windows imaging, since
    woodland_assets.read_png only reads 8-bit PNGs and there is no PIL here."""
    target = source.with_name(source.stem + "_8bit.png")
    if not target.exists():
        script = ("Add-Type -AssemblyName System.Drawing; "
                  f"$i=[System.Drawing.Image]::FromFile('{source}'); "
                  "$b=New-Object System.Drawing.Bitmap $i.Width,$i.Height,"
                  "([System.Drawing.Imaging.PixelFormat]::Format24bppRgb); "
                  "$g=[System.Drawing.Graphics]::FromImage($b); $g.DrawImage($i,0,0,$i.Width,$i.Height); "
                  f"$b.Save('{target}',[System.Drawing.Imaging.ImageFormat]::Png)")
        subprocess.run(["powershell", "-NoProfile", "-Command", script], check=True)
    return target


def cook(asset: str, name: str, lods, distances, masked: bool, list_only: bool, node=None):
    fbx, maps = fetch(asset)
    tool = find_prop_cook()
    if list_only:
        subprocess.run([str(tool), str(fbx), str(MESHES), "--list"], check=True)
        return
    for folder in (MESHES, TEXTURES, MATERIALS, PREFABS):
        folder.mkdir(parents=True, exist_ok=True)
    textures = {}
    if masked and "alpha" in maps:
        # RGBA colour with coverage-preserving mips (TextureCook) so the cards
        # do not thin out or bloat with distance.
        import numpy as np
        color = wa.read_png(to_png8(maps["diff"]))[..., :3]
        alpha = wa.read_png(to_png8(maps["alpha"]))
        alpha = alpha[..., 0] if alpha.ndim == 3 else alpha
        merged = CACHE / asset / f"{asset}_rgba.png"
        wa.write_png(merged, np.concatenate([color, alpha[..., None]], axis=2))
        textures["diff"] = wa.asset_path(wa.cook(merged, TEXTURES / f"{name}_diff.ktx2", "color",
                                                 alpha_cutoff=0.4))
    for tag, source in maps.items():
        if tag == "alpha" or tag in textures:
            continue
        target = TEXTURES / f"{name}_{tag}{source.suffix}"
        shutil.copyfile(source, target)
        textures[tag] = wa.asset_path(target)
    # Cliff pieces get a cool tint: those scans are warm sandstone, the
    # landscape's rock is grey. Everything else keeps its scanned colour.
    cliff = name.startswith(("RockFace", "Mountainside", "Cliff_Scan"))
    # The mossy rock scans come out as bright orange sandstone under the
    # landscape's lighting: darken and green them toward damp forest rock.
    tint = [0.74, 0.84, 0.98] if cliff else [0.42, 0.5, 0.4] if name.startswith("MossRock")         else [1.0, 1.0, 1.0]
    fields = dict(baseColor=tint,
                  baseColorTexture=textures.get("diff", ""), metallic=0, roughness=1.0,
                  normalScale=1.0, useVertexColors=False,
                  # Scans are open shells: seen from behind or above, a
                  # one-sided shell shows only stray back-facing shards.
                  doubleSided=True)
    if "nor_gl" in textures:
        fields["normalTexture"] = textures["nor_gl"]
    if "rough" in textures:
        # Roughness sits in green for the engine's metallic-roughness lookup;
        # a grey roughness map has it in every channel.
        fields["metallicRoughnessTexture"] = textures["rough"]
    if masked:
        fields.update(alphaMode="Mask", alphaCutoff=0.4, doubleSided=True, foliageTranslucency=0.8)
    material = wa.write_material(MATERIALS / f"{name}.material", **fields)
    nodes = () if node is None else (node,) if isinstance(node, int) else tuple(node)
    split = ["--split", "--only", ",".join(str(n) for n in nodes)] if nodes else []
    subprocess.run([str(tool), str(fbx), str(MESHES), "--name", name, *split,
                    "--lods", ",".join(str(t) for t in lods),
                    "--mesh-asset", wa.asset_path(MESHES), "--prefab-dir", str(PREFABS),
                    "--material", wa.engine_path(material)], check=True)
    base = name
    # PropCook --split numbers its outputs 01, 02, ... in the order kept.
    for name in ([f"{base}_{i + 1:02d}" for i in range(len(nodes))] if len(nodes) > 1 or
                 (nodes and (MESHES / f"{base}_01_LOD0.mesh").exists()) else [base]):
      if not masked:
        for lod in range(len(lods)):
            mesh = MESHES / f"{name}_LOD{lod}.mesh"
            removed = drop_fragments(mesh) if mesh.exists() else 0
            if removed:
                print(f"  {name} LOD{lod}: dropped {removed} loose triangles")
      prefab = PREFABS / f"{name}.prefab"
      if prefab.exists():
        # PropCook writes default LOD distances; set this asset's.
        text = prefab.read_text(encoding="utf-8")
        import re
        text = re.sub(r'Mesh\.gpuLod1Distance="[^"]*"', f'Mesh.gpuLod1Distance="{distances[0]}"', text)
        text = re.sub(r'Mesh\.gpuLod2Distance="[^"]*"', f'Mesh.gpuLod2Distance="{distances[1]}"', text)
        prefab.write_text(text, encoding="utf-8")
      print(f"{name}: cooked from Poly Haven '{asset}' (CC0)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", help="comma-separated asset ids")
    parser.add_argument("--list", action="store_true", help="list mesh nodes and sizes, cook nothing")
    args = parser.parse_args()
    wanted = set(args.only.split(",")) if args.only else None
    for asset, (name, lods, distances, masked, *node) in ASSETS.items():
        if wanted and asset not in wanted:
            continue
        print(f"{asset}:")
        cook(asset, name, lods, distances, masked, args.list, node[0] if node else None)


if __name__ == "__main__":
    main()
