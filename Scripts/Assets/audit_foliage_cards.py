"""Estimate base-mip empty leaf-card area; this is not GPU overdraw or timing.

Uses deterministic uniform triangle quadrature, native UVs, repeat addressing
and nearest base-mip alpha. Requires numpy, scipy and Pillow (as canopy tools do).
Mesh-local surface areas exclude prefab transforms and are not projected areas.
"""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

import numpy as np
from build_canopy_lod import ROOT, load_part, resolve


def coverage(triangles, texture, cutoff, resolution):
    """Return accepted sample fraction per triangle using equal-area cells."""
    if resolution < 1:
        raise ValueError('Sample resolution must be positive')
    weights = []
    # Each row has upward and downward equal-area subtriangles.
    for i in range(resolution):
        for j in range(resolution - i):
            weights.append(((i + 1/3) / resolution, (j + 1/3) / resolution))
            if i + j < resolution - 1:
                weights.append(((i + 2/3) / resolution, (j + 2/3) / resolution))
    weights = np.asarray(weights)
    bary = np.column_stack((weights, 1 - weights.sum(axis=1)))
    fractions = []
    height, width = texture.shape[:2]
    for start in range(0, len(triangles), 256):
        uv = np.einsum('sv,tvc->tsc', bary, triangles[start:start+256, :, 6:8])
        alpha = texture[np.floor(uv[..., 1] * height).astype(np.int64) % height,
                        np.floor(uv[..., 0] * width).astype(np.int64) % width, 3]
        fractions.extend((alpha >= cutoff * 255).mean(axis=1).tolist())
    return np.asarray(fractions)


def audit(prefab, resolution):
    parts = []
    inputs = {prefab}
    for group in ET.parse(prefab).getroot().iter('components'):
        mesh = next((e for e in group if e.tag.endswith('}Mesh')), None)
        material = next((e for e in group if e.tag.endswith('}Material')), None)
        if mesh is None or material is None:
            continue
        material_path = resolve(material.get('Material.materialAsset'))
        data = json.loads(material_path.read_text())
        if data.get('alphaMode') != 'Mask':
            continue
        mesh_path = resolve(mesh.get('Mesh.file'))
        texture_path = resolve(data['baseColorTexture'])
        inputs.update((mesh_path, material_path, texture_path))
        part = load_part(mesh_path, material_path)
        triangles = part['triangles']
        area = np.linalg.norm(np.cross(triangles[:, 1, :3] - triangles[:, 0, :3],
                                       triangles[:, 2, :3] - triangles[:, 0, :3]), axis=1) / 2
        total = float(area.sum())
        if total <= 0:
            raise ValueError(f'No nondegenerate surface in {mesh_path}')
        estimates = {}
        for n in (resolution, resolution * 2):
            accepted = coverage(triangles, part['texture'], part['cutoff'], n)
            estimates[str(n)] = float(np.dot(area, 1 - accepted) / total)
        parts.append(dict(mesh=part['source'], triangles=len(triangles),
                          mesh_local_surface_area=total, alpha_cutoff=part['cutoff'],
                          empty_area_fraction_by_resolution=estimates,
                          convergence_absolute_delta=abs(estimates[str(resolution)] - estimates[str(resolution*2)])))
    return dict(prefab=prefab.relative_to(ROOT).as_posix(), parts=parts), inputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resolution', type=int, default=8)
    args = parser.parse_args()
    if args.resolution < 1:
        parser.error('--resolution must be positive')
    prefabs = sorted((ROOT / 'Engine/Core/Assets/Prefabs/Woodland/Oaks').glob('*.prefab'))
    # Fingerprint before measurement, then verify every referenced input afterward.
    paths = set(prefabs)
    for prefab in prefabs:
        for group in ET.parse(prefab).getroot().iter('components'):
            for node in group:
                for key in ('Mesh.file', 'Material.materialAsset'):
                    if node.get(key):
                        paths.add(resolve(node.get(key)))
    for path in list(paths):
        if path.suffix == '.material':
            data = json.loads(path.read_text())
            if data.get('baseColorTexture'):
                paths.add(resolve(data['baseColorTexture']))
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {p: digest(p) for p in paths}
    rows = []
    for prefab in prefabs:
        row, used = audit(prefab, args.resolution)
        if not used <= paths:
            raise ValueError('Unfingerprinted input')
        rows.append(row)
    if any(digest(p) != value for p, value in before.items()):
        raise ValueError('Audit inputs changed')
    result = dict(schema=1, method=__doc__, resolution=args.resolution,
                  inputs_sha256={p.relative_to(ROOT).as_posix(): h for p, h in sorted(before.items())},
                  inputs_unchanged=True, prefabs=rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'Audited {len(rows)} prefabs: {args.output}')


if __name__ == '__main__':
    main()
