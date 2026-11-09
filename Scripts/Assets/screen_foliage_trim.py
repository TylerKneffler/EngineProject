"""Screen a global transparent-border UV crop before generating foliage meshes.

This CPU screen samples native triangle surface points removed by a proposed
base-mip crop, then checks bilinear/repeat alpha across the full native mip chain.
A visible removed sample rejects this strict candidate, not every tighter-card
strategy. It does not measure projected forest coverage, wind or GPU performance.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from build_canopy_lod import ROOT, load_part
from audit_foliage_overlap import load_prefab


def mip_chain(alpha):
    """Match Texture.cpp's RGBA8 alpha reduction, including odd-size handling."""
    alpha = np.asarray(alpha, dtype=np.uint8)
    if alpha.ndim != 2 or 0 in alpha.shape:
        raise ValueError('Nonempty two-dimensional alpha required')
    levels = [alpha]
    while alpha.shape != (1, 1):
        h, w = alpha.shape
        y = np.arange(max(1, h//2))*2
        x = np.arange(max(1, w//2))*2
        total = np.zeros((len(y), len(x)), dtype=np.float32)
        for dy in range(2):
            for dx in range(2):
                total += alpha[np.ix_(np.minimum(y+dy, h-1), np.minimum(x+dx, w-1))].astype(np.float32)/np.float32(255)
        alpha = np.clip(total*np.float32(.25)*np.float32(255)+np.float32(.5), 0, 255).astype(np.uint8)
        levels.append(alpha)
    return levels


def bilinear(alpha, uv):
    h, w = alpha.shape
    coordinates = np.asarray(uv, dtype=float)*[w, h]-.5
    low = np.floor(coordinates).astype(np.int64)
    f = coordinates-low
    x, y = low[:, 0], low[:, 1]
    a = alpha[y % h, x % w].astype(float)
    b = alpha[y % h, (x+1) % w].astype(float)
    c = alpha[(y+1) % h, x % w].astype(float)
    d = alpha[(y+1) % h, (x+1) % w].astype(float)
    return ((a*(1-f[:, 0])+b*f[:, 0])*(1-f[:, 1])+
            (c*(1-f[:, 0])+d*f[:, 0])*f[:, 1])/255


def crop_bounds(alpha, cutoff):
    """Conservative base-mip bilinear support box with repeat edge guarding."""
    h, w = alpha.shape
    support = alpha.astype(float) >= cutoff*255
    # A passing bilinear sample needs at least one passing texel. Expand by a
    # full texel cell conservatively; wrapped edge support protects both seams.
    guarded = np.zeros_like(support)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            guarded |= np.roll(support, (dy, dx), axis=(0, 1))
    yy, xx = np.nonzero(guarded)
    if not len(xx):
        raise ValueError('No base-mip alpha support')
    return np.array([xx.min()/w, yy.min()/h, (xx.max()+1)/w, (yy.max()+1)/h])


def quadrature(n):
    if n < 1:
        raise ValueError('Positive sample resolution required')
    points = []
    for i in range(n):
        for j in range(n-i):
            points.append(((i+1/3)/n, (j+1/3)/n))
            if i+j < n-1:
                points.append(((i+2/3)/n, (j+2/3)/n))
    xy = np.asarray(points)
    return np.column_stack((xy, 1-xy.sum(1)))


def screen(triangles, levels, cutoff, crop, resolution):
    if not np.isfinite(triangles).all() or not np.isfinite(crop).all() or not 0 <= cutoff <= 1:
        raise ValueError('Invalid geometry, crop or alpha cutoff')
    bary = quadrature(resolution)
    area = np.linalg.norm(np.cross(triangles[:, 1, :3]-triangles[:, 0, :3],
                                   triangles[:, 2, :3]-triangles[:, 0, :3]), axis=1)/2
    total_area = float(area.sum())
    if total_area <= 0:
        raise ValueError('No nondegenerate surface')
    removed_area = 0.
    lost = [0. for _ in levels]
    witnesses = [None for _ in levels]
    for start in range(0, len(triangles), 128):
        batch = triangles[start:start+128]
        uv = np.einsum('sv,tvc->tsc', bary, batch[:, :, 6:8])
        removed = ((uv[..., 0] < crop[0]) | (uv[..., 0] > crop[2]) |
                   (uv[..., 1] < crop[1]) | (uv[..., 1] > crop[3]))
        ti, si = np.nonzero(removed)
        weights = area[start+ti]/len(bary)
        removed_area += float(weights.sum())
        if not len(ti):
            continue
        for level, alpha in enumerate(levels):
            values = bilinear(alpha, uv[removed])
            visible = values >= cutoff
            lost[level] += float(weights[visible].sum())
            if visible.any() and witnesses[level] is None:
                index = int(np.flatnonzero(visible)[0])
                triangle = int(start+ti[index])
                point = bary[si[index]]
                witnesses[level] = dict(triangle=triangle, barycentric=point.tolist(),
                                        uv=uv[removed][index].tolist(),
                                        mesh_local_position=(point@triangles[triangle, :, :3]).tolist(),
                                        sampled_alpha=float(values[index]))
    return dict(resolution=resolution, samples_per_triangle=len(bary),
                removed_surface_fraction=removed_area/total_area,
                rejected=any(w is not None for w in witnesses),
                levels=[dict(mip=i, width=a.shape[1], height=a.shape[0],
                             lost_surface_fraction=lost[i]/total_area, witness=witnesses[i])
                        for i, a in enumerate(levels)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resolutions', nargs='+', type=int, default=[16, 32])
    args = parser.parse_args()
    if any(n < 1 or n > 128 for n in args.resolutions):
        parser.error('Sample resolution must be 1..128')
    prefabs = sorted((ROOT/'Engine/Core/Assets/Prefabs/Woodland/Oaks').glob('Large_oak_tree_*_Canopy.prefab'))
    if len(prefabs) != 4:
        raise ValueError('Expected four installed canopies')
    sources = {p: load_prefab(p) for p in prefabs}
    before = {}
    for _, inputs in sources.values():
        for path, value in inputs.items():
            if path in before and before[path] != value:
                raise ValueError('Changed input during discovery')
            before[path] = value
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    for path in (Path(__file__).resolve(), ROOT/'Engine/Core/Compoonents/Materials/Texture.cpp'):
        before[path] = digest(path)
    rows = []
    for prefab, (references, _) in sources.items():
        for mesh, material in references:
            part = load_part(mesh, material)
            alpha = part['texture'][:, :, 3]
            levels = mip_chain(alpha)
            crop = crop_bounds(alpha, part['cutoff'])
            rows.append(dict(prefab=prefab.relative_to(ROOT).as_posix(), mesh=part['source'],
                             alpha_cutoff=part['cutoff'], proposed_uv_bounds=crop.tolist(),
                             texture_rectangle_area_reduction=1-(crop[2]-crop[0])*(crop[3]-crop[1]),
                             results=[screen(part['triangles'], levels, part['cutoff'], crop, n)
                                      for n in args.resolutions]))
        print(f'Screened {prefab.name}', flush=True)
    if any(digest(p) != value for p, value in before.items()):
        raise ValueError('Screen inputs changed')
    if args.output.resolve() in {p.resolve() for p in before}:
        raise ValueError('Output must not replace a screen input')
    output = dict(schema=1, method=__doc__, inputs_unchanged=True, approved_for_installation=False,
                  inputs_sha256={p.relative_to(ROOT).as_posix(): h for p, h in sorted(before.items())},
                  candidates=rows, any_candidate_rejected=any(r['rejected'] for row in rows for r in row['results']))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2)+'\n')
    print(args.output)


if __name__ == '__main__':
    main()
