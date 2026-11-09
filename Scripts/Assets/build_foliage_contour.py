"""Stage filtered-alpha UV contour candidates against installed canopy meshes.

Conservative cell bounds cover mathematical bilinear/trilinear sampling across
the native mip chain. Anisotropy, GPU filter precision, nonlinear vertex shading,
wind and actual rendered coverage/cost still require renderer acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

import numpy as np
from scipy.spatial import ConvexHull
from audit_foliage_overlap import load_prefab
from build_canopy_lod import ROOT, load_part, resolve
from build_foliage_interior_trim import trim
from screen_foliage_trim import mip_chain


def grouped_max(values, groups, count, axis):
    unique, starts = np.unique(groups, return_index=True)
    shape = list(values.shape)
    shape[axis] = count
    result = np.full(shape, -np.inf)
    reduced = np.maximum.reduceat(values, starts, axis=axis)
    if axis == 1:
        result[:, unique] = reduced
    else:
        result[unique] = reduced
    return result


def cell_upper_bounds(alpha, size):
    """Upper-bound bilinear alpha everywhere in each uniform UV cell.

    Along X, maxima occur at source texel centres or cell endpoints. Along Y,
    interpolating those row maxima gives an upper bound on the maximum of the
    interpolated rows (max is convex). Include native Y centres and endpoints.
    """
    if size < 1 or alpha.ndim != 2 or 0 in alpha.shape:
        raise ValueError('Positive grid size and nonempty alpha required')
    h, w = alpha.shape
    source = alpha.astype(float)/255
    groups_x = np.floor((np.arange(w)+.5)*size/w).astype(int)
    rows = grouped_max(source, groups_x, size, 1)
    x = np.arange(size+1)*w/size-.5
    low = np.floor(x).astype(int)
    fraction = x-low
    endpoints = source[:, low % w]*(1-fraction)+source[:, (low+1) % w]*fraction
    rows = np.maximum(rows, np.maximum(endpoints[:, :-1], endpoints[:, 1:]))
    groups_y = np.floor((np.arange(h)+.5)*size/h).astype(int)
    result = grouped_max(rows, groups_y, size, 0)
    y = np.arange(size+1)*h/size-.5
    low = np.floor(y).astype(int)
    fraction = (y-low)[:, None]
    endpoints = rows[low % h]*(1-fraction)+rows[(low+1) % h]*fraction
    return np.maximum(result, np.maximum(endpoints[:-1], endpoints[1:]))


def support_grid(levels, cutoff, size):
    if not np.isfinite(cutoff) or not 0 <= cutoff <= 1:
        raise ValueError('Invalid alpha cutoff')
    grid = np.zeros((size, size), dtype=bool)
    for level in levels:
        if level.max()/255 < cutoff:
            continue
        grid |= cell_upper_bounds(level, size) >= cutoff-1e-12
    return grid


def clip(bary, uv, a, b):
    """Clip a barycentric polygon against the left half-plane of UV edge a/b."""
    result = []
    edge = b-a
    def distance(point):
        delta = point@uv-a
        return edge[0]*delta[1]-edge[1]*delta[0]
    for first, second in zip(bary, np.roll(bary, -1, axis=0)):
        d0, d1 = distance(first), distance(second)
        inside0, inside1 = d0 >= 0, d1 >= 0
        if inside0:
            result.append(first)
        if inside0 != inside1:
            result.append(first+(second-first)*(d0/(d0-d1)))
    clean = []
    for point in result:
        if not clean or np.max(np.abs(point-clean[-1])) > 1e-12:
            clean.append(point)
    if len(clean)>1 and np.max(np.abs(clean[0]-clean[-1])) <= 1e-12:
        clean.pop()
    return np.asarray(clean).reshape(-1, 3)


def triangles_from_polygon(polygon):
    return [np.array([polygon[0], polygon[i], polygon[i+1]]) for i in range(1, len(polygon)-1)]


def fraction(polygon):
    return sum(abs(float(np.linalg.det(triangle))) for triangle in triangles_from_polygon(polygon))


def contour_plan(uv, grid, mode):
    if not np.isfinite(uv).all() or np.any(uv < 0) or np.any(uv > 1):
        raise ValueError('Finite UVs within one repeat tile required')
    if mode not in ('single', 'triangle', 'hull'):
        raise ValueError('Unknown contour mode')
    determinant = float(np.linalg.det(np.column_stack((uv, np.ones(3)))))
    if abs(determinant) < 1e-14:
        return [np.eye(3)], 0., False
    size = len(grid)
    low = np.maximum(np.floor(uv.min(0)*size).astype(int), 0)
    high = np.minimum(np.floor(uv.max(0)*size).astype(int), size-1)
    yy, xx = np.nonzero(grid[low[1]:high[1]+1, low[0]:high[0]+1])
    xx += low[0]; yy += low[1]
    centres = np.column_stack((xx+.5, yy+.5))/size
    triangle = uv if determinant > 0 else uv[::-1]
    selected = np.ones(len(xx), dtype=bool)
    # Separating-axis test: cell axes are covered by the bounding-box query;
    # each triangle edge must have at least one cell corner on its inside.
    for a, b in zip(triangle, np.roll(triangle, -1, axis=0)):
        edge = b-a
        delta = centres-a
        maximum = edge[0]*delta[:, 1]-edge[1]*delta[:, 0]+np.abs(edge).sum()/(2*size)
        selected &= maximum >= -1e-14
    xx, yy = xx[selected], yy[selected]
    if not len(xx):
        return [], 1., True
    corners = np.concatenate([np.column_stack((xx+dx, yy+dy))/size for dx, dy in ((0, 0), (1, 0), (1, 1), (0, 1))])
    hull = corners[ConvexHull(corners).vertices]
    polygon = np.eye(3)
    if mode in ('single', 'triangle'):
        best_area = 1.
        for a, b in zip(hull, np.roll(hull, -1, axis=0)):
            candidate = clip(np.eye(3), uv, a, b)
            if mode == 'triangle' and len(candidate) > 3:
                continue
            area = fraction(candidate)
            if area < best_area:
                polygon, best_area = candidate, area
    else:
        for a, b in zip(hull, np.roll(hull, -1, axis=0)):
            polygon = clip(polygon, uv, a, b)
            if len(polygon) < 3:
                break
    area = fraction(polygon)
    if not -1e-8 <= area <= 1+1e-8:
        raise ValueError('Invalid barycentric contour area')
    if area >= 1-1e-9:
        return [np.eye(3)], 0., False
    return triangles_from_polygon(polygon), max(0., 1-area), True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--sizes', nargs='+', type=int, default=[256, 512])
    args = parser.parse_args()
    if any(s < 16 or s > 2048 for s in args.sizes) or len(set(args.sizes)) != len(args.sizes):
        parser.error('Use distinct grid sizes from 16 to 2048')
    destination = args.output.resolve()
    destination.relative_to(ROOT/'.local')
    if destination.exists() and any(destination.iterdir()):
        parser.error('Use a fresh output directory')
    prefabs = sorted((ROOT/'Engine/Core/Assets/Prefabs/Woodland/Oaks').glob('Large_oak_tree_*_Canopy.prefab'))
    if len(prefabs) != 4:
        raise ValueError('Expected four installed canopies')
    sources = {p: load_prefab(p) for p in prefabs}
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {}
    for _, inputs in sources.values():
        for p, h in inputs.items():
            if p in before and before[p] != h:
                raise ValueError('Input changed during discovery')
            before[p] = h
    for name in ('build_foliage_contour.py', 'build_foliage_interior_trim.py', 'screen_foliage_trim.py', 'build_canopy_lod.py', 'audit_foliage_overlap.py', 'audit_oak_lods.py'):
        p = Path(__file__).resolve().parent/name
        before[p] = digest(p)
    for name in ('Compoonents/Materials/Texture.cpp', 'Model/MeshData.h', 'Shaders/GrassWind.hlsli'):
        p = ROOT/'Engine/Core'/name
        before[p] = digest(p)
    destination.mkdir(parents=True)
    grids, plans, rows = {}, {}, []
    for prefab, (references, _) in sources.items():
        name = prefab.stem.removesuffix('_Canopy')
        for mesh, material in references:
            part = load_part(mesh, material)
            alpha = part['texture'][:, :, 3]
            identity = (hashlib.sha256(alpha.tobytes()).hexdigest(), part['cutoff'])
            for size in args.sizes:
                key = identity+(size,)
                if key not in grids:
                    grids[key] = support_grid(mip_chain(alpha), part['cutoff'], size)
                grid = grids[key]
                for mode in ('triangle', 'single', 'hull'):
                    def planner(uv):
                        k = key+(mode, uv.tobytes())
                        if k not in plans:
                            plans[k] = contour_plan(uv, grid, mode)
                        return plans[k]
                    candidate, metrics = trim(part['triangles'], None, 0, planner=planner)
                    folder = destination/f'{mode}-{size}'/name
                    folder.mkdir(parents=True)
                    path = folder/(name+'_Contour.mesh')
                    vertices = candidate.reshape(-1, 66)
                    path.write_bytes(struct.pack('<4I', 0x4853454D, 7, len(vertices), 0)+vertices.astype('<f4').tobytes())
                    tree = ET.parse(prefab)
                    replaced = 0
                    for node in tree.getroot().iter():
                        if node.tag.endswith('}Mesh') and resolve(node.get('Mesh.file')) == mesh:
                            node.set('Mesh.file', path.relative_to(ROOT).as_posix()); replaced += 1
                    if replaced != 1:
                        raise ValueError('Expected one leaf replacement')
                    ET.indent(tree)
                    tree.write(folder/(name+'_CanopyCandidate.prefab'), encoding='utf-8', xml_declaration=True)
                    rows.append(dict(name=name, mode=mode, grid_size=size, baseline_prefab=prefab.relative_to(ROOT).as_posix(),
                                     source=part['source'], supported_uv_cell_fraction=float(grid.mean()), **metrics))
                    print(name, mode, size, metrics, flush=True)
    if any(digest(p) != h for p, h in before.items()):
        raise ValueError('Contour inputs changed')
    artifacts = {p.relative_to(destination).as_posix(): digest(p) for p in sorted(destination.rglob('*')) if p.is_file()}
    report = dict(schema=1, method=__doc__, complete=True, approved_for_installation=False, inputs_unchanged=True,
                  inputs_sha256={p.relative_to(ROOT).as_posix(): h for p, h in sorted(before.items())},
                  artifacts_sha256=artifacts, candidates=rows)
    (destination/'report.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
