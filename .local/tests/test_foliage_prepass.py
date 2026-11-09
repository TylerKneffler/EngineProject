import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts'))
import compare_foliage_prepass as study
from summarize_forest_performance import CASE_FIELDS


class PrepassComparisonTests(unittest.TestCase):
    def compare(self, change=None, image_change=None, record_rejected=False):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            base = {f: 1 for f in CASE_FIELDS}
            base.update(backend='dx12', view='entrance', width=4, height=4, executable_path=__file__,
                        candidate_triangles=100, draws=10, instanced_material_slots=20,
                        instanced_materials=20, shader_paths=1, material_shader_states=1,
                        full_shadow_refresh=True, shadows=True, frame_ms=10.)
            rows = []
            for mode in study.MODES:
                prepass = mode == 'leaves-alpha-prepass'
                vertex = mode.endswith('vertex-only')
                for i in range(24):
                    rows.append(dict(base, mode=mode, alpha_prepass=prepass,
                                     equal_depth_ready=True,
                                     prepass_draws=10 if prepass else 0, total_color_draws=20 if prepass else 10,
                                     scissor_mode='empty' if vertex else 'full', gpu_sample_id=i+1,
                                     shadow_sample_id=i+1, gpu_valid=True, gpu_workload_valid=True,
                                     shadow_valid=True, gpu_tree_leaves_ms=3. if prepass else 2.,
                                     gpu_tree_leaves_vs_invocations=600 if prepass else 300,
                                     gpu_tree_leaves_primitives=200 if prepass else 100,
                                     gpu_tree_leaves_ps_invocations=0 if vertex else 1000,
                                     gpu_shadow_ms=1., gpu_shadow_vs_invocations=300,
                                     gpu_shadow_primitives=100, gpu_shadow_ps_invocations=500))
            if change:
                change(rows)
            (directory/'dx12.jsonl').write_text('\n'.join(map(json.dumps, rows)))
            manifest = dict(backend='dx12', executable_path=__file__, executable_sha256='exe',
                            inputs_sha256='inputs', captures_sha256={})
            for mode in study.MODES[:3]:
                pixels = (b'\xff\xff\xff' if mode == 'leaves-alpha-mask' else b'\x30\x50\x20')*16
                if image_change:
                    pixels = image_change(mode, pixels)
                name = f'dx12-entrance-{mode}-90-4.ppm'
                data = b'P6\n4 4\n255\n'+pixels
                (directory/name).write_bytes(data)
                manifest['captures_sha256'][name] = hashlib.sha256(data).hexdigest()
            with patch.object(study, 'load_manifest', return_value=manifest):
                return study.compare(directory, allow_image_differences=record_rejected)

    def test_accepts_combined_two_pass_cost(self):
        result = self.compare()['cases'][0]
        self.assertTrue(result['image_byte_identical'])
        self.assertEqual(result['combined_prepass_delta_ms'], 1)

    def test_rejects_missing_prepass_geometry(self):
        with self.assertRaisesRegex(ValueError, 'executed geometry'):
            self.compare(lambda rows: rows[24].update(gpu_tree_leaves_primitives=100))

    def test_rejects_missing_submission_accounting(self):
        with self.assertRaisesRegex(ValueError, 'submission'):
            self.compare(lambda rows: rows[24].update(prepass_draws=0))

    def test_rejects_changed_final_image(self):
        with self.assertRaisesRegex(ValueError, 'final color/coverage'):
            self.compare(image_change=lambda mode, data: b'\x00'+data[1:] if mode == 'leaves-alpha-prepass' else data)

    def test_rejects_inactive_mask_shader(self):
        with self.assertRaisesRegex(ValueError, 'not active'):
            self.compare(image_change=lambda mode, data: b'\x30\x50\x20'*16 if mode == 'leaves-alpha-mask' else data)

    def test_rejects_shadow_geometry_change(self):
        with self.assertRaisesRegex(ValueError, 'shadow workload'):
            self.compare(lambda rows: rows[24].update(gpu_shadow_primitives=101))

    def test_rejects_nonzero_vertex_control_fragments(self):
        with self.assertRaisesRegex(ValueError, 'executed fragments'):
            self.compare(lambda rows: rows[72].update(gpu_tree_leaves_ps_invocations=1))

    def test_rejects_camera_change(self):
        with self.assertRaisesRegex(ValueError, 'camera_position'):
            self.compare(lambda rows: rows[24].update(camera_position='different'))

    def test_rejects_unready_equal_depth_pipeline(self):
        with self.assertRaisesRegex(ValueError, 'not active'):
            self.compare(lambda rows: rows[24].update(equal_depth_ready=False))

    def test_accepts_unchanged_colored_sky(self):
        self.compare(image_change=lambda mode, data: data[:24]+b'\x80\xb0\xe0'*8)

    def test_rejects_changed_background_under_mask(self):
        with self.assertRaisesRegex(ValueError, 'background coverage'):
            self.compare(image_change=lambda mode, data: data[:24]+b'\x80\xb0\xe0'*8 if mode == 'leaves-alpha-mask' else data)

    def test_recording_rejected_image_never_marks_quality_accepted(self):
        result = self.compare(record_rejected=True,
            image_change=lambda mode, data: b'\x00'+data[1:] if mode == 'leaves-alpha-prepass' else data)['cases'][0]
        self.assertFalse(result['quality_accepted'])
        self.assertFalse(result['image_byte_identical'])
        self.assertEqual(result['changed_pixels'], 1)


if __name__ == '__main__':
    unittest.main()
