"""Reject unmatched camera routes and misclassified experimental geometry."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'Scripts'))
from run_canopy_study import summarize


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
                        gpu_opaque_other_primitives=0,shadow_vs=100,shadow_primitives=100)
                    for part in ('tree_bark','tree_leaves'):
                        for field,value in [('ms',2.),('vs_invocations',100),('primitives',100)]:
                            row['gpu_'+part+'_'+field]=value if variant=='baseline' else value*.9
                    row['gpu_shadow_ms']=1.
                    if fault and process==0 and variant=='candidate' and frame==0: fault(row)
                    rows.append(row)
        path.write_text('\n'.join(json.dumps(r) for r in rows))


class CanopyStudyTests(unittest.TestCase):
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
