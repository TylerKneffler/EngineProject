"""Run alternating, repeated coverage-matched foliage prepass comparisons."""
import argparse
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

from compare_foliage_prepass import compare
from run_forest_performance import ROOT
from summarize_forest_performance import load_manifest


def summarize(paths, record_rejected=False):
    manifests = [load_manifest(p) for p in paths]
    first = manifests[0]
    for manifest in manifests[1:]:
        for field in ('backend', 'executable_sha256', 'inputs_sha256', 'cmake_cache_sha256', 'platform'):
            if manifest[field] != first[field]:
                raise ValueError(f'Repeated prepass captures changed {field}')
        normalized = lambda m: {k: v for k, v in m['environment'].items() if k != 'ENGINE_FOREST_AUDIT_REVERSE'}
        if normalized(manifest) != normalized(first):
            raise ValueError('Repeated captures changed effective environment')
    runs = [compare(p, allow_image_differences=record_rejected) for p in paths]
    cases = []
    for index, case in enumerate(runs[0]['cases']):
        matching = [r['cases'][index] for r in runs]
        if any((c['width'], c['height']) != (case['width'], case['height']) for c in matching):
            raise ValueError('Repeated captures changed resolution')
        distribution = lambda values: dict(median=statistics.median(values), minimum=min(values), maximum=max(values))
        item = dict(width=case['width'], height=case['height'], processes=len(runs),
                    quality_accepted=all(c['quality_accepted'] for c in matching),
                    changed_pixels=distribution([c['changed_pixels'] for c in matching]),
                    maximum_channel_delta=max(c['maximum_channel_delta'] for c in matching),
                    combined_prepass_delta_ms=distribution([c['combined_prepass_delta_ms'] for c in matching]),
                    alpha_raster_depth_color_residual_ms=distribution([c['alpha_raster_depth_color_residual_ms'] for c in matching]))
        for mode in ('leaves', 'leaves-alpha-prepass', 'leaves-alpha-mask'):
            item[mode] = dict(gpu_ms=distribution([c['color'][mode]['gpu_tree_leaves_ms'] for c in matching]),
                              ps_invocations=distribution([c['color'][mode]['gpu_tree_leaves_ps_invocations'] for c in matching]),
                              frame_wall_ms=distribution([c['frame_wall_mean_ms'][mode] for c in matching]))
        cases.append(item)
    return dict(backend=first['backend'], directories=list(map(str, paths)), cases=cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--backends', nargs='+', choices=('dx11', 'dx12', 'vulkan'), default=['dx11', 'dx12', 'vulkan'])
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--record-rejected', action='store_true', help='Retain image rejection results; no candidate approval')
    parser.add_argument('--resume', action='store_true', help='Revalidate completed captures and continue the frozen runtime study')
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error('At least three independent processes required')
    root = args.directory.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()) and not args.resume:
        parser.error('Use an empty output directory')
    source = ROOT/'build/Debug/Release'
    runtime = root/'runtime/Release'
    if args.resume:
        if not (runtime/'ForestPerformanceAudit.exe').is_file() or not (runtime.parent/'CMakeCache.txt').is_file():
            parser.error('Resume requires the existing frozen runtime')
    else:
        runtime.mkdir(parents=True)
        for path in list(source.glob('*.dll'))+[source/'ForestPerformanceAudit.exe']:
            shutil.copy2(path, runtime/path.name)
        shutil.copy2(source.parent/'CMakeCache.txt', runtime.parent/'CMakeCache.txt')
    summaries = []
    output = root/'study.json'
    output.write_text(json.dumps(dict(complete=False, results=[]), indent=2)+'\n')
    for backend in args.backends:
        paths = []
        for index in range(args.repeats):
            directory = root/f'{backend}-{index+1}'
            command = [sys.executable, str(ROOT/'Scripts/run_forest_performance.py'), str(directory),
                       '--backend', backend, '--width', '1920', '--shadows', '--counters', '--prepass-only',
                       '--executable', str(runtime/'ForestPerformanceAudit.exe')]
            if index % 2:
                command += ['--reverse']
            if not (args.resume and (directory/'provenance.json').is_file()):
                with (root/f'{backend}-{index+1}.log').open('w') as log:
                    subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            result = compare(directory, allow_image_differences=args.record_rejected)
            (directory/'foliage-prepass.json').write_text(json.dumps(result, indent=2)+'\n')
            paths.append(directory)
            accepted = all(c['quality_accepted'] for c in result['cases'])
            print(f'{backend}: checked process {index+1}/{args.repeats}; image_quality_accepted={accepted}', flush=True)
        summaries.append(summarize(paths, args.record_rejected))
        output.write_text(json.dumps(dict(complete=False, results=summaries), indent=2)+'\n')
    # Backend identity differs intentionally; binaries and content must not.
    identities = [load_manifest(Path(result['directories'][0])) for result in summaries]
    for field in ('inputs_sha256', 'executable_sha256', 'cmake_cache_sha256'):
        if len({m[field] for m in identities}) != 1:
            raise ValueError(f'Backend studies changed {field}')
    output.write_text(json.dumps(dict(complete=True, results=summaries), indent=2)+'\n')
    print(output, flush=True)


if __name__ == '__main__':
    main()
