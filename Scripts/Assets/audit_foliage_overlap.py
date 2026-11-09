"""Software projected leaf overlap screening, not GPU fragment/depth cost.

Counts every covered triangle at pixel centres before any depth rejection.
Uses two-sided rasterization, top-left edge ownership, nearest base-mip alpha,
native UV0/repeat addressing, and frozen mesh-local geometry. Opaque bark,
forest placements, wind, filtering/mips and renderer shadow cascades are excluded.
Requires the same numpy/scipy/Pillow dependencies as build_canopy_lod.py.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET

import numpy as np
from build_canopy_lod import ROOT, load_part, resolve


def raster_counts(triangles, texture, cutoff, size, perspective=True, accepted_visitor=None,
                  fragment_visitor=None):
    """Input x/y are screen positions, z positive camera depth, UV at 6:8."""
    cards = np.zeros((size, size), dtype=np.uint32)
    leaves = np.zeros_like(cards)
    if not np.isfinite(triangles).all() or np.any(triangles[:, :, 2] <= 0):
        raise ValueError('Invalid projection or geometry crosses camera plane')
    height, width = texture.shape[:2]
    for triangle_index, original in enumerate(triangles):
        tri = original.copy()
        a, b, c = tri[:, :2]
        ab, ac = b-a, c-a
        signed = float(ab[0]*ac[1]-ab[1]*ac[0])
        if abs(signed) < 1e-10:
            continue
        if signed < 0:
            tri[[1, 2]] = tri[[2, 1]]
        xy = tri[:, :2]
        low = np.maximum(np.ceil(xy.min(0)-.5).astype(int), 0)
        high = np.minimum(np.floor(xy.max(0)-.5).astype(int), size-1)
        if np.any(high < low):
            continue
        x, y = np.meshgrid(np.arange(low[0], high[0]+1)+.5,
                           np.arange(low[1], high[1]+1)+.5)
        edges = []
        inside = np.ones(x.shape, dtype=bool)
        for first, second in ((1, 2), (2, 0), (0, 1)):
            a, b = xy[first], xy[second]
            dx, dy = b-a
            edge = dx*(y-a[1])-dy*(x-a[0])
            top_left = dy < 0 or (dy == 0 and dx > 0)
            inside &= (edge > 0) | ((edge == 0) & top_left)
            edges.append(edge)
        if not inside.any():
            continue
        weights = np.stack(edges, axis=-1)[inside] / abs(signed)
        if perspective:
            weights /= tri[:, 2]
            weights /= weights.sum(axis=-1, keepdims=True)
        uv = weights @ tri[:, 6:8]
        alpha = texture[np.floor(uv[:, 1]*height).astype(np.int64) % height,
                        np.floor(uv[:, 0]*width).astype(np.int64) % width, 3]
        yy, xx = np.nonzero(inside)
        yy += low[1]; xx += low[0]
        cards[yy, xx] += 1
        accepted = alpha >= cutoff*255
        leaves[yy[accepted], xx[accepted]] += 1
        if accepted_visitor is not None:
            accepted_visitor(triangle_index, yy[accepted]*size+xx[accepted])
        if fragment_visitor is not None:
            fragment_visitor(triangle_index, yy*size+xx, weights @ tri[:, 2], accepted)
    return cards, leaves


def summarize(cards, leaves):
    raw = int(cards.sum())
    accepted = int(leaves.sum())
    union = int(np.count_nonzero(leaves))
    if raw < accepted or np.any(leaves > cards):
        raise ValueError('Accepted coverage exceeds card coverage')
    if not union:
        raise ValueError('Empty leaf projection; increase resolution or check view')
    return dict(card_triangle_samples=raw, alpha_accepted_samples=accepted,
                empty_alpha_samples=raw-accepted, leaf_union_pixels=union,
                excess_accepted_layers=accepted-union,
                accepted_layers_per_leaf_pixel=accepted/union,
                empty_fraction_of_card_samples=(raw-accepted)/raw,
                multiply_covered_leaf_fraction=float(np.count_nonzero(leaves > 1)/union),
                maximum_accepted_layers=int(leaves.max()))


def load_prefab(path):
    raw = path.read_bytes()
    tree = ET.ElementTree(ET.fromstring(raw))
    # Reject transforms rather than silently comparing incompatible mesh spaces.
    for transform in tree.getroot().iter('transform'):
        for name, expected in (('position', [0, 0, 0]), ('rotation', [0, 0, 0]), ('scale', [1, 1, 1])):
            node = transform.find('Transform.'+name)
            if node is not None and [float(n.text) for n in node] != expected:
                raise ValueError(f'Nonidentity prefab transform: {path}')
    parts = []
    inputs = {path: hashlib.sha256(raw).hexdigest()}
    for group in tree.getroot().iter('components'):
        mesh = next((e for e in group if e.tag.endswith('}Mesh')), None)
        material = next((e for e in group if e.tag.endswith('}Material')), None)
        if mesh is None or material is None:
            continue
        mp = resolve(material.get('Material.materialAsset'))
        raw_material = mp.read_bytes()
        data = json.loads(raw_material)
        material_hash = hashlib.sha256(raw_material).hexdigest()
        if mp in inputs and inputs[mp] != material_hash:
            raise ValueError(f'Material changed during discovery: {mp}')
        inputs[mp] = material_hash
        if data.get('alphaMode') != 'Mask':
            continue
        # This screen implements the supported native leaf materials only.
        if not data.get('doubleSided') or data.get('useVertexColors') or data.get('baseColorUvSet', 0) != 0:
            raise ValueError(f'Unsupported leaf material: {mp}')
        for key in ('baseColorAlpha', 'baseColorTextureTintAlpha'):
            if data.get(key, 1) != 1:
                raise ValueError(f'Unsupported alpha multiplier: {mp}')
        mesh_path = resolve(mesh.get('Mesh.file'))
        for dependency in (mesh_path, resolve(data['baseColorTexture'])):
            value = hashlib.sha256(dependency.read_bytes()).hexdigest()
            if dependency in inputs and inputs[dependency] != value:
                raise ValueError(f'Input changed during discovery: {dependency}')
            inputs[dependency] = value
        parts.append((mesh_path, mp))
    if not parts:
        raise ValueError(f'No masked leaves: {path}')
    return parts, inputs


def project(part, target, radius, azimuth, elevation, size, orthographic):
    az, el = np.radians([azimuth, elevation])
    direction = np.array([math.cos(el)*math.sin(az), math.sin(el), math.cos(el)*math.cos(az)])
    eye = target + radius*3*direction
    forward = -direction
    right = np.cross(forward, [0., 1., 0.]); right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    tri = part['triangles'].copy()
    camera = (tri[:, :, :3]-eye) @ np.array([right, up, forward]).T
    if orthographic:
        xy = camera[:, :, :2] * (size/(radius*2.2))
    else:
        xy = camera[:, :, :2]/camera[:, :, 2:] * (size/(2*math.tan(math.radians(50)/2)))
    tri[:, :, :2] = xy * [1, -1]+size/2
    tri[:, :, 2] = camera[:, :, 2]
    return tri


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--sizes', nargs='+', type=int, default=[128, 256])
    args = parser.parse_args()
    if any(s < 16 or s > 2048 for s in args.sizes) or len(set(args.sizes)) != len(args.sizes):
        parser.error('Use distinct sizes from 16 to 2048')
    prefabs = sorted((ROOT/'Engine/Core/Assets/Prefabs/Woodland/Oaks').glob('Large_oak_tree_*_Canopy.prefab'))
    if len(prefabs) != 4:
        raise ValueError('Expected four installed canopy prefabs')
    sources = {p: load_prefab(p) for p in prefabs}
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {}
    for _, inputs in sources.values():
        for path, value in inputs.items():
            if path in before and before[path] != value:
                raise ValueError(f'Input changed during discovery: {path}')
            before[path] = value
    rows = []
    views = [('ground', 17, -12, False), ('elevated', 107, 35, False),
             ('light-low', 37, 25, True), ('light-high', 137, 65, True)]
    for prefab, (references, _) in sources.items():
        parts = [load_part(mesh, material) for mesh, material in references]
        xyz = np.concatenate([p['triangles'][:, :, :3].reshape(-1, 3) for p in parts])
        low, high = xyz.min(0), xyz.max(0)
        target = (low+high)/2; radius = float(np.linalg.norm(high-low)/2)
        for size in args.sizes:
            for view, az, el, ortho in views:
                cards = np.zeros((size, size), dtype=np.uint32)
                leaves = np.zeros_like(cards)
                for part in parts:
                    a, b = raster_counts(project(part, target, radius, az, el, size, ortho),
                                         part['texture'], part['cutoff'], size, not ortho)
                    cards += a; leaves += b
                rows.append(dict(prefab=prefab.relative_to(ROOT).as_posix(), size=size, view=view,
                                 azimuth=az, elevation=el, orthographic=ortho,
                                 target=target.tolist(), radius=radius, **summarize(cards, leaves)))
        print(f'Screened {prefab.name}', flush=True)
    if any(digest(p) != h for p, h in before.items()):
        raise ValueError('Inputs changed during projection screen')
    result = dict(schema=1, method=__doc__, inputs_unchanged=True,
                  inputs_sha256={p.relative_to(ROOT).as_posix(): h for p, h in sorted(before.items())}, cases=rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(args.output)


if __name__ == '__main__':
    main()
