import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Scripts'))
import compare_foliage_overdraw as study
from summarize_forest_performance import CASE_FIELDS


class FoliageComparisonTests(unittest.TestCase):
    def rows(self):
        rows = []
        base = {field: 1 for field in CASE_FIELDS}
        base.update(backend='dx12', view='entrance', width=1920, height=1080,
                    executable_path=__file__, shadows=True, candidate_triangles=100,
                    draws=10, instanced_material_slots=20, instanced_materials=20,
                    shader_paths=1, material_shader_states=1)
        for mode in study.MODES:
            for i in range(24):
                row = dict(base, mode=mode, scissor_mode='empty' if mode.endswith('vertex-only') else 'full',
                           gpu_valid=True, gpu_workload_valid=True, gpu_sample_id=i+1,
                           shadow_valid=True, shadow_sample_id=i+1,
                           gpu_tree_leaves_ms=2., gpu_tree_leaves_vs_invocations=300,
                           gpu_tree_leaves_primitives=100,
                           gpu_tree_leaves_ps_invocations=0 if mode.endswith('vertex-only') else 1000,
                           gpu_shadow_ms=1., gpu_shadow_vs_invocations=300*(i%4+1),
                           gpu_shadow_primitives=100*(i%4+1), gpu_shadow_ps_invocations=500)
                rows.append(row)
        return rows

    def compare(self, rows):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            (path/'dx12.jsonl').write_text('\n'.join(map(json.dumps, rows)))
            manifest = dict(backend='dx12', executable_path=__file__, executable_sha256='exe', inputs_sha256='inputs')
            with patch.object(study, 'load_manifest', return_value=manifest):
                return study.compare(path)

    def test_accepts_matched_cascade_cycle(self):
        case = self.compare(self.rows())['cases'][0]
        self.assertEqual(len(case['shadow_workload_distribution']), 4)
        self.assertEqual(case['color_delta_ms']['leaves-solid'], 0)

    def test_rejects_changed_shadow_distribution(self):
        rows = self.rows()
        rows[24]['gpu_shadow_vs_invocations'] = 600
        rows[24]['gpu_shadow_primitives'] = 200
        with self.assertRaisesRegex(ValueError, 'refresh distribution'):
            self.compare(rows)

    def test_rejects_geometry_change(self):
        rows = self.rows()
        rows[24]['gpu_tree_leaves_primitives'] += 1
        with self.assertRaisesRegex(ValueError, 'executed geometry'):
            self.compare(rows)

    def test_rejects_duplicate_samples(self):
        rows = self.rows()
        rows[1]['gpu_sample_id'] = rows[0]['gpu_sample_id']
        with self.assertRaisesRegex(ValueError, 'Repeated GPU'):
            self.compare(rows)

    def test_rejects_nonfinite_timing(self):
        rows = self.rows()
        rows[0]['gpu_tree_leaves_ms'] = float('nan')
        with self.assertRaisesRegex(ValueError, 'Invalid workload'):
            self.compare(rows)

    def test_rejects_identity_changes(self):
        for field in ('camera_position', 'draws', 'backend', 'scissor_mode'):
            with self.subTest(field=field):
                rows = copy.deepcopy(self.rows())
                rows[24][field] = 'changed'
                with self.assertRaises(ValueError):
                    self.compare(rows)

    def test_rejects_missing_control(self):
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            self.compare(self.rows()[:72])


if __name__ == '__main__':
    unittest.main()
