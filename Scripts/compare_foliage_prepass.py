"""Validate coverage-matched alpha prepass and report combined two-pass cost.

The mask-only residual includes rasterization, alpha/depth/color work and is
not isolated texture-fetch time. The prepass writes color as well as depth;
compare its complete cost, never just the second shading pass.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics

from compare_foliage_overdraw import samples
from summarize_forest_performance import CASE_FIELDS, load_manifest

MODES = ('leaves', 'leaves-alpha-prepass', 'leaves-alpha-mask', 'leaves-alpha-mask-vertex-only')


def image(directory, manifest, mode, width, height):
    name = f"{manifest['backend']}-entrance-{mode}-90-{width}.ppm"
    data = (directory/name).read_bytes()
    if manifest.get('captures_sha256', {}).get(name) != hashlib.sha256(data).hexdigest():
        raise ValueError('Missing or changed image identity')
    header, dimensions, maximum, pixels = data.split(b'\n', 3)
    if header != b'P6' or dimensions != f'{width} {height}'.encode() or maximum != b'255' or len(pixels) != width*height*3:
        raise ValueError('Invalid capture dimensions')
    return pixels


def compare(directory, allow_image_differences=False):
    directory = Path(directory)
    manifest = load_manifest(directory)
    groups = {}
    for path in directory.glob('*.jsonl'):
        for line in path.read_text().splitlines():
            row = json.loads(line)
            if row.get('mode') not in MODES:
                continue
            if row.get('backend') != manifest['backend'] or Path(row.get('executable_path', '')).resolve() != Path(manifest['executable_path']).resolve():
                raise ValueError('Manifest/recording identity mismatch')
            groups.setdefault((row['width'], row['height']), {}).setdefault(row['mode'], []).append(row)
    if not groups:
        raise ValueError('No prepass controls recorded')
    results = []
    for (width, height), controls in sorted(groups.items()):
        if set(controls) != set(MODES):
            raise ValueError('Incomplete prepass controls')
        reference = controls['leaves'][0]
        fields = tuple(f for f in CASE_FIELDS if f not in ('mode', 'scissor_mode')) + (
            'candidate_triangles', 'draws', 'instanced_material_slots', 'instanced_materials',
            'shader_paths', 'material_shader_states', 'full_shadow_refresh')
        for mode, rows in controls.items():
            for row in rows:
                for field in fields:
                    if row.get(field) is None or row[field] != reference[field]:
                        raise ValueError(f'Prepass control changed {field}')
                prepass = mode == 'leaves-alpha-prepass'
                if row.get('alpha_prepass') != prepass or row.get('prepass_draws') != (row['draws'] if prepass else 0):
                    raise ValueError('Prepass submission accounting mismatch')
                if prepass and not row.get('equal_depth_ready'):
                    raise ValueError('Equal-depth pipeline was not active')
                if row.get('total_color_draws') != row['draws'] + row['prepass_draws']:
                    raise ValueError('Incomplete total submission count')
                if row['scissor_mode'] != ('empty' if mode.endswith('vertex-only') else 'full'):
                    raise ValueError('Unexpected scissor intervention')
                if row['build_configuration'] == 'unknown' or not row['workload_counters_requested'] or not row['full_shadow_refresh']:
                    raise ValueError('Missing build/counter/refresh identity')
        color = {mode: samples(rows) for mode, rows in controls.items()}
        for mode, (rows, _) in color.items():
            for suffix in ('_vs_invocations', '_primitives'):
                field = 'gpu_tree_leaves' + suffix
                baseline = {r[field] for r in color['leaves'][0]}
                values = {r[field] for r in rows}
                factor = 2 if mode == 'leaves-alpha-prepass' else 1
                if len(baseline) != 1 or min(baseline) <= 0 or values != {next(iter(baseline))*factor}:
                    raise ValueError('Prepass executed geometry differs from expected workload')
        vertex = color['leaves-alpha-mask-vertex-only'][1]
        if vertex['gpu_tree_leaves_ps_invocations'] != 0:
            raise ValueError('Vertex-only control executed fragments')
        baseline_image = image(directory, manifest, 'leaves', width, height)
        candidate_image = image(directory, manifest, 'leaves-alpha-prepass', width, height)
        mask = image(directory, manifest, 'leaves-alpha-mask', width, height)
        identical = baseline_image == candidate_image
        if not identical and not allow_image_differences:
            raise ValueError('Prepass changed final color/coverage')
        reference_rgb = list(zip(baseline_image[::3], baseline_image[1::3], baseline_image[2::3]))
        candidate_rgb = zip(candidate_image[::3], candidate_image[1::3], candidate_image[2::3])
        changed_pixels = sum(a != b for a, b in zip(reference_rgb, candidate_rgb))
        max_delta = max(abs(a-b) for a, b in zip(baseline_image, candidate_image))
        pixels = list(zip(mask[::3], mask[1::3], mask[2::3]))
        white = sum(min(pixel) >= 250 for pixel in pixels)
        if white <= width*height/20:
            raise ValueError('Alpha-only shader not active')
        background_mismatch = sum(p != ref for p, ref in zip(pixels, reference_rgb) if min(p) < 250)
        if background_mismatch and not allow_image_differences:
            raise ValueError('Alpha mask changed background coverage')
        result = dict(width=width, height=height, image_byte_identical=identical,
                      quality_accepted=identical and background_mismatch == 0,
                      changed_pixels=changed_pixels, maximum_channel_delta=max_delta,
                      mask_background_mismatch_pixels=background_mismatch, mask_white_pixels=white,
                      color={mode: dict(samples=len(rows), **means) for mode, (rows, means) in color.items()},
                      alpha_raster_depth_color_residual_ms=color['leaves-alpha-mask'][1]['gpu_tree_leaves_ms']-vertex['gpu_tree_leaves_ms'],
                      combined_prepass_delta_ms=color['leaves-alpha-prepass'][1]['gpu_tree_leaves_ms']-color['leaves'][1]['gpu_tree_leaves_ms'],
                      frame_wall_mean_ms={mode: statistics.mean(r['frame_ms'] for r in rows) for mode, rows in controls.items()})
        if reference['shadows']:
            shadow = {mode: samples(controls[mode], shadow=True) for mode in MODES}
            for field in ('gpu_shadow_vs_invocations', 'gpu_shadow_primitives'):
                values = {r[field] for rows, _ in shadow.values() for r in rows}
                if len(values) != 1 or min(values) <= 0:
                    raise ValueError('Full shadow workload changed')
            result['shadow'] = {mode: dict(samples=len(rows), **means) for mode, (rows, means) in shadow.items()}
        results.append(result)
    return dict(backend=manifest['backend'], executable_sha256=manifest['executable_sha256'],
                inputs_sha256=manifest['inputs_sha256'], method=__doc__, cases=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--record-rejected', action='store_true', help='Report image failures explicitly without accepting the candidate')
    args = parser.parse_args()
    result = compare(args.directory, allow_image_differences=args.record_rejected)
    output = args.directory/'foliage-prepass.json'
    output.write_text(json.dumps(result, indent=2)+'\n')
    print(output)


if __name__ == '__main__':
    main()
