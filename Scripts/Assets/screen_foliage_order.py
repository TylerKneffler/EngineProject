"""Screen leaf triangle ordering with an ideal per-sample early-depth model.

Nearest base-mip alpha, frozen isolated meshes, LESS_EQUAL depth and no bark/forest.
Counts eligible card samples and accepted depth writes, not GPU invocations,
texture reads or timing. No hardware early-test guarantee, filtering or wind.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from audit_foliage_overlap import load_prefab, project, raster_counts
from build_canopy_lod import ROOT, load_part


def simulate(fragments, order, pixels):
    if sorted(order) != list(range(len(fragments))):
        raise ValueError('Order must be a triangle permutation')
    depth = np.full(pixels, np.inf)
    eligible = writes = 0
    for index in order:
        positions, z, accepted = fragments[index]
        if len(np.unique(positions)) != len(positions):
            raise ValueError('Duplicate per-triangle pixels')
        if not np.isfinite(z).all() or np.any(z <= 0):
            raise ValueError('Invalid camera depth')
        passing = z <= depth[positions]
        eligible += int(passing.sum())
        update = passing & accepted
        writes += int(update.sum())
        depth[positions[update]] = z[update]
    return dict(eligible_card_samples=eligible, accepted_depth_writes=writes), depth


def capture(part, target, radius, view, size):
    _, az, el, ortho = view
    triangles = project(part, target, radius, az, el, size, ortho)
    fragments = [(np.empty(0, dtype=np.int64), np.empty(0), np.empty(0, dtype=bool))
                 for _ in triangles]
    def visit(index, pixels, depth, accepted):
        fragments[index] = (pixels, depth, accepted)
    raster_counts(triangles, part['texture'], part['cutoff'], size, not ortho,
                  fragment_visitor=visit)
    return fragments, triangles[:, :, 2].mean(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.relative_to(ROOT/'.local')
    if output.exists():
        parser.error('Use a fresh output file')
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    dependencies = [Path(__file__).resolve()]
    dependencies += [Path(__file__).resolve().parent/n for n in
                     ('audit_foliage_overlap.py', 'build_canopy_lod.py', 'audit_oak_lods.py')]
    before = {p: digest(p) for p in dependencies}
    views = [(f'camera-{az}', az, el, False) for az, el in
             ((17, -12), (62, -5), (107, 35), (152, 20), (197, -12), (242, 35), (287, 35), (332, 0))]
    views += [(f'light-{az}', az, el, True) for az, el in ((37, 25), (137, 65), (217, 25), (317, 65))]
    prefabs = sorted((ROOT/'Engine/Core/Assets/Prefabs/Woodland/Oaks').glob('Large_oak_tree_*_Canopy.prefab'))
    if len(prefabs) != 4:
        raise ValueError('Expected four installed canopies')
    rows = []
    for prefab in prefabs:
        references, inputs = load_prefab(prefab)
        for p, h in inputs.items():
            if p in before and before[p] != h:
                raise ValueError('Input changed during discovery')
            before[p] = h
        if len(references) != 1:
            raise ValueError('Expected one native leaf part')
        part = load_part(*references[0])
        xyz = part['triangles'][:, :, :3]
        low, high = xyz.min((0, 1)), xyz.max((0, 1))
        target = (low+high)/2; radius = float(np.linalg.norm(high-low)/2)
        anchor = project(part, target, radius, 17, -12, 256, False)
        fixed = np.argsort(anchor[:, :, 2].mean(1), kind='stable')
        for view in views:
            fragments, centroids = capture(part, target, radius, view, 256)
            orders = dict(native=np.arange(len(fragments)), fixed_anchor=fixed,
                          view_front=np.argsort(centroids, kind='stable'),
                          view_back=np.argsort(-centroids, kind='stable'))
            metrics = {}; reference = None
            for name, order in orders.items():
                metrics[name], depth = simulate(fragments, order, 256**2)
                if reference is None:
                    reference = depth
                elif not np.array_equal(reference, depth):
                    raise ValueError('Ordering changed final sampled depth')
            rows.append(dict(prefab=prefab.relative_to(ROOT).as_posix(), view=view[0],
                             size=256, final_depth_equal=True, orders=metrics))
        print('Screened', prefab.stem, flush=True)
    if any(digest(p) != h for p, h in before.items()):
        raise ValueError('Inputs changed during screen')
    result = dict(schema=1, method=__doc__, complete=True, inputs_unchanged=True,
                  approved_for_installation=False, views=views,
                  inputs_sha256={p.relative_to(ROOT).as_posix(): h for p, h in sorted(before.items())}, cases=rows)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    main()
