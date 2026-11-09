"""Stage mip-aware adaptive interior-cut candidates; never install authored assets.

The UV-box support bound protects bilinear/trilinear alpha across all generated
mips. It does not establish anisotropic footprint, wind, shaded-image or GPU cost
equivalence. New vertices may change nonlinear vertex shading despite retaining
interpolated source attributes. Renderer acceptance is required separately.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

import numpy as np
from audit_foliage_overlap import load_prefab
from build_canopy_lod import ROOT, load_part, resolve
from screen_foliage_trim import mip_chain


class Support:
    def __init__(self, levels, cutoff):
        if not np.isfinite(cutoff) or not 0 <= cutoff <= 1:
            raise ValueError('Invalid alpha cutoff')
        self.tables = []
        for alpha in levels:
            occupied = alpha.astype(float) >= cutoff*255
            if not occupied.any():
                continue
            # Native canopy UVs lie in one repeat tile. Pad both boundaries so
            # footprint boxes include wrapped neighbours, not clamped samples.
            padded = np.pad(occupied, 1, mode='wrap').astype(np.int32)
            table = np.pad(padded.cumsum(0, dtype=np.int32).cumsum(1, dtype=np.int32), ((1, 0), (1, 0)))
            self.tables.append((alpha.shape, table))

    def possible(self, uv):
        if not np.isfinite(uv).all() or np.any(uv < 0) or np.any(uv > 1):
            raise ValueError('Only finite native UVs inside one repeat tile supported')
        low, high = uv.min(0), uv.max(0)
        for (h, w), table in reversed(self.tables):
            # Pixel-centre sampling touches floor(uv*size-.5) and its neighbour.
            x0, y0 = np.floor(low*[w, h]-.5).astype(int)+1
            x1, y1 = np.floor(high*[w, h]-.5).astype(int)+3
            count = table[y1, x1]-table[y0, x1]-table[y1, x0]+table[y0, x0]
            if count > 0:
                return True
        return False


def split(bary):
    a, b, c = bary
    ab, bc, ca = (a+b)/2, (b+c)/2, (c+a)/2
    return [np.array(t) for t in ((a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca))]


def plan(uv, support, depth, bary=None):
    if bary is None:
        bary = np.eye(3)
    if not support.possible(bary@uv):
        return [], 1., True
    if depth == 0:
        return [bary], 0., False
    children = [plan(uv, support, depth-1, child) for child in split(bary)]
    if not any(cut for _, _, cut in children):
        # Avoid extra triangles when subdivision cannot remove any surface.
        return [bary], 0., False
    return [child for kept, _, _ in children for child in kept], sum(a for _, a, _ in children)/4, True


def trim(triangles, support, depth, planner=None):
    if triangles.shape[1:] != (3, 66) or not np.isfinite(triangles).all():
        raise ValueError('Finite native animation vertices required')
    # Discrete skeletal/mode fields must remain constant at inserted vertices.
    for first, last in ((18, 34), (54, 66)):
        if np.any(triangles[:, :, first:last] != triangles[:, :1, first:last]):
            raise ValueError('Nonconstant animation fields cannot be interpolated')
    xyz = triangles[:, :, :3]
    low, high = xyz.min((0, 1)), xyz.max((0, 1))
    protected = np.any((xyz == low) | (xyz == high), axis=(1, 2))
    area = np.linalg.norm(np.cross(xyz[:, 1]-xyz[:, 0], xyz[:, 2]-xyz[:, 0]), axis=1)/2
    if float(area.sum()) <= 0:
        raise ValueError('No nondegenerate source surface')
    patterns, inverse = np.unique(triangles[:, :, 6:8].reshape(-1, 6), axis=0, return_inverse=True)
    plans = [(planner(uv.reshape(3, 2)) if planner else plan(uv.reshape(3, 2), support, depth)) for uv in patterns]
    output = []
    removed = 0.
    changed = 0
    for index, (tri, kind) in enumerate(zip(triangles, inverse)):
        kept, fraction, cut = plans[kind]
        if protected[index] or not cut:
            output.append(tri)
            continue
        changed += 1
        removed += float(area[index])*fraction
        for bary in kept:
            output.append((bary@tri).astype(np.float32))
    result = np.asarray(output, dtype=np.float32).reshape(-1, 3, 66)
    if len(result) == 0:
        raise ValueError('Candidate has no retained triangles')
    if not np.array_equal(result[:, :, :3].min((0, 1)), low) or not np.array_equal(result[:, :, :3].max((0, 1)), high):
        raise ValueError('Candidate changed native extrema')
    return result, dict(original_triangles=len(triangles), candidate_triangles=len(result),
                        triangle_ratio=len(result)/len(triangles),
                        removed_surface_fraction=removed/float(area.sum()),
                        changed_source_triangles=changed, protected_source_triangles=int(protected.sum()),
                        unique_uv_triangles=len(patterns))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--depths', nargs='+', type=int, default=[1, 2, 3])
    args = parser.parse_args()
    if any(d < 0 or d > 5 for d in args.depths) or len(set(args.depths)) != len(args.depths):
        parser.error('Use distinct subdivision depths from 0 to 5')
    destination = args.output.resolve()
    # Experimental prefabs must stay out of the authored asset tree.
    destination.relative_to(ROOT/'.local')
    if destination.exists() and any(destination.iterdir()):
        parser.error('Use a fresh candidate directory')
    prefabs = sorted((ROOT/'Engine/Core/Assets/Prefabs/Woodland/Oaks').glob('Large_oak_tree_*_Canopy.prefab'))
    if len(prefabs) != 4:
        raise ValueError('Expected four installed canopies')
    sources = {p: load_prefab(p) for p in prefabs}
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {}
    for _, inputs in sources.values():
        for path, value in inputs.items():
            if path in before and before[path] != value:
                raise ValueError('Input changed during discovery')
            before[path] = value
    for name in ('build_foliage_interior_trim.py', 'screen_foliage_trim.py', 'build_canopy_lod.py', 'audit_foliage_overlap.py'):
        path = Path(__file__).resolve().parent/name
        before[path] = digest(path)
    for name in ('Compoonents/Materials/Texture.cpp', 'Model/MeshData.h', 'Shaders/GrassWind.hlsli'):
        path = ROOT/'Engine/Core'/name
        before[path] = digest(path)
    destination.mkdir(parents=True)
    rows = []
    for prefab, (references, _) in sources.items():
        name = prefab.stem.removesuffix('_Canopy')
        for mesh, material in references:
            part = load_part(mesh, material)
            support = Support(mip_chain(part['texture'][:, :, 3]), part['cutoff'])
            for depth in args.depths:
                candidate, metrics = trim(part['triangles'], support, depth)
                folder = destination/f'depth-{depth}'/name
                folder.mkdir(parents=True)
                path = folder/(name+'_InteriorTrim.mesh')
                vertices = candidate.reshape(-1, 66)
                path.write_bytes(struct.pack('<4I', 0x4853454D, 7, len(vertices), 0)+vertices.astype('<f4').tobytes())
                tree = ET.parse(prefab)
                replaced = 0
                for node in tree.getroot().iter():
                    if node.tag.endswith('}Mesh') and resolve(node.get('Mesh.file')) == mesh:
                        node.set('Mesh.file', path.relative_to(ROOT).as_posix())
                        replaced += 1
                if replaced != 1:
                    raise ValueError('Expected exactly one leaf mesh replacement')
                ET.indent(tree)
                tree.write(folder/(name+'_CanopyCandidate.prefab'), encoding='utf-8', xml_declaration=True)
                rows.append(dict(name=name, depth=depth, source=part['source'],
                                 baseline_prefab=prefab.relative_to(ROOT).as_posix(), **metrics))
                print(name, depth, metrics, flush=True)
    if any(digest(p) != value for p, value in before.items()):
        raise ValueError('Candidate inputs changed')
    artifacts = {p.relative_to(destination).as_posix(): digest(p) for p in sorted(destination.rglob('*')) if p.is_file()}
    report = dict(schema=1, method=__doc__, complete=True, approved_for_installation=False,
                  inputs_unchanged=True, inputs_sha256={p.relative_to(ROOT).as_posix(): h for p, h in sorted(before.items())},
                  artifacts_sha256=artifacts, candidates=rows)
    (destination/'report.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
