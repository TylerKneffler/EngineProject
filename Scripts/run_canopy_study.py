"""Freeze the canopy test runtime and collect matched quality/GPU evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess

from run_forest_performance import ROOT, content_identity, identity_digest, sha256


def candidate_identity(root):
    return {str(p.relative_to(ROOT)):sha256(p) for pattern in ('*/*.mesh','*/*.prefab') for p in root.glob(pattern)}


def validate_installed_baseline_log(text):
    for variety in range(1,5):
        name=f'Large_oak_tree_{variety:03}'
        expected=f'{name} variant=baseline prefab=Engine/Core/Assets/Prefabs/Woodland/Oaks/{name}_Canopy.prefab '
        if expected not in text:
            raise ValueError('Missing installed baseline evidence for '+name)


def summarize(directory):
    provenance=directory/'timing-provenance.json'
    if provenance.exists():
        manifest=json.loads(provenance.read_text())
        if manifest.get('baseline')=='installed':
            if not all(manifest.get(k) for k in ('complete','inputs_unchanged','candidates_unchanged','runtime_unchanged')):
                raise ValueError('Incomplete installed-baseline timing provenance')
            recordings=manifest.get('timing_recordings_sha256')
            if not recordings or set(recordings)!={p.relative_to(directory).as_posix() for p in (directory/'timings').rglob('*.jsonl')}:
                raise ValueError('Missing timing recording identities')
            for name,digest in recordings.items():
                path=(directory/name).resolve()
                path.relative_to(directory.resolve())
                if sha256(path)!=digest: raise ValueError('Timing recording changed after capture')
    summaries=[]
    for backend in ('dx11','dx12','vulkan'):
        runs=[]
        for path in sorted((directory/'timings'/backend).glob('*/*.jsonl')):
            groups={}
            for line in path.read_text().splitlines():
                row=json.loads(line)
                groups.setdefault((row['view'],row['variant']),[]).append(row)
            result=[]
            for view in ('entrance','overlook','zoom','motion'):
                baseline,candidate=[groups[(view,v)] for v in ('baseline','candidate')]
                if len(baseline)!=180 or len(candidate)!=180: raise ValueError('Incomplete route')
                for a,b in zip(baseline,candidate):
                    for key in ('frame','width','height','fov','distance0','distance1','x','y','z','backend','executable_path','build_configuration'):
                        if a[key]!=b[key]: raise ValueError('Comparison changed '+key)
                valid=[]
                for rows in (baseline,candidate):
                    samples=[r for r in rows if r['gpu_valid'] and r['shadow_valid']]
                    if len(samples)<160: raise ValueError('Too few completed GPU/workload samples')
                    for key in ('gpu_sample_id','shadow_sample_id'):
                        if len({r[key] for r in samples})!=len(samples): raise ValueError('Repeated GPU query')
                    valid.append(samples)
                entry=dict(view=view)
                other=[statistics.mean(r['gpu_opaque_other_primitives'] for r in samples) for samples in valid]
                if abs(other[0]-other[1])>1:
                    raise ValueError('Experimental tree geometry escaped its timing category')
                for variant,samples in zip(('baseline','candidate'),valid):
                    fields=('gpu_tree_bark_ms','gpu_tree_leaves_ms','gpu_shadow_ms',
                            'gpu_tree_bark_vs_invocations','gpu_tree_leaves_vs_invocations',
                            'gpu_tree_bark_primitives','gpu_tree_leaves_primitives','shadow_vs','shadow_primitives',
                            'gpu_tree_bark_ps_invocations','gpu_tree_leaves_ps_invocations')
                    if all('shadow_ps' in r for rows in valid for r in rows): fields+=('shadow_ps',)
                    entry[variant]={key:statistics.mean(r[key] for r in samples) for key in fields}
                    entry[variant]['samples']=len(samples)
                    entry[variant]['tree_shadow_ms']=sum(entry[variant][key] for key in fields[:3])
                result.append(entry)
            runs.append(result)
        if not runs: continue
        if len(runs)<3: raise ValueError('Need three independent paired processes')
        for index,view in enumerate(('entrance','overlook','zoom','motion')):
            entry=dict(backend=backend,view=view,processes=len(runs))
            for variant in ('baseline','candidate'):
                entry[variant]={key:statistics.median(run[index][variant][key] for run in runs) for key in runs[0][index][variant]}
            entry['paired_tree_shadow_ms_delta']=[run[index]['candidate']['tree_shadow_ms']-run[index]['baseline']['tree_shadow_ms'] for run in runs]
            summaries.append(entry)
    (directory/'timing-summary.json').write_text(json.dumps(summaries,indent=2)+'\n')
    return summaries


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate',type=Path)
    parser.add_argument('--backends',nargs='+',default=['dx11','dx12','vulkan'])
    parser.add_argument('--quality-only',action='store_true')
    parser.add_argument('--timing-only',action='store_true')
    parser.add_argument('--study-output',type=Path,help='Use a fresh frozen runtime/evidence directory')
    parser.add_argument('--baseline',choices=('original','installed'),default='original')
    parser.add_argument('--runtime-source',type=Path,default=ROOT/'build/Release')
    args=parser.parse_args()
    candidate=args.candidate.resolve()
    if not all((candidate/f'Large_oak_tree_{i:03}'/f'Large_oak_tree_{i:03}_CanopyCandidate.prefab').exists() for i in range(1,5)):
        parser.error('All four completed candidate prefabs are required')
    directory=args.study_output.resolve() if args.study_output else candidate/'validation'
    directory.mkdir(parents=True,exist_ok=True)
    runtime=directory/'runtime/Release'; runtime.mkdir(parents=True,exist_ok=True)
    # Never recopy into an existing frozen runtime during a resumed suite.
    if not (runtime/'ForestPerformanceAudit.exe').exists():
        source=args.runtime_source.resolve()
        for path in [*source.glob('*.dll'),source/'ForestPerformanceAudit.exe',source/'TerrainPrefabAndOakTests.exe']:
            shutil.copy2(path,runtime/path.name)
    before=content_identity(); candidates=candidate_identity(candidate)
    manifest=dict(inputs_sha256=identity_digest(before),inputs=before,candidates=candidates,
                  baseline=args.baseline,
                  runtime={p.name:sha256(p) for p in runtime.iterdir() if p.is_file()},
                  platform=dict(node=platform.node(),system=platform.platform()),complete=False)
    manifest_path=directory/('quality-provenance.json' if args.quality_only else 'timing-provenance.json')
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    clean={k:v for k,v in os.environ.items() if not k.startswith(('ENGINE_','VK_','DX','D3D'))}
    if args.baseline=='installed': clean['ENGINE_OAK_CANOPY_INSTALLED_BASELINE']='1'
    for backend in args.backends:
        if not args.timing_only:
            env=dict(clean,ENGINE_OAK_CANOPY_ROOT=str(candidate),ENGINE_OAK_DX12_VALIDATION='1')
            with (directory/(backend+'-quality.log')).open('w') as log:
                subprocess.run([str(runtime/'TerrainPrefabAndOakTests.exe'),backend],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
            if args.baseline=='installed':
                validate_installed_baseline_log((candidate/'renderer'/backend/'1'/(backend+'.log')).read_text())
            print(backend+' quality complete',flush=True)
        if not args.quality_only:
            for index in range(3):
                output=directory/'timings'/backend/str(index+1)
                output.mkdir(parents=True,exist_ok=True)
                env=dict(clean,ENGINE_FOREST_AUDIT_CANOPY=str(candidate),ENGINE_FOREST_AUDIT_OUTPUT=str(output),
                         ENGINE_FOREST_AUDIT_WIDTH='1920',ENGINE_FOREST_AUDIT_SHADOWS='1',ENGINE_GPU_WORKLOAD_COUNTERS='1')
                if index%2: env['ENGINE_FOREST_AUDIT_REVERSE']='1'
                with (output/'process.log').open('w') as log:
                    subprocess.run([str(runtime/'ForestPerformanceAudit.exe'),backend],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
                if args.baseline=='installed': validate_installed_baseline_log((output/'process.log').read_text())
                print(f'{backend} paired timing {index+1}/3 complete',flush=True)
    manifest['inputs_unchanged']=before==content_identity()
    manifest['candidates_unchanged']=candidates==candidate_identity(candidate)
    manifest['runtime_unchanged']=manifest['runtime']=={p.name:sha256(p) for p in runtime.iterdir() if p.is_file()}
    manifest['quality_images_sha256']={str(p.relative_to(candidate)):sha256(p) for p in sorted((candidate/'renderer').rglob('*.ppm'))}
    manifest['timing_recordings_sha256']={p.relative_to(directory).as_posix():sha256(p) for p in sorted((directory/'timings').rglob('*.jsonl'))}
    manifest['complete']=True
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    if not all(manifest[key] for key in ('inputs_unchanged','candidates_unchanged','runtime_unchanged')):
        raise ValueError('Study inputs changed during capture')
    if not args.quality_only: summarize(directory)


if __name__=='__main__': main()
