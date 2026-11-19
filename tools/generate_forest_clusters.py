#!/usr/bin/env python3
"""Bake far-forest cluster impostors: a stand of trees rendered into one
view-baked impostor (Material.impostorBillboard), so distant forests draw a
tenth of the instances.

Sources are the real tree meshes: the oak pack's LOD2 parts with their
material textures (read from the prefabs), and the conifers rebuilt from
Tools/generate_conifers.py at LOD1. Each cluster scatters CLUSTER_TREES trees
(Poisson disc) over a CLUSTER_SIZE square with the same species mix and
scale range the landscape's forest generator uses, then bakes six side
views and a top view.

Writes (under Engine/Core/Assets):
  Models/Woodland/Clusters/Meshes/<Cluster>.mesh
  Models/Woodland/Clusters/Textures/<Cluster>_Impostor_{Color,Normal}.ktx2
  Models/Woodland/Clusters/Materials/<Cluster>.material
  Prefabs/Woodland/Clusters/<Cluster>.prefab

TerrainManager.farClusters entries reference these prefabs with a match text
("/Oaks/" or "/Conifers/") and the baked tree count; TerrainManagerFarObjects
swaps squares of far trees for them beyond farClusterDistance.

Usage: python Tools/generate_forest_clusters.py [--only Name]
"""
from __future__ import annotations

import argparse
import json
import math
import random
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402
import generate_conifers as gc  # noqa: E402

MODELS = wa.ASSETS / "Models" / "Woodland" / "Clusters"
MESHES = MODELS / "Meshes"
TEXTURES = MODELS / "Textures"
MATERIALS = MODELS / "Materials"
PREFABS = wa.ASSETS / "Prefabs" / "Woodland" / "Clusters"
SOURCES = wa.WORK / "clusters"
OAK_PREFABS = wa.ASSETS / "Prefabs" / "Woodland" / "Oaks"

CLUSTER_SIZE = 36.0
CLUSTER_TREES = 14
CUTOFF = 0.4
TEXTURE_MAX = 1024  # source textures are downsampled to this for baking

CLUSTERS = [
    # name, seed, family, species (weights)
    ("ForestCluster_Oak_01", 8101, "oak", {"Big_oak_tree_001": 2, "Large_oak_tree_001": 2,
                                           "Medium_oak_tree_001": 2, "Small_oak_tree_001": 1}),
    ("ForestCluster_Oak_02", 8203, "oak", {"Big_oak_tree_002": 2, "Large_oak_tree_002": 2,
                                           "Medium_oak_tree_002": 2, "Sapling_oak_tree_001": 1}),
    ("ForestCluster_Conifer_01", 8307, "conifer", {"Spruce_tall_01": 3, "Spruce_young_01": 2,
                                                   "Fir_subalpine_01": 2}),
    ("ForestCluster_Conifer_02", 8409, "conifer", {"Spruce_tall_01": 2, "Pine_scots_01": 3,
                                                   "Fir_subalpine_01": 1}),
]
MATCH = {"oak": "/Oaks/", "conifer": "/Conifers/"}
# Same as the source trees' leaf materials, so clusters match them.
FOLIAGE_VARIATION = {"oak": 0.7, "conifer": 0.55}

NS = "{urn:engine-components}"
_texture_cache: dict = {}


def load_texture(asset: str):
    if asset in _texture_cache:
        return _texture_cache[asset]
    path = wa.ASSETS / asset.removeprefix("Assets/")
    if path.suffix.lower() == ".ktx2":
        # Cooked foliage (Tools/cook_foliage_textures.py) keeps its PNG source.
        path = path.with_suffix(".png")
    image = wa.read_png(path)
    factor = 1
    while image.shape[0] // factor > TEXTURE_MAX:
        factor *= 2
    if factor > 1:
        # Alpha-weighted downsample keeps cutout colour clean.
        a = image[..., 3:4]
        rgb = wa.downsample(image[..., :3] * a, factor) / np.maximum(wa.downsample(a, factor), 1e-6)
        image = np.concatenate([rgb, wa.downsample(a, factor)], axis=2)
    image[..., :3] = wa.srgb_to_linear(image[..., :3])
    texture = wa.Texture(image)
    _texture_cache[asset] = texture
    return texture


def oak_surfaces(name: str, level: int = 2):
    """Surfaces of an oak prefab at a LOD level, with their materials."""
    root = ET.parse(OAK_PREFABS / f"{name}.prefab").getroot()
    surfaces = []
    for mesh_el in root.iter(NS + "Mesh"):
        parent = None
        for comps in root.iter("components"):
            if mesh_el in list(comps):
                parent = comps
        material_el = parent.find(NS + "Material") if parent is not None else None
        lods = [item.text or "" for item in mesh_el.findall("Mesh.lodMeshes/item")]
        mesh_asset = mesh_el.get("Mesh.file") if level == 0 else (lods[level - 1] if len(lods) >= level else "")
        if not mesh_asset or "Impostor" in mesh_asset:
            continue
        material = json.loads((wa.ROOT / material_el.get("Material.materialAsset")).read_text(encoding="utf-8"))
        p, n, uv, c, idx = wa.read_native_mesh(wa.ASSETS / mesh_asset.removeprefix("Assets/"))
        texture = load_texture(material["baseColorTexture"]) if material.get("baseColorTexture") else None
        masked = material.get("alphaMode") == "Mask"
        surfaces.append(wa.Surface(p, n, uv, c, idx, texture=texture,
                                   base_color=material.get("baseColor", [1, 1, 1])[:3],
                                   alpha_cutoff=material.get("alphaCutoff", 0.5) if masked else 0.0,
                                   use_vertex_colors=bool(material.get("useVertexColors", False))))
    return surfaces


_conifer_cache: dict = {}


def conifer_surfaces(name: str):
    if not _conifer_cache:
        rgb, alpha = gc.make_needle_atlas()
        bark_rgb, _ = gc.make_bark()
        _conifer_cache["needles"] = wa.Texture(np.concatenate([rgb, alpha[..., None]], axis=2))
        _conifer_cache["bark"] = wa.Texture(np.concatenate([bark_rgb, np.ones(bark_rgb.shape[:2] + (1,))], 2))
    species = next(s for s in gc.SPECIES if s.name == name)
    trunk, needles = gc.build_lod(gc.build_tree(species), 1)
    return [wa.Surface.from_builder(trunk, texture=_conifer_cache["bark"]),
            wa.Surface.from_builder(needles, texture=_conifer_cache["needles"], alpha_cutoff=gc.NEEDLE_CUTOFF)]


def poisson(rng, count, size, spacing):
    points = []
    attempts = 0
    while len(points) < count and attempts < 20000:
        attempts += 1
        p = np.array([rng.uniform(-size / 2, size / 2), rng.uniform(-size / 2, size / 2)])
        if all(np.linalg.norm(p - q) >= spacing for q in points):
            points.append(p)
        if attempts % 2000 == 0:
            spacing *= 0.92
    return points


def build_cluster(seed, family, species):
    rng = random.Random(seed)
    names = [name for name, weight in species.items() for _ in range(weight)]
    sources = {}
    surfaces = []
    spacing = 8.0 if family == "oak" else 6.0
    for x, z in poisson(rng, CLUSTER_TREES, CLUSTER_SIZE, spacing):
        name = rng.choice(names)
        if name not in sources:
            sources[name] = oak_surfaces(name) if family == "oak" else conifer_surfaces(name)
        yaw = rng.random() * math.tau
        scale = 0.8 + 0.45 * rng.random()
        c, s = math.cos(yaw), math.sin(yaw)
        rotation = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]]) * scale
        for surface in sources[name]:
            surfaces.append(surface.transformed(rotation, np.array([x, -0.08, z])))
    return surfaces


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only")
    args = parser.parse_args()
    entries = []
    for name, seed, family, species in CLUSTERS:
        if args.only and name != args.only:
            continue
        print(f"{name}: baking {CLUSTER_TREES} {family} trees ...", flush=True)
        surfaces = build_cluster(seed, family, species)
        mesh_path = MESHES / f"{name}.mesh"
        color_png, normal_png, tris = wa.bake_impostor(surfaces, name, SOURCES, mesh_path,
                                                       atlas_size=1024, supersample=2)
        color = wa.cook(color_png, TEXTURES / f"{name}_Impostor_Color.ktx2", "color", alpha_cutoff=CUTOFF)
        normal = wa.cook(normal_png, TEXTURES / f"{name}_Impostor_Normal.ktx2", "normal")
        material = wa.write_material(
            MATERIALS / f"{name}.material", baseColorTexture=wa.asset_path(color),
            normalTexture=wa.asset_path(normal), useVertexColors=False, alphaMode="Mask",
            alphaCutoff=CUTOFF, doubleSided=True, roughness=0.88, normalScale=0.5, alphaEdgeFade=1.0,
            impostorBillboard=True, foliageColorVariation=FOLIAGE_VARIATION[family],
            # Oak leaves and impostors carry this tint in their materials.
            baseColor=[0.86, 0.84, 0.77] if family == "oak" else [1, 1, 1])
        prefab = wa.write_prefab(PREFABS / f"{name}.prefab", name, [
            dict(mesh=wa.asset_path(mesh_path), material=wa.engine_path(material), lod1=100000, lod2=100000)])
        entries.append({"asset": wa.asset_path(prefab), "match": MATCH[family], "trees": CLUSTER_TREES})
        print(f"  {tris} triangles")
    print("TerrainManager.farClusters:")
    print(json.dumps(entries, indent=2))


if __name__ == "__main__":
    main()
