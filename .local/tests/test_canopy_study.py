"""Reject unmatched camera routes and misclassified experimental geometry."""
import json
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'Scripts'))
from run_canopy_study import summarize, validate_installed_baseline_log


def fixture(root, fault=None):
    for process in range(3):
        path=root/'timings/dx12'/str(process)/'dx12.jsonl'
        path.parent.mkdir(parents=True)
        rows=[]
        for view in ('entrance','overlook','zoom','motion'):
            for variant in ('baseline','candidate'):
                for frame in range(180):
                    row=dict(view=view,variant=variant,frame=frame,width=1920,height=1080,fov=60,
                        distance0=28,distance1=90,x=frame,y=2,z=3,backend='dx12',
                        executable_path='frozen.exe',build_configuration='Release',gpu_valid=True,
                        shadow_valid=True,gpu_sample_id=frame,shadow_sample_id=frame,
                        gpu_opaque_other_primitives=0,shadow_vs=100,shadow_primitives=100,shadow_ps=500)
                    for part in ('tree_bark','tree_leaves'):
                        for field,value in [('ms',2.),('vs_invocations',100),('primitives',100),('ps_invocations',500)]:
                            row['gpu_'+part+'_'+field]=value if variant=='baseline' else value*.9
                    row['gpu_shadow_ms']=1.
                    if fault and process==0 and variant=='candidate' and frame==0: fault(row)
                    rows.append(row)
        path.write_text('\n'.join(json.dumps(r) for r in rows))


class CanopyStudyTests(unittest.TestCase):
    def provenance(self, root):
        recordings={p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'timings').rglob('*.jsonl')}
        data=dict(baseline='installed',complete=True,inputs_unchanged=True,candidates_unchanged=True,runtime_unchanged=True,timing_recordings_sha256=recordings)
        (root/'timing-provenance.json').write_text(json.dumps(data))

    def test_installed_baseline_requires_complete_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); fixture(root)
            (root/'timing-provenance.json').write_text(json.dumps(dict(baseline='installed',complete=False)))
            with self.assertRaisesRegex(ValueError,'Incomplete installed'): summarize(root)

    def test_installed_baseline_rejects_mutated_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); fixture(root); self.provenance(root)
            path=next((root/'timings').rglob('*.jsonl'))
            path.write_text(path.read_text()+'\n')
            with self.assertRaisesRegex(ValueError,'changed after'): summarize(root)

    def test_installed_baseline_accepts_verified_recordings(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); fixture(root); self.provenance(root)
            self.assertEqual(len(summarize(root)),4)

    def baseline_log(self):
        return '\n'.join(f'Large_oak_tree_{i:03} variant=baseline prefab=Engine/Core/Assets/Prefabs/Woodland/Oaks/Large_oak_tree_{i:03}_Canopy.prefab validatedCandidateTriangles=9000' for i in range(1,5))

    def test_accepts_all_four_installed_baselines(self):
        validate_installed_baseline_log(self.baseline_log())

    def test_rejects_original_lod1_baseline(self):
        with self.assertRaisesRegex(ValueError,'Missing installed'):
            validate_installed_baseline_log(self.baseline_log().replace('_Canopy.prefab','_LOD1.prefab'))

    def test_rejects_missing_baseline_variety(self):
        with self.assertRaisesRegex(ValueError,'004'):
            validate_installed_baseline_log('\n'.join(self.baseline_log().splitlines()[:3]))
    def test_matched_repeated_processes_and_smaller_workload(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); fixture(root)
            result=summarize(root)
            self.assertEqual(len(result),4)
            self.assertEqual(result[0]['processes'],3)
            self.assertLess(result[0]['candidate']['tree_shadow_ms'],result[0]['baseline']['tree_shadow_ms'])

    def test_rejects_route_change(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); fixture(root,lambda r:r.update(x=-100))
            with self.assertRaisesRegex(ValueError,'changed x'): summarize(root)

    def test_rejects_geometry_moved_to_other_opaque(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); fixture(root,lambda r:r.update(gpu_opaque_other_primitives=10000))
            with self.assertRaisesRegex(ValueError,'timing category'): summarize(root)


if __name__=='__main__': unittest.main()
