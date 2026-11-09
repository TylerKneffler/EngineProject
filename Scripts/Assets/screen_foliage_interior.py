"""Screen redundant leaf triangles using training views and disjoint holdouts.

Nearest base-mip alpha, pixel centres, frozen mesh-local geometry, pre-depth.
Not filtered renderer coverage, wind equivalence, shadow acceptance or GPU cost.
Writes index plans only; never installs or generates replacement meshes.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from audit_foliage_overlap import load_prefab, project, raster_counts
from build_canopy_lod import ROOT, load_part


def capture(part, target, radius, view, size):
    _, az, el, ortho = view
    hits = [np.empty(0, dtype=np.int64) for _ in part['triangles']]
    def visit(index, pixels):
        hits[index] = pixels
    _, counts = raster_counts(project(part, target, radius, az, el, size, ortho),
                              part['texture'], part['cutoff'], size, not ortho, visit)
    return hits, counts.reshape(-1)


def prune(captures, protected):
    """Sequential removal preserves every training union, including joint loss."""
    count = len(protected)
    if not captures or any(len(hits) != count for hits, _ in captures):
        raise ValueError('Matching nonempty training captures required')
    remaining = []
    for hits, counts in captures:
        expected = np.zeros_like(counts)
        for pixels in hits:
            if len(np.unique(pixels)) != len(pixels):
                raise ValueError('Duplicate pixels in one triangle')
            np.add.at(expected, pixels, 1)
        if not np.array_equal(expected, counts):
            raise ValueError('Coverage counts disagree with triangle hits')
        remaining.append(counts.copy())
    kept = np.ones(count, dtype=bool)
    # Low observed contribution first; source index breaks ties deterministically.
    order = sorted(range(count), key=lambda i: (sum(len(h[i]) for h, _ in captures), i))
    for index in order:
        if protected[index] or any(np.any(c[h[index]] <= 1) for (h, _), c in zip(captures, remaining)):
            continue
        kept[index] = False
        for (hits, _), counts in zip(captures, remaining):
            counts[hits[index]] -= 1
    if any(not np.array_equal(original > 0, final > 0)
           for (_, original), final in zip(captures, remaining)):
        raise ValueError('Training coverage changed')
    return kept


def coverage(hits, counts, kept):
    candidate = np.zeros_like(counts)
    for keep, pixels in zip(kept, hits):
        if keep:
            np.add.at(candidate, pixels, 1)
    union = int(np.count_nonzero(counts))
    if not union:
        raise ValueError('Empty projection')
    lost = int(np.count_nonzero((counts > 0) & (candidate == 0)))
    return dict(reference_pixels=union, lost_pixels=lost, lost_fraction=lost/union,
                reference_accepted_samples=int(counts.sum()), candidate_accepted_samples=int(candidate.sum()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.relative_to(ROOT/'.local')
    if output.exists():
        parser.error('Use a fresh output file')
    train = [('ground', 17, -12, False), ('elevated', 107, 35, False),
             ('light-low', 37, 25, True), ('light-high', 137, 65, True)]
    holdout = [(f'camera-{a}', a, e, False) for a, e in ((62, -5), (152, 20), (242, 35), (332, 0))]
    holdout += [(f'light-{a}', a, e, True) for a, e in ((82, 25), (172, 65), (262, 25), (352, 65))]
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {Path(__file__).resolve(): digest(Path(__file__).resolve())}
    for name in ('audit_foliage_overlap.py', 'build_canopy_lod.py'):
        path = Path(__file__).resolve().parent/name
        before[path] = digest(path)
    rows = []
    prefabs = sorted((ROOT/'Engine/Core/Assets/Prefabs/Woodland/Oaks').glob('Large_oak_tree_*_Canopy.prefab'))
    if len(prefabs) != 4:
        raise ValueError('Expected four installed canopies')
    for prefab in prefabs:
        references, inputs = load_prefab(prefab)
        for p, value in inputs.items():
            if p in before and before[p] != value:
                raise ValueError('Input changed during discovery')
            before[p] = value
        if len(references) != 1:
            raise ValueError('Screen requires one native leaf part')
        part = load_part(*references[0])
        xyz = part['triangles'][:, :, :3]
        low, high = xyz.min((0, 1)), xyz.max((0, 1))
        target = (low+high)/2; radius = float(np.linalg.norm(high-low)/2)
        protected = np.any((xyz == low) | (xyz == high), axis=(1, 2))
        captures = []; cases = []
        for size in (128, 256):
            for view in train:
                captures.append(capture(part, target, radius, view, size))
                cases.append((view[0], size))
        kept = prune(captures, protected)
        training = [dict(view=v, size=s, **coverage(h, c, kept)) for (v, s), (h, c) in zip(cases, captures)]
        validation = []
        for view in holdout:
            h, c = capture(part, target, radius, view, 256)
            validation.append(dict(view=view[0], size=256, **coverage(h, c, kept)))
        row = dict(prefab=prefab.relative_to(ROOT).as_posix(), source_triangles=len(kept),
                   retained_triangles=int(kept.sum()), protected_triangles=int(protected.sum()),
                   removed_indices=np.flatnonzero(~kept).tolist(), training=training, holdouts=validation)
        rows.append(row)
        print(prefab.stem, 'removed', len(kept)-int(kept.sum()), 'worst holdout loss',
              max(c['lost_fraction'] for c in validation), flush=True)
    if any(digest(p) != value for p, value in before.items()):
        raise ValueError('Screen inputs changed')
    report = dict(schema=1, method=__doc__, complete=True, inputs_unchanged=True,
                  approved_for_installation=False, training_views=train, holdout_views=holdout,
                  inputs_sha256={p.relative_to(ROOT).as_posix(): h for p, h in sorted(before.items())}, candidates=rows)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
