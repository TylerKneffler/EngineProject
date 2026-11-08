import json
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Scripts'))
import compare_material_costs as study


class MaterialCostTests(unittest.TestCase):
    def run_comparison(self, changed=None):
        rows = []
        for mode in ('material-reference', 'material-mips', 'material-cheap'):
            for index in range(20):
                row = dict(mode=mode, gpu_valid=True, gpu_workload_valid=True,
                    backend='dx12',executable_path=__file__,build_configuration='Release',assertions_enabled=False,
                    gpu_sample_id=index+1,shadow_valid=True,shadow_sample_id=index+1,
                    gpu_shadow_ms=1,gpu_shadow_vs_invocations=30,gpu_shadow_primitives=10,gpu_shadow_ps_invocations=50,
                    gpu_tree_bark_vs_invocations=30, gpu_tree_leaves_vs_invocations=60,
                    gpu_tree_bark_primitives=10, gpu_tree_leaves_primitives=20,
                    gpu_tree_bark_ps_invocations=40, gpu_tree_leaves_ps_invocations=80,
                    gpu_tree_bark_ms=1, gpu_tree_leaves_ms=2)
                row.update({k: 1 for k in ('width', 'height', 'view', 'camera_position',
                    'camera_target', 'impostor_distance', 'shadows', 'candidate_triangles',
                    'draws', 'instanced_material_slots', 'instanced_materials', 'shader_paths', 'material_shader_states')})
                if changed and mode == 'material-cheap':
                    row[changed] = 2
                rows.append(row)
        with tempfile.TemporaryDirectory() as name, patch.object(study, 'load_manifest',return_value=dict(backend='dx12',executable_path=__file__)):
            directory = Path(name)
            (directory / 'test.jsonl').write_text('\n'.join(map(json.dumps, rows)))
            return study.compare(directory)

    def test_controls_match(self):
        self.assertEqual(len(self.run_comparison()), 3)

    def test_changed_work_is_rejected(self):
        for field in ('draws', 'instanced_material_slots', 'shader_paths', 'material_shader_states',
                      'gpu_tree_leaves_primitives', 'gpu_tree_bark_vs_invocations', 'gpu_sample_id',
                      'gpu_shadow_vs_invocations','shadow_sample_id', 'backend','executable_path','build_configuration'):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.run_comparison(field)

    def test_repeated_build_mismatch_rejected(self):
        manifests=[dict(backend='dx12',inputs_sha256='assets',executable_sha256='a',
            cmake_cache_sha256='cache',platform='machine',environment={}) for _ in range(2)]
        manifests[1]['executable_sha256']='b'
        with patch.object(study,'load_manifest',side_effect=manifests),self.assertRaises(ValueError):
            study.repeated([Path('a'),Path('b')])

    def test_reverse_order_is_allowed_but_other_environment_changes_are_not(self):
        manifest=dict(backend='dx12',inputs_sha256='assets',executable_sha256='exe',
            cmake_cache_sha256='cache',platform='machine',environment={})
        reverse=copy.deepcopy(manifest)
        reverse['environment']['ENGINE_FOREST_AUDIT_REVERSE']='1'
        row=dict(draws=1,material_slots=1,shader_paths=1,material_shader_states=1,width=1920,height=1080,
            shadows=True,build_configuration='Release',bark_vs_invocations=30,leaves_vs_invocations=60,
            bark_primitives=10,leaves_primitives=20,bark_ms=1,leaves_ms=2,shadow_ms=3)
        with patch.object(study,'load_manifest',side_effect=[manifest,reverse]),patch.object(study,'compare',return_value=[row]*3):
            self.assertEqual(study.repeated([Path('a'),Path('b')])['results'][0]['processes'],2)
        reverse['environment']['ENGINE_TEXTURE_MAX_DIMENSION']='256'
        with patch.object(study,'load_manifest',side_effect=[manifest,reverse]),self.assertRaises(ValueError):
            study.repeated([Path('a'),Path('b')])


if __name__ == '__main__':
    unittest.main()
