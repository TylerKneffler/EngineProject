"""Screen precomputed local-direction triangle order banks; no runtime changes.

Uses the ideal early-depth model from screen_foliage_order, not GPU costs.
Storage estimates distinguish hypothetical index banks from the current forest
path's expanded vertex ranges. Instance grouping/submission costs are unmeasured.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path

import numpy as np
from audit_foliage_overlap import load_prefab
from build_canopy_lod import ROOT, load_part
from screen_foliage_order import capture, simulate


def unit(vector):
    vector = np.asarray(vector, dtype=float)
    length = np.linalg.norm(vector)
    if vector.shape != (3,) or not np.isfinite(vector).all() or length <= 0:
        raise ValueError('Finite nonzero direction required')
    return vector/length


def local_axis(linear, world_direction):
    """Dot(world direction, A * local position) = dot(A.T * direction, position)."""
    linear = np.asarray(linear, dtype=float)
    if linear.shape != (3, 3) or not np.isfinite(linear).all() or abs(np.linalg.det(linear)) < 1e-12:
        raise ValueError('Finite nonsingular instance linear transform required')
    return unit(linear.T @ unit(world_direction))


def directions(count):
    axes = np.concatenate((np.eye(3), -np.eye(3)))
    if count == 14:
        axes = np.concatenate((axes, np.array(list(itertools.product((-1, 1), repeat=3)))/np.sqrt(3)))
    elif count != 6:
        raise ValueError('Use six or fourteen directions')
    return axes


def bank_orders(triangles, axes):
    centres = triangles[:, :, :3].mean(1)
    return [np.argsort(-(centres @ unit(axis)), kind='stable') for axis in axes]


def select(axes, axis):
    return int(np.argmax(axes @ unit(axis)))


def direction(azimuth, elevation):
    az, el = np.radians([azimuth, elevation])
    return np.array([np.cos(el)*np.sin(az), np.sin(el), np.cos(el)*np.cos(az)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.relative_to(ROOT/'.local')
    if output.exists():
        parser.error('Use a fresh output file')
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    paths = [Path(__file__).resolve()]
    paths += [Path(__file__).resolve().parent/n for n in
              ('screen_foliage_order.py', 'audit_foliage_overlap.py', 'build_canopy_lod.py', 'audit_oak_lods.py')]
    paths += [ROOT/'Engine/Core/Scene/Rendering/SceneRendering.cpp']
    before = {p: digest(p) for p in paths}
    views = [(f'camera-{az}', az, el, False) for az, el in
             ((17, -12), (62, -5), (107, 35), (152, 20), (197, -12), (242, 35), (287, 35), (332, 0))]
    views += [(f'light-{az}', az, el, True) for az, el in ((37, 25), (137, 65), (217, 25), (317, 65))]
    prefabs = sorted((ROOT/'Engine/Core/Assets/Prefabs/Woodland/Oaks').glob('Large_oak_tree_*_Canopy.prefab'))
    if len(prefabs) != 4:
        raise ValueError('Expected four installed canopies')
    rows = []; assets = []
    for prefab in prefabs:
        references, inputs = load_prefab(prefab)
        for p, h in inputs.items():
            if p in before and before[p] != h:
                raise ValueError('Input changed during discovery')
            before[p] = h
        if len(references) != 1:
            raise ValueError('Expected one leaf part')
        part = load_part(*references[0]); triangles = part['triangles']
        low, high = triangles[:, :, :3].min((0, 1)), triangles[:, :, :3].max((0, 1))
        target = (low+high)/2; radius = float(np.linalg.norm(high-low)/2)
        banks = {n: bank_orders(triangles, directions(n)) for n in (6, 14)}
        vertex_bytes = triangles.size*triangles.dtype.itemsize
        assets.append(dict(prefab=prefab.relative_to(ROOT).as_posix(), triangles=len(triangles),
                           expanded_vertex_bytes=vertex_bytes,
                           banks={str(n): dict(hypothetical_index_bytes=n*len(triangles)*3*4,
                                               all_expanded_orders_vertex_bytes=n*vertex_bytes,
                                               order_sha256=[hashlib.sha256(o.astype('<u4').tobytes()).hexdigest() for o in orders])
                                  for n, orders in banks.items()}))
        for view in views:
            fragments, depths = capture(part, target, radius, view, 256)
            native, reference = simulate(fragments, np.arange(len(triangles)), 256**2)
            front, final = simulate(fragments, np.argsort(depths, kind='stable'), 256**2)
            if not np.array_equal(reference, final):
                raise ValueError('View order changed depth')
            metrics = {}
            for n, orders in banks.items():
                selected = select(directions(n), local_axis(np.eye(3), direction(view[1], view[2])))
                measurements, final = simulate(fragments, orders[selected], 256**2)
                if not np.array_equal(reference, final):
                    raise ValueError('Bank order changed depth')
                metrics[str(n)] = dict(selected=selected, **measurements)
            rows.append(dict(prefab=prefab.relative_to(ROOT).as_posix(), view=view[0], size=256,
                             final_depth_equal=True, native=native, view_front=front, banks=metrics))
        print('Screened', prefab.stem, flush=True)
    if any(digest(p) != h for p, h in before.items()):
        raise ValueError('Inputs changed during screen')
    result = dict(schema=1, method=__doc__, complete=True, inputs_unchanged=True,
                  approved_for_installation=False, views=views, assets=assets, cases=rows,
                  inputs_sha256={p.relative_to(ROOT).as_posix(): h for p, h in sorted(before.items())})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    main()
