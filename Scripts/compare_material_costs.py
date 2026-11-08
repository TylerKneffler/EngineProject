"""Validate independent material controls and report measured tree pass costs."""
import argparse
import json
from pathlib import Path
import statistics
from summarize_forest_performance import load_manifest


def compare(directory):
    manifest=load_manifest(directory)
    groups = {}
    for path in directory.glob('*.jsonl'):
        for line in path.read_text().splitlines():
            row = json.loads(line)
            if row['mode'].startswith('material-'):
                if row.get('backend') != manifest['backend']:
                    raise ValueError('Recording backend differs from manifest')
                if not isinstance(row.get('executable_path'),str) or Path(row['executable_path']).resolve() != Path(manifest['executable_path']).resolve():
                    raise ValueError('Recording executable differs from manifest')
                groups.setdefault(row['mode'], []).append(row)
    reference = groups['material-reference']
    results = []
    for mode in ('material-reference', 'material-mips', 'material-cheap'):
        rows = groups[mode]
        for field in ('width', 'height', 'view', 'camera_position', 'camera_target',
                      'impostor_distance', 'shadows', 'candidate_triangles', 'draws',
                      'instanced_material_slots', 'instanced_materials', 'shader_paths', 'material_shader_states'):
            if any(row[field] != reference[0][field] for row in reference + rows):
                raise ValueError(f'Material comparison changed {field}')
        samples = [r for r in rows if r['gpu_valid'] and r['gpu_workload_valid']]
        baseline = [r for r in reference if r['gpu_valid'] and r['gpu_workload_valid']]
        for samples_to_check in (samples,baseline):
            ids=[r['gpu_sample_id'] for r in samples_to_check]
            if len(ids)!=len(set(ids)):
                raise ValueError('Repeated GPU sample in material comparison')
        for field in ('backend','build_configuration','assertions_enabled','executable_path'):
            if any(row.get(field) is None or row[field]!=reference[0][field] for row in rows+reference):
                raise ValueError(f'Material comparison changed or omitted {field}')
        if min(len(samples), len(baseline)) < 20:
            raise ValueError('Need at least 20 resolved workload samples')
        for field in ('gpu_tree_bark_vs_invocations', 'gpu_tree_leaves_vs_invocations',
                      'gpu_tree_bark_primitives', 'gpu_tree_leaves_primitives'):
            if {r[field] for r in samples + baseline} != {baseline[0][field]}:
                raise ValueError(f'Material control changed geometry: {field}')
        results.append(dict(mode=mode, samples=len(samples),
            width=rows[0]['width'],height=rows[0]['height'],shadows=rows[0]['shadows'],
            build_configuration=rows[0]['build_configuration'],
            draws=rows[0]['draws'], material_slots=rows[0]['instanced_material_slots'],
            shader_paths=rows[0]['shader_paths'],
            material_shader_states=rows[0]['material_shader_states'],
            bark_vs_invocations=samples[0]['gpu_tree_bark_vs_invocations'],
            leaves_vs_invocations=samples[0]['gpu_tree_leaves_vs_invocations'],
            bark_primitives=samples[0]['gpu_tree_bark_primitives'],
            leaves_primitives=samples[0]['gpu_tree_leaves_primitives'],
            bark_ps_invocations=statistics.mean(r['gpu_tree_bark_ps_invocations'] for r in samples),
            leaves_ps_invocations=statistics.mean(r['gpu_tree_leaves_ps_invocations'] for r in samples),
            bark_ms=statistics.mean(r['gpu_tree_bark_ms'] for r in samples),
            leaves_ms=statistics.mean(r['gpu_tree_leaves_ms'] for r in samples)))
        if rows[0]['shadows']:
            shadow=[r for r in rows if r.get('shadow_valid')]
            shadow_reference=[r for r in reference if r.get('shadow_valid')]
            if min(len(shadow),len(shadow_reference))<20:
                raise ValueError('Need at least 20 resolved shadow workload samples')
            if len({r['shadow_sample_id'] for r in shadow})!=len(shadow):
                raise ValueError('Repeated shadow GPU sample')
            for field in ('gpu_shadow_vs_invocations','gpu_shadow_primitives'):
                if {r[field] for r in shadow+shadow_reference}!={shadow_reference[0][field]} or shadow_reference[0][field]<=0:
                    raise ValueError('Material control changed shadow geometry')
            results[-1].update(shadow_ms=statistics.mean(r['gpu_shadow_ms'] for r in shadow),
                shadow_samples=len(shadow),shadow_ps_invocations=statistics.mean(r['gpu_shadow_ps_invocations'] for r in shadow))
    return results


def repeated(directories):
    manifests=[load_manifest(path) for path in directories]
    for manifest in manifests[1:]:
        for field in ('backend','inputs_sha256','executable_sha256','cmake_cache_sha256','platform'):
            if manifest[field]!=manifests[0][field]:
                raise ValueError(f'Repeated material runs changed {field}')
        normalize=lambda m: {k:v for k,v in m['environment'].items() if k!='ENGINE_FOREST_AUDIT_REVERSE'}
        if normalize(manifest)!=normalize(manifests[0]):
            raise ValueError('Repeated material runs changed environment')
    runs=[compare(path) for path in directories]
    for run in runs[1:]:
        for index,item in enumerate(run):
            for field in ('draws','material_slots','shader_paths','material_shader_states','width','height','shadows',
                          'build_configuration','bark_vs_invocations','leaves_vs_invocations','bark_primitives','leaves_primitives'):
                if item[field]!=runs[0][index][field]:
                    raise ValueError(f'Repeated material runs changed {field}')
    results=[]
    for index,mode in enumerate(('material-reference','material-mips','material-cheap')):
        result=dict(mode=mode, processes=len(runs))
        for stage in ('bark_ms','leaves_ms')+ (('shadow_ms',) if 'shadow_ms' in runs[0][index] else ()):
            values=[run[index][stage] for run in runs]
            result[stage]=dict(median=statistics.median(values),minimum=min(values),maximum=max(values))
            deltas=[run[index][stage]-run[0][stage] for run in runs]
            result[stage+'_paired_delta']=dict(median=statistics.median(deltas),minimum=min(deltas),maximum=max(deltas))
        results.append(result)
    return dict(backend=manifests[0]['backend'],directories=list(map(str,directories)),results=results)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--repeat',type=Path,nargs='+',help='Additional matched processes, allowing reverse case order')
    args = parser.parse_args()
    result = repeated([args.directory]+args.repeat) if args.repeat else compare(args.directory)
    (args.directory / 'material-costs.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
