import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('material_visual',ROOT/'Scripts/Assets/validate_texture_sampling.py')
visual=importlib.util.module_from_spec(spec)
spec.loader.exec_module(visual)


class MaterialVisualTests(unittest.TestCase):
    def analyze(self,mutation=None):
        motion=[]
        for frame in range(180):
            for bias in (0,1):
                motion.append(dict(foliage=0,frame=frame,bias=bias,time=frame/60,
                    distance=1.2 if frame<10 else 10.2,angle=60,coverage=100,
                    mask_xor=1,mask_union=110,common_pixels=90,rgb_delta=30,
                    paired_rgb_delta=30 if bias and frame>=10 else 0))
        shadows=[dict(foliage=0,angle=a,distance=d,bias=b,masked_pixels=20,
            opaque_pixels=40,cutout_holes=20,wind_changed_pixels=0)
            for a in (0,60,80) for d in (3,5.5,8) for b in (0,1)]
        report=dict(material_study=True,motion=motion,shadows=shadows,diagonal_shadow=False)
        if mutation: mutation(report)
        with tempfile.TemporaryDirectory() as name,patch.object(visual,'read_image',return_value=bytes([255]*20+[0]*10)):
            path=Path(name)
            (path/'dx12.json').write_text(json.dumps(report))
            return visual.analyze_extended(path,'dx12')

    def test_lit_candidate_and_coverage_are_exercised(self):
        self.assertEqual(self.analyze()['identical_motion_pairs'],180)

    def test_noop_candidate_rejected(self):
        with self.assertRaises(ValueError):
            self.analyze(lambda r:[x.update(paired_rgb_delta=0) for x in r['motion']])

    def test_changed_alpha_coverage_rejected(self):
        with self.assertRaises(ValueError):
            self.analyze(lambda r:r['motion'][1].update(coverage=99))

    def test_lighting_pop_rejected(self):
        with self.assertRaises(ValueError):
            self.analyze(lambda r:r['motion'][21].update(paired_rgb_delta=3000))

    def test_incomplete_motion_rejected(self):
        with self.assertRaises(ValueError):
            self.analyze(lambda r:r['motion'].pop())


if __name__=='__main__': unittest.main()
