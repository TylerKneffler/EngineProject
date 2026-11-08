"""Build isolated, visibility-pruned 3D oak candidates and matched software captures.

Requires numpy, scipy and Pillow. Does not edit scene, source meshes or LOD policy.
The alpha-tested software views are an asset screening tool, not renderer approval.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / '.local/canopy-deps'))
from PIL import Image
from audit_oak_lods import resolve

# The native XML reader recognizes the literal components prefix.
ET.register_namespace('components', 'urn:engine-components')


def load_part(mesh, material):
    raw = mesh.read_bytes()
    magic, version, nv, ni = struct.unpack('<4I', raw[:16])
    if (magic, version) != (0x4853454D, 7) or len(raw) != 16 + nv * 264 + ni * 4:
        raise ValueError(f'Unsupported mesh layout: {mesh}')
    vertices = np.frombuffer(raw, '<f4', nv * 66, 16).reshape(nv, 66).copy()
    indices = np.frombuffer(raw, '<u4', ni, 16 + nv * 264) if ni else np.arange(nv)
    triangles = vertices[indices].reshape(-1, 3, 66)
    # Position welding reconnects expanded native vertices without changing attributes.
    _, weld = np.unique(triangles[:, :, :3].reshape(-1, 3), axis=0, return_inverse=True)
    welded = weld.reshape(-1, 3)
    edges = np.concatenate((welded[:, :2], welded[:, 1:], welded[:, [2, 0]]))
    count, labels = connected_components(coo_matrix((np.ones(len(edges)), edges.T)), directed=False)
    groups = labels[welded[:, 0]]
    if 'Bark' in material.name:
        # Bark is usually one connected trunk/branch network. Cull hidden faces,
        # rather than treating the entire network as one indivisible component.
        groups = np.arange(len(triangles))
        count = len(triangles)
    data = json.loads(material.read_text())
    texture = np.asarray(Image.open(resolve(data['baseColorTexture'])).convert('RGBA'))
    return dict(triangles=triangles, groups=groups, count=count, texture=texture,
                cutoff=data.get('alphaCutoff', .32), source=str(mesh.relative_to(ROOT)),
                sha256=hashlib.sha256(raw).hexdigest())


def render(parts, eye, target, fov, size, masks=None, footprints=None, view_index=0):
    forward = target - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0., 1., 0.]); right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    basis = np.array([right, up, forward]).T
    focal = size / (2 * math.tan(math.radians(fov) / 2))
    depth = np.full((size, size), np.inf)
    owner = np.full((size, size), -1, dtype=np.int32)
    rgb = np.full((size, size, 3), (172, 198, 218), dtype=np.uint8)
    offset = 0
    for part_index, part in enumerate(parts):
        triangles = part['triangles']
        camera = (triangles[:, :, :3] - eye) @ basis
        xy = camera[:, :, :2] / camera[:, :, 2:] * [focal, -focal] + size / 2
        for i, points in enumerate(xy):
            if masks is not None and not masks[part_index][i]: continue
            if camera[i, :, 2].min() <= .1: continue
            lo = np.maximum(np.floor(points.min(0)).astype(int), 0)
            hi = np.minimum(np.ceil(points.max(0)).astype(int), size - 1)
            if np.any(hi < lo): continue
            x, y = np.meshgrid(np.arange(lo[0], hi[0]+1)+.5, np.arange(lo[1], hi[1]+1)+.5)
            a, b, c = points
            denominator = (b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
            if abs(denominator) < 1e-8: continue
            w0 = ((b[1]-c[1])*(x-c[0])+(c[0]-b[0])*(y-c[1]))/denominator
            w1 = ((c[1]-a[1])*(x-c[0])+(a[0]-c[0])*(y-c[1]))/denominator
            weights = np.stack((w0, w1, 1-w0-w1), -1)
            inside = (weights >= -1e-6).all(-1)
            if not inside.any(): continue
            weights[~inside] = (1, 0, 0)
            reciprocal = weights / camera[i, :, 2]
            z = 1 / np.maximum(reciprocal.sum(-1), 1e-12)
            uv = (reciprocal @ triangles[i, :, 6:8]) * z[:, :, None]
            tex = part['texture']; th, tw = tex.shape[:2]
            # Native UVs and WIC/Pillow rows both use a top-left texture origin.
            samples = tex[(np.floor(uv[:, :, 1]*th).astype(int) % th),
                          (np.floor(uv[:, :, 0]*tw).astype(int) % tw)]
            region = np.s_[lo[1]:hi[1]+1, lo[0]:hi[0]+1]
            opaque = inside & (samples[:, :, 3] >= part['cutoff']*255)
            if footprints is not None and opaque.any():
                yy, xx = np.nonzero(opaque)
                ids = ((yy+lo[1])*size+xx+lo[0]+view_index*size*size).astype(np.int32)
                footprints[offset+part['groups'][i]].append(ids)
            hit = opaque & (z < depth[region])
            depth[region][hit] = z[hit]
            owner[region][hit] = offset + part['groups'][i]
            # Fixed diffuse diagnostic lighting, identical for both geometries.
            normal = triangles[i, :, 3:6].mean(0)
            light = .45 + .55 * abs(np.dot(normal, np.array([.3, .8, .52])))
            rgb[region][hit] = np.clip(samples[:, :, :3][hit] * light, 0, 255).astype('uint8')
        offset += part['count']
    return rgb, owner, depth


def prefab_parts(tree):
    parts = []
    nodes = []
    for group in tree.getroot().iter('components'):
        mesh = next((e for e in group if e.tag.endswith('}Mesh')), None)
        material = next((e for e in group if e.tag.endswith('}Material')), None)
        if mesh is None: continue
        parts.append(load_part(resolve(mesh.get('Mesh.file')), resolve(material.get('Material.materialAsset'))))
        nodes.append(mesh)
    return parts, nodes


def screen_authored(trees, destination):
    original,_=prefab_parts(trees[0]); candidate,_=prefab_parts(trees[1])
    points=np.concatenate([p['triangles'][:,:,:3].reshape(-1,3) for p in original])
    low,high=points.min(0),points.max(0)
    target=(low+high)/2; radius=np.linalg.norm(high-low)/2
    rows=[]
    for view,elevation,fov,distance in [('ground',-12,60,3),('elevated',35,60,3),('zoom',35,30,5)]:
        az,el=np.radians([17,elevation])
        eye=target+radius*distance*np.array([math.cos(el)*math.sin(az),math.sin(el),math.cos(el)*math.cos(az)])
        a,owner,_=render(original,eye,target,fov,384)
        b,other,_=render(candidate,eye,target,fov,384)
        coverage=owner>=0
        rows.append(dict(view=view,frame=0,coverage_pixels=int(coverage.sum()),
                         missing_coverage_fraction=float(np.count_nonzero(coverage & (other<0))/max(1,coverage.sum())),
                         changed_tree_fraction=float(np.count_nonzero(coverage & np.any(np.abs(a.astype(int)-b.astype(int))>3,axis=2))/max(1,coverage.sum()))))
        Image.fromarray(np.concatenate((a,b),axis=1)).save(destination/(view+'.png'))
    return dict(comparisons=rows,bounds_min=low.tolist(),bounds_max=high.tolist(),
                max_missing_coverage_fraction=max(r['missing_coverage_fraction'] for r in rows),
                max_changed_tree_fraction=max(r['changed_tree_fraction'] for r in rows))


def evaluate(name, output, training_size, retention, coverage_constrained):
    source = ROOT / 'Engine/Core/Assets/Prefabs/Woodland/Oaks' / (name+'_LOD1.prefab')
    tree = ET.parse(source)
    parts,nodes=prefab_parts(tree)
    positions = np.concatenate([p['triangles'][:, :, :3].reshape(-1, 3) for p in parts])
    low, high = positions.min(0), positions.max(0)
    target = (low+high)/2
    radius = np.linalg.norm(high-low)/2
    seen = np.zeros(sum(p['count'] for p in parts), dtype=int)
    footprints = [[] for _ in seen] if coverage_constrained else None
    view_index = 0
    for elevation in (-12, 15, 40, 65):
        for azimuth in range(0, 360, 30):
            az, el = np.radians([azimuth, elevation])
            eye = target + radius*3*np.array([math.cos(el)*math.sin(az), math.sin(el), math.cos(el)*math.cos(az)])
            _, owner, _ = render(parts, eye, target, 50, training_size,
                                 footprints=footprints, view_index=view_index)
            ids, counts = np.unique(owner[owner >= 0], return_counts=True)
            seen[ids] += counts
            view_index += 1
    protected_keep = None
    if coverage_constrained:
        footprints = [np.unique(np.concatenate(items)) if items else np.array([], dtype=np.int32) for items in footprints]
        counts = np.zeros(view_index*training_size**2, dtype=np.int32)
        for ids in footprints: counts[ids] += 1
        covered = np.bincount(np.flatnonzero(counts)//training_size**2, minlength=view_index)
        lost = np.zeros(view_index, dtype=int)
        costs = np.concatenate([np.bincount(p['groups'], minlength=p['count']) for p in parts])
        protected_keep = np.ones(len(seen), dtype=bool)
        remaining = int(costs.sum())
        # Remove low-contribution components only while every training view retains
        # at least 99.9% of its original alpha-tested coverage.
        for group in np.argsort(seen/np.maximum(costs,1), kind='stable'):
            if remaining <= costs.sum()*retention: break
            ids = footprints[group]
            newly_missing = ids[counts[ids] == 1]
            loss = np.bincount(newly_missing//training_size**2, minlength=view_index)
            if np.any(lost+loss > covered*.001): continue
            lost += loss; counts[ids] -= 1
            protected_keep[group] = False; remaining -= int(costs[group])
    masks = []
    offset = 0
    part_rows = []
    destination = output / name
    destination.mkdir(parents=True, exist_ok=True)
    for part, node in zip(parts, nodes):
        contribution = seen[offset:offset+part['count']]
        costs = np.bincount(part['groups'], minlength=part['count'])
        importance = contribution / np.maximum(costs, 1)
        keep = np.zeros(part['count'], dtype=bool)
        order = np.argsort(-importance, kind='stable')
        budget = math.ceil(len(part['triangles'])*retention)
        accumulated = 0
        for group in order:
            if accumulated >= budget or importance[group] == 0: break
            keep[group] = True
            accumulated += costs[group]
        if protected_keep is not None:
            keep = protected_keep[offset:offset+part['count']].copy()
        # Protect exact native extrema in every direction.
        xyz = part['triangles'][:, :, :3]
        for axis in range(3):
            for extreme in (xyz[:, :, axis].min(), xyz[:, :, axis].max()):
                keep[part['groups'][np.any(xyz[:, :, axis] == extreme, axis=1)]] = True
        mask = keep[part['groups']]
        offset += part['count']
        masks.append(mask)
        vertices = part['triangles'][mask].reshape(-1, 66)
        path = destination / Path(part['source']).name.replace('LOD1', 'CanopyCandidate')
        path.write_bytes(struct.pack('<4I', 0x4853454D, 7, len(vertices), 0) + vertices.astype('<f4').tobytes())
        node.set('Mesh.file', str(path.relative_to(ROOT)).replace('\\', '/'))
        part_rows.append(dict(source=part['source'], sha256=part['sha256'],
                              original_triangles=len(mask), candidate_triangles=int(mask.sum()),
                              components=part['count'], retained_components=int(keep.sum())))
    ET.indent(tree)
    tree.write(destination / (name+'_CanopyCandidate.prefab'), encoding='utf-8', xml_declaration=True)
    print(name, [(p['original_triangles'],p['candidate_triangles']) for p in part_rows], flush=True)
    rows = []
    previous = None
    for view, elevation, fov, distance in [('ground', -12, 60, 3), ('elevated', 35, 60, 3), ('zoom', 35, 30, 5)]:
        frames = []
        previous = None
        for frame in range(12):
            az, el = np.radians([17+frame*2, elevation])
            eye = target + radius*distance*np.array([math.cos(el)*math.sin(az), math.sin(el), math.cos(el)*math.cos(az)])
            baseline, owner, depth = render(parts, eye, target, fov, 384)
            candidate, other, other_depth = render(parts, eye, target, fov, 384, masks)
            coverage, candidate_coverage = owner >= 0, other >= 0
            missing = coverage & ~candidate_coverage
            delta = np.abs(baseline.astype(int)-candidate.astype(int)).max(-1)
            changed = delta > 3
            row = dict(view=view, frame=frame, coverage_pixels=int(coverage.sum()),
                       missing_coverage_fraction=float(missing.sum()/max(1, coverage.sum())),
                       changed_tree_fraction=float((changed & coverage).sum()/max(1, coverage.sum())),
                       mean_tree_channel_delta=float(np.abs(baseline.astype(int)-candidate.astype(int))[coverage].mean()),
                       newly_exposed_pixels=int((coverage & candidate_coverage & (other_depth > depth+.01)).sum()))
            if previous is not None:
                row['difference_mask_churn_pixels'] = int(np.count_nonzero(changed != previous))
            previous = changed
            rows.append(row)
            sheet = np.concatenate((baseline, candidate), axis=1)
            frames.append(Image.fromarray(sheet))
            if frame == 0: frames[-1].save(destination / (view+'.png'))
        frames[0].save(destination / (view+'-motion.gif'), save_all=True, append_images=frames[1:], duration=100, loop=0)
    return dict(name=name, bounds_min=low.tolist(), bounds_max=high.tolist(), parts=part_rows, comparisons=rows,
                max_missing_coverage_fraction=max(r['missing_coverage_fraction'] for r in rows),
                max_changed_tree_fraction=max(r['changed_tree_fraction'] for r in rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'.local/canopy-lod')
    parser.add_argument('--training-size', type=int, default=96)
    parser.add_argument('--retention', type=float, default=.55)
    parser.add_argument('--coverage-constrained', action='store_true', help='protect 99.9 percent training-view alpha coverage')
    parser.add_argument('--authored-lod2', action='store_true', help='stage existing spatial LOD2 geometry as a middle-band candidate')
    parser.add_argument('--screen-authored', action='store_true', help='also render three isolated authored-candidate screening stills')
    parser.add_argument('--varieties', nargs='+', default=[f'Large_oak_tree_{i:03}' for i in range(1,5)])
    args = parser.parse_args()
    output = args.output.resolve()
    output.relative_to(ROOT)
    output.mkdir(parents=True, exist_ok=True)
    if not 0 < args.retention <= 1: parser.error('--retention must be in (0, 1]')
    if not 16 <= args.training_size <= 1024: parser.error('--training-size must be between 16 and 1024')
    rows = []
    for name in args.varieties:
        if args.authored_lod2:
            folder=ROOT/'Engine/Core/Assets/Prefabs/Woodland/Oaks'
            trees=[ET.parse(folder/(name+f'_LOD{lod}.prefab')) for lod in (1,2)]
            nodes=[[node for node in tree.getroot().iter() if node.tag.endswith('}Mesh')] for tree in trees]
            parts=[]
            for original,candidate in zip(*nodes):
                paths=[resolve(node.get('Mesh.file')) for node in (original,candidate)]
                raw=[path.read_bytes() for path in paths]
                headers=[struct.unpack('<4I',data[:16]) for data in raw]
                if any(h[:2]!=(0x4853454D,7) for h in headers): raise ValueError('Unsupported authored mesh')
                parts.append(dict(source=str(paths[0].relative_to(ROOT)),sha256=hashlib.sha256(raw[0]).hexdigest(),
                                  candidate_source=str(paths[1].relative_to(ROOT)),candidate_sha256=hashlib.sha256(raw[1]).hexdigest(),
                                  original_triangles=(headers[0][3] or headers[0][2])//3,
                                  candidate_triangles=(headers[1][3] or headers[1][2])//3))
            destination=output/name; destination.mkdir(parents=True,exist_ok=True)
            trees[1].getroot().find('object').set('name',name+'_CanopyCandidate')
            ET.indent(trees[1]); trees[1].write(destination/(name+'_CanopyCandidate.prefab'),encoding='utf-8',xml_declaration=True)
            rows.append(dict(name=name,parts=parts,comparisons=[],kind='authored spatial LOD2 staged as middle band'))
            if args.screen_authored: rows[-1].update(screen_authored(trees,destination))
            print(name,[(p['original_triangles'],p['candidate_triangles']) for p in parts],flush=True)
        else:
            rows.append(evaluate(name, output, args.training_size, args.retention, args.coverage_constrained))
        (output/'report.json').write_text(json.dumps(dict(training_size=args.training_size,
            retention=args.retention, coverage_constrained=args.coverage_constrained,
            authored_lod2=args.authored_lod2,
            status='experimental; no production rollout', results=rows), indent=2)+'\n')


if __name__ == '__main__': main()
