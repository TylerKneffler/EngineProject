import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('forest_summary', ROOT / 'Scripts/summarize_forest_performance.py')
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)
sys.path.insert(0, str(ROOT / 'Scripts'))
import editor_performance_provenance as editor_provenance


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.manifest = dict(inputs_sha256='assets', environment={}, platform={'gpu': 'device'}, cmake_cache_sha256='cache')
        self.row = {field: 1 for field in summary.CASE_FIELDS}
        self.row['build_configuration'] = 'Release'

    def test_missing_identity_cannot_match(self):
        for field in summary.CASE_FIELDS:
            row = dict(self.row)
            del row[field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                summary.validate_comparison(self.manifest, self.manifest, [row])

    def test_assets_driver_and_environment_must_match(self):
        for field in ('inputs_sha256', 'environment', 'platform', 'cmake_cache_sha256'):
            baseline = dict(self.manifest)
            baseline[field] = 'different'
            with self.subTest(field=field), self.assertRaises(ValueError):
                summary.validate_comparison(self.manifest, baseline, [self.row])

    def test_executable_may_change_for_a_before_after_build(self):
        before = dict(self.manifest, executable_sha256='before')
        after = dict(self.manifest, executable_sha256='after')
        summary.validate_comparison(after, before, [self.row])

    def test_modified_recording_cannot_match(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            recording = path / 'dx12.jsonl'
            recording.write_bytes(b'original')
            manifest = dict(self.manifest, schema=1, inputs_unchanged=True, executable_unchanged=True,
                            returncode=0, executable_sha256='binary',
                            recordings_sha256={'dx12.jsonl': hashlib.sha256(b'original').hexdigest()})
            (path / 'provenance.json').write_text(json.dumps(manifest))
            summary.load_manifest(path)
            recording.write_bytes(b'changed')
            with self.assertRaises(ValueError):
                summary.load_manifest(path)

    def test_camera_and_build_prevent_pooling(self):
        for field in ('height', 'build_configuration', 'camera_position', 'workload_counters_requested'):
            other = dict(self.row)
            other[field] = 'changed'
            with self.subTest(field=field):
                self.assertNotEqual(summary.case_key(self.row), summary.case_key(other))

    def test_stale_gpu_samples_are_excluded_from_stage_and_counter_means(self):
        base = dict(self.row, backend='dx12', view='entrance', mode='leaves', width=960,
                    frame_ms=4, gpu_ms=2, gpu_valid=True, gpu_workload_valid=True,
                    gpu_tree_leaves_ms=2, gpu_tree_leaves_vs_invocations=100,
                    gpu_tree_leaves_ps_invocations=200)
        stale = dict(base, gpu_valid=False, gpu_tree_leaves_ms=999,
                     gpu_tree_leaves_vs_invocations=999, gpu_tree_leaves_ps_invocations=999)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'dx12.jsonl').write_text('\n'.join(json.dumps(row) for row in (base, stale)))
            subprocess.run([sys.executable, str(ROOT / 'Scripts/summarize_forest_performance.py'), directory],
                           check=True, capture_output=True)
            row = json.loads((path / 'summary.json').read_text())[0]
            self.assertEqual(row['gpu_samples'], 1)
            self.assertEqual(row['phase_mean_ms']['gpu_tree_leaves_ms'], 2)
            self.assertEqual(row['workload_mean']['gpu_tree_leaves_vs_invocations'], 100)

    def cost_pair(self):
        common = dict(self.row, backend='dx12', view='entrance', executable_path='audit.exe',
                      width=960, height=540, workload_counters_requested=True, workload_samples=36)
        counters = dict(gpu_tree_leaves_vs_invocations=100, gpu_tree_leaves_primitives=50,
                        gpu_tree_leaves_ps_invocations=1000)
        return [dict(common, mode='leaves', scissor_mode='full', workload_mean=counters,
                     phase_mean_ms={'gpu_tree_leaves_ms': 3}),
                dict(common, mode='leaves-vertex-only', scissor_mode='empty',
                     workload_mean=dict(counters, gpu_tree_leaves_ps_invocations=0),
                     phase_mean_ms={'gpu_tree_leaves_ms': 1})]

    def test_cost_isolation_requires_same_geometry_and_zero_fragments(self):
        pair = self.cost_pair()
        result = summary.isolate_costs(pair)[0]
        self.assertEqual(result['vertex_front_end_control_ms'], 1)
        self.assertEqual(result['incremental_raster_fragment_ms'], 2)
        for field in ('gpu_tree_leaves_vs_invocations', 'gpu_tree_leaves_primitives',
                      'gpu_tree_leaves_ps_invocations'):
            pair = self.cost_pair()
            pair[1]['workload_mean'][field] += 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                summary.isolate_costs(pair)

    def test_negative_ablation_residual_is_not_hidden(self):
        pair = self.cost_pair()
        pair[1]['phase_mean_ms']['gpu_tree_leaves_ms'] = 4
        self.assertEqual(summary.isolate_costs(pair)[0]['incremental_raster_fragment_ms'], -1)


class EditorRouteTests(unittest.TestCase):
    def write_capture(self, path, matrix_offset=0, step=1/60):
        settings = {key: 1 for key in editor_provenance.SETTINGS}
        settings.update(graphics_backend='dx12', build_configuration='Release')
        rows = [dict(settings, type='frame', frame=i, playing=True, measured=True,
                     executable_path='editor.exe',
                     recurring_frame=True, simulation_delta_ms=step*1000,
                     simulation_time_seconds=(i+32)*step, game_camera_view=[i+matrix_offset]*16,
                     game_camera_projection=[1]*16, editor_update_interval_ms=10) for i in range(3)]
        rows.append(dict(type='capture_complete'))
        path.write_text('\n'.join(json.dumps(row) for row in rows))
        manifest = dict(capture_succeeded=True, comparison={'unchanged': True}, layout_seed={'sha256': 'layout'},
            provenance=dict(schema=1, explicit_inputs_sha256='inputs', assets_unchanged=True, build_unchanged=True, recording_sha256=editor_provenance.sha256(path),
                assets_sha256='assets', platform={}, environment={}, route_sha256='route',
                build={'cmake_cache_sha256': 'cache', 'executable_sha256': 'exe', 'executable_path': 'editor.exe'},
                workload=editor_provenance.workload_identity(path, step)))
        path.with_suffix('.inputs.json').write_text(json.dumps(manifest))

    def test_matching_route_and_changed_trajectory(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = (Path(directory) / name for name in ('a.jsonl', 'b.jsonl'))
            self.write_capture(a)
            self.write_capture(b)
            self.assertTrue(editor_provenance.compare_captures(a, b)['matched'])
            self.write_capture(b, matrix_offset=1)
            with self.assertRaises(ValueError):
                editor_provenance.compare_captures(a, b)

    def test_editor_recording_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.jsonl'
            self.write_capture(path)
            path.write_text(path.read_text() + '\n{}')
            with self.assertRaises(ValueError):
                editor_provenance.load_capture(path)

    def test_changed_external_inputs_or_driver_cannot_match(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = (Path(directory) / name for name in ('a.jsonl', 'b.jsonl'))
            self.write_capture(a)
            for key in ('explicit_inputs_sha256', 'platform', 'route_sha256', 'environment'):
                self.write_capture(b)
                sidecar = b.with_suffix('.inputs.json')
                manifest = json.loads(sidecar.read_text())
                manifest['provenance'][key] = 'changed'
                sidecar.write_text(json.dumps(manifest))
                with self.subTest(field=key), self.assertRaises(ValueError):
                    editor_provenance.compare_captures(a, b)


if __name__ == '__main__':
    unittest.main()
