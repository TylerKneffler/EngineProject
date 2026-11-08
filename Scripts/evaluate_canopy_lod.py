"""Measure real screen selector decisions and summarize isolated canopy captures."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
from scipy.ndimage import binary_fill_holes, binary_erosion, distance_transform_edt

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.local/canopy-deps'))
sys.path.insert(0, str(ROOT / 'Scripts/Assets'))
from PIL import Image
from audit_oak_lods import inventory, resolve


def selection_study(output):
    executable = ROOT / 'build/Release/CanopyLodStudy.exe'
    results = []
    assets=inventory()
    for row in assets:
        if not row['prefab'].startswith('Large_oak_tree_') or '_LOD1.' not in row['prefab']: continue
        points = []
        name = row['prefab'].replace('_LOD1.prefab', '')
        # Match the renderer's stable union of every resident band, including
        # detailed geometry and the authored far impostor, rather than LOD1 alone.
        chain=[r for r in assets if r['prefab'] in {name+'_LOD0.prefab',name+'_LOD1.prefab',name+'_Impostor.prefab'}]
        for part in [p for r in chain for p in r['parts']]:
            raw = resolve(part['mesh']).read_bytes()
            count = int.from_bytes(raw[8:12], 'little')
            points.append(np.frombuffer(raw, '<f4', count*66, 16).reshape(-1,66)[:,:3])
        points = np.concatenate(points)
        bounds = [*points.min(0), *points.max(0)]
        text = subprocess.check_output([str(executable), *map(str,bounds)], text=True)
        (output/(name+'-selection.csv')).write_text(text)
        groups = {}
        for sample in csv.DictReader(io.StringIO(text)):
            key = tuple(sample[k] for k in ('elevation','fov','scale','hysteresis'))
            groups.setdefault(key, []).append(sample)
        for key, samples in groups.items():
            changes = []
            jitter_changes = 0
            previous = None
            for sample in samples:
                if previous is not None and sample['lod'] != previous['lod']:
                    if sample['phase'] == previous['phase'] == 'jitter': jitter_changes += 1
                    if sample['phase'] in ('out','in') and previous['phase'] != 'jitter':
                        changes.append(dict(phase=sample['phase'], distance=float(sample['distance']),
                                            pixels=float(sample['pixels']), lod=int(sample['lod'])))
                previous = sample
            results.append(dict(name=name, elevation=float(key[0]), fov=float(key[1]),
                                scale=float(key[2]), hysteresis=float(key[3]),
                                threshold0=float(samples[0]['threshold0']), threshold1=float(samples[0]['threshold1']),
                                jitter_changes=jitter_changes, transitions=changes))
    return dict(selector_executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(), measurements=results)


def renderer_study(output):
    results = []
    for reference in sorted(output.glob('renderer/*/*/*-baseline.ppm')):
        candidate_path = reference.with_name(reference.name.replace('-baseline.ppm','-candidate.ppm'))
        if not candidate_path.exists(): continue
        a, b = [np.asarray(Image.open(p)).astype(np.int16) for p in (reference,candidate_path)]
        delta = np.abs(a-b)
        row = dict(capture=str(reference.relative_to(output)),
                   changed_fraction=float(np.any(delta>3,axis=2).mean()),
                   mean_channel_delta=float(delta.mean()), max_channel_delta=int(delta.max()))
        frame=int(reference.stem.split('-')[-2])
        if frame>0:
            prefix='-'.join(reference.stem.split('-')[:-2])
            previous=[reference.with_name(f'{prefix}-{frame-1}-{variant}.ppm') for variant in ('baseline','candidate')]
            if all(p.exists() for p in previous):
                before=[np.asarray(Image.open(p)).astype(np.int16) for p in previous]
                temporal=np.abs((b-before[1])-(a-before[0]))
                row['temporal_residual_mean_channel_delta']=float(temporal.mean())
                row['temporal_residual_changed_fraction']=float(np.any(temporal>3,axis=2).mean())
        repeat = reference.with_name(reference.stem+'-repeat.ppm')
        if repeat.exists():
            noise = np.abs(a-np.asarray(Image.open(repeat)).astype(np.int16))
            row['unchanged_repeat_fraction'] = float(np.any(noise>3,axis=2).mean())
            candidate_repeat=candidate_path.with_name(candidate_path.stem+'-repeat.ppm')
            if candidate_repeat.exists():
                candidate_noise=np.abs(b-np.asarray(Image.open(candidate_repeat)).astype(np.int16))
                row['candidate_unchanged_repeat_fraction']=float(np.any(candidate_noise>3,axis=2).mean())
            diff = np.clip(delta*5,0,255).astype('uint8')
            sheet = np.concatenate((a.astype('uint8'),b.astype('uint8'),diff),axis=1)
            Image.fromarray(sheet).save(reference.with_name(reference.name.replace('-baseline.ppm','-comparison.png')))
        results.append(row)
    # Preserve complete matched motion sequences as baseline | candidate GIFs.
    for folder in output.glob('renderer/*/*'):
        for view in ('entrance','overlook'):
            for fov in (60,30):
                frames = []
                for frame in range(12):
                    prefix=f'{view}-fov{fov}-{frame}'
                    paths=[folder/(prefix+'-'+variant+'.ppm') for variant in ('baseline','candidate')]
                    if not all(p.exists() for p in paths): break
                    frames.append(Image.fromarray(np.concatenate([np.asarray(Image.open(p)) for p in paths],axis=1)))
                if len(frames)==12:
                    frames[0].save(folder/f'{view}-fov{fov}-motion.gif',save_all=True,append_images=frames[1:],duration=150,loop=0)
                    Image.fromarray(np.concatenate([np.asarray(frames[i]) for i in (0,5,11)],axis=0)).save(
                        folder/f'{view}-fov{fov}-storyboard.png')
    return results


def software_silhouettes(output):
    results=[]
    for path in sorted(output.glob('*/*.png')):
        image=np.asarray(Image.open(path))
        if image.shape[1]!=768 or image.shape[0]!=384: continue
        left,right=np.split(image,2,axis=1)
        masks=[binary_fill_holes(np.any(pixels!=[172,198,218],axis=2)) for pixels in (left,right)]
        a,b=masks
        boundaries=[mask & ~binary_erosion(mask) for mask in masks]
        distances=np.concatenate([distance_transform_edt(~boundaries[0])[boundaries[1]],
                                  distance_transform_edt(~boundaries[1])[boundaries[0]]])
        results.append(dict(capture=str(path.relative_to(output)),
                            filled_silhouette_iou=float(np.count_nonzero(a&b)/max(1,np.count_nonzero(a|b))),
                            contour_distance_p95_pixels=float(np.quantile(distances,.95))))
    return results


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--capture-executable',type=Path,default=ROOT/'build/Release/TerrainPrefabAndOakTests.exe')
    args=parser.parse_args()
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    result=dict(selection=selection_study(output), renderer=renderer_study(output),
                software_still_silhouettes=software_silhouettes(output),
                status='Diagnostic evidence only; authored 28/90 m retained. No timing claim.')
    paths=[ROOT/'Engine/Core/Assets/Scenes/terrain/oak_meadow.scene',
           args.capture_executable.resolve(),
           ROOT/'Scripts/Assets/build_canopy_lod.py',Path(__file__).resolve()]
    paths.extend(output.glob('*/*.mesh'))
    paths.extend(output.glob('*/*.prefab'))
    result['sha256']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    (output/'evaluation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f"Measured {len(result['selection']['measurements'])} selector cases and {len(result['renderer'])} renderer pairs")


if __name__=='__main__': main()
