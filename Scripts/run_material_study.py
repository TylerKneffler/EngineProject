"""Run repeated 1080p shadow-enabled material controls and lit motion validation."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import shutil

from compare_material_costs import compare, repeated
from run_forest_performance import ROOT, content_identity, sha256


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--backends',nargs='+',choices=('dx11','dx12','vulkan'),default=['dx11','dx12','vulkan'])
    parser.add_argument('--repeats',type=int,default=3)
    args=parser.parse_args()
    if args.repeats<3: parser.error('Use at least three independent processes per backend')
    root=args.directory.resolve()
    root.mkdir(parents=True,exist_ok=True)
    if any(root.iterdir()): parser.error('Use an empty output directory')
    # Freeze the validated executables so an unrelated workspace rebuild cannot
    # replace them between matched processes. Preserve their DLLs and build identity.
    runtime=root/'runtime/Release'
    runtime.mkdir(parents=True)
    source=ROOT/'build/Debug/Release'
    for path in list(source.glob('*.dll'))+[source/'ForestPerformanceAudit.exe',source/'TextureSamplingBackendTests.exe']:
        shutil.copy2(path,runtime/path.name)
    shutil.copy2(source.parent/'CMakeCache.txt',runtime.parent/'CMakeCache.txt')
    summaries=[]
    for backend in args.backends:
        paths=[]
        for index in range(args.repeats):
            path=root/f'{backend}-{index+1}'
            command=[sys.executable,str(ROOT/'Scripts/run_forest_performance.py'),str(path),
                '--backend',backend,'--width','1920','--shadows','--counters','--material-only',
                '--executable',str(runtime/'ForestPerformanceAudit.exe')]
            if index%2: command+=['--reverse']
            with (root/f'{backend}-{index+1}.log').open('w') as log:
                subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
            compare(path)
            paths.append(path)
            print(f'{backend}: completed process {index+1}/{args.repeats}',flush=True)
        summaries.append(repeated(paths))
    (root/'timing-summary.json').write_text(json.dumps(summaries,indent=2)+'\n')
    executable=runtime/'TextureSamplingBackendTests.exe'
    before=content_identity()
    identity=dict(executable_sha256=sha256(executable),inputs=before,complete=False)
    manifest=root/'visual-provenance.json'
    manifest.write_text(json.dumps(identity,indent=2)+'\n')
    for extended in (False,True):
        directory=root/('motion' if extended else 'still')
        command=[sys.executable,str(ROOT/'Scripts/Assets/validate_texture_sampling.py'),str(executable),
                 '--output',str(directory),'--material-study']
        if extended: command+=['--extended']
        with (root/('motion.log' if extended else 'still.log')).open('w') as log:
            subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    identity['inputs_unchanged']=before==content_identity()
    identity['executable_unchanged']=identity['executable_sha256']==sha256(executable)
    if not identity['inputs_unchanged'] or not identity['executable_unchanged']:
        raise ValueError('Visual validation inputs changed during capture')
    identity['artifacts_sha256']={p.relative_to(root).as_posix():sha256(p)
        for name in ('still','motion') for p in (root/name).rglob('*') if p.is_file()}
    identity['complete']=True
    manifest.write_text(json.dumps(identity,indent=2)+'\n')
    print('Material timing, still, motion and shadow validation complete',flush=True)


if __name__=='__main__':
    main()
