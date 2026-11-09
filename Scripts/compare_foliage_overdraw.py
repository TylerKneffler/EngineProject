"""Report controlled foliage ablations without calling PS invocations overdraw.

Removing alpha cutoff changes coverage and depth rejection as well as sampling;
its delta is a combined ablation, not isolated alpha-texture cost or a candidate.
"""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import statistics

from summarize_forest_performance import CASE_FIELDS, load_manifest

MODES = ('leaves', 'leaves-unlit', 'leaves-solid', 'leaves-vertex-only')


def samples(rows, shadow=False):
    valid = 'shadow_valid' if shadow else 'gpu_valid'
    identity = 'shadow_sample_id' if shadow else 'gpu_sample_id'
    selected = [r for r in rows if r.get(valid) and (shadow or r.get('gpu_workload_valid'))]
    if len(selected) < 20:
        raise ValueError('Need at least 20 completed workload samples per control')
    ids = [r[identity] for r in selected]
    if len(set(ids)) != len(ids):
        raise ValueError('Repeated GPU sample identity')
    prefix = 'gpu_shadow' if shadow else 'gpu_tree_leaves'
    fields = [prefix + suffix for suffix in ('_ms', '_vs_invocations', '_primitives', '_ps_invocations')]
    for row in selected:
        for field in fields:
            value = row[field]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError(f'Invalid workload value: {field}')
    return selected, {field: statistics.mean(r[field] for r in selected) for field in fields}


def compare(directory):
    directory = Path(directory)
    manifest = load_manifest(directory)
    groups = {}
    for path in sorted(directory.glob('*.jsonl')):
        for line in path.read_text().splitlines():
            row = json.loads(line)
            if row.get('mode') not in MODES:
                continue
            if row.get('backend') != manifest['backend'] or Path(row.get('executable_path', '')).resolve() != Path(manifest['executable_path']).resolve():
                raise ValueError('Recording differs from manifest backend/executable')
            for field in CASE_FIELDS:
                if row.get(field) is None:
                    raise ValueError(f'Missing comparison identity: {field}')
            if row['build_configuration'] == 'unknown' or not row['workload_counters_requested']:
                raise ValueError('Unknown build or workload counters disabled')
            groups.setdefault((row['view'], row['width'], row['height']), {}).setdefault(row['mode'], []).append(row)
    if not groups:
        raise ValueError('No foliage controls recorded')
    results = []
    for key, controls in sorted(groups.items()):
        if set(controls) != set(MODES):
            raise ValueError('Incomplete foliage control group')
        reference = controls['leaves'][0]
        # Mode and scissor are intentional interventions. All other identity must match.
        fields = tuple(f for f in CASE_FIELDS if f not in ('mode', 'scissor_mode')) + (
            'candidate_triangles', 'draws', 'instanced_material_slots', 'instanced_materials',
            'shader_paths', 'material_shader_states', 'executable_path')
        for mode, rows in controls.items():
            for row in rows:
                for field in fields:
                    if row.get(field) is None or row[field] != reference[field]:
                        raise ValueError(f'Foliage control changed {field}')
                if row['scissor_mode'] != ('empty' if mode.endswith('vertex-only') else 'full'):
                    raise ValueError('Unexpected scissor intervention')
        color = {mode: samples(rows) for mode, rows in controls.items()}
        for suffix in ('_vs_invocations', '_primitives'):
            field = 'gpu_tree_leaves' + suffix
            values = {r[field] for rows, _ in color.values() for r in rows}
            if len(values) != 1 or next(iter(values)) <= 0:
                raise ValueError('Color control changed executed geometry')
        if color['leaves-vertex-only'][1]['gpu_tree_leaves_ps_invocations'] != 0:
            raise ValueError('Vertex-only control executed fragments')
        if color['leaves'][1]['gpu_tree_leaves_ps_invocations'] <= 0:
            raise ValueError('Reference executed no fragments')
        result = dict(view=key[0], width=key[1], height=key[2], shadows=reference['shadows'],
                      color={mode: dict(samples=len(rows), **means) for mode, (rows, means) in color.items()})
        baseline = color['leaves'][1]['gpu_tree_leaves_ms']
        result['color_delta_ms'] = {mode: means['gpu_tree_leaves_ms'] - baseline for mode, (_, means) in color.items()}
        if reference['shadows']:
            # Empty color scissor does not define a shadow-only vertex control.
            shadow = {mode: samples(controls[mode], shadow=True) for mode in MODES[:3]}
            # Staggered cascade refresh changes workload from frame to frame.
            # Require the complete joint VS/IA histogram, including frequency.
            histogram = lambda rows: Counter((r['gpu_shadow_vs_invocations'], r['gpu_shadow_primitives']) for r in rows)
            expected = histogram(shadow['leaves'][0])
            if any(v <= 0 or p <= 0 for v, p in expected):
                raise ValueError('Empty shadow geometry')
            if any(histogram(rows) != expected for rows, _ in shadow.values()):
                raise ValueError('Shadow control changed geometry or refresh distribution')
            result['shadow_workload_distribution'] = [dict(vs_invocations=v, primitives=p, samples=count)
                                                       for (v, p), count in sorted(expected.items())]
            result['shadow'] = {mode: dict(samples=len(rows), **means) for mode, (rows, means) in shadow.items()}
            result['shadow_delta_ms'] = {mode: means['gpu_shadow_ms'] - shadow['leaves'][1]['gpu_shadow_ms']
                                         for mode, (_, means) in shadow.items()}
        results.append(result)
    return dict(backend=manifest['backend'], executable_sha256=manifest['executable_sha256'],
                inputs_sha256=manifest['inputs_sha256'], method=__doc__, cases=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = compare(args.directory)
    output = args.directory / 'foliage-ablation.json'
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
