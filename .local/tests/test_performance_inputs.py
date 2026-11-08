import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Scripts'))
from performance_inputs import compare_inputs, snapshot_inputs
import profile_tree_memory


class PerformanceInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # These tests isolate scene/layout lifecycle behavior. Device provenance
        # and real recording validation have separate tests in test_forest_performance.
        original_sha = profile_tree_memory.sha256
        for name, replacement in (
            ('machine_identity', lambda: {}),
            ('content_identity', lambda: {}),
            ('workload_identity', lambda *args: {}),
            ('sha256', lambda path: original_sha(path) if Path(path).exists() else 'mock-recording'),
        ):
            patcher = patch.object(profile_tree_memory, name, replacement)
            patcher.start()
            self.addCleanup(patcher.stop)

    def write(self, path, contents):
        path = self.root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding='utf-8')
        return path

    def test_nested_json_xml_and_cycles(self):
        scene = self.write('main.scene', '<Scene><object prefab="Assets/a.prefab"/></Scene>')
        self.write('Engine/Core/Assets/a.prefab',
                   '{"object":{"prefab":"Assets/b.prefab"}}')
        self.write('Engine/Core/Assets/b.prefab',
                   '<Prefab><item>Assets/a.prefab</item></Prefab>')
        before = snapshot_inputs(scene, root=self.root)
        self.assertEqual(len(before['files']), 3)
        self.assertEqual(before['errors'], [])
        self.assertTrue(compare_inputs(before, snapshot_inputs(scene, root=self.root))['unchanged'])

    def test_same_size_same_timestamp_content_edit(self):
        import os
        scene = self.write('main.scene', '<Scene/>')
        asset = self.write('route.txt', 'aaaa')
        before = snapshot_inputs(scene, [asset], self.root)
        stamp = asset.stat().st_mtime_ns
        asset.write_text('bbbb')
        os.utime(asset, ns=(stamp, stamp))
        comparison = compare_inputs(before, snapshot_inputs(scene, [asset], self.root))
        self.assertFalse(comparison['unchanged'])
        self.assertIn(str(asset.resolve()), comparison['changed_files'])

    def test_override_changes_resolution_and_missing_is_incomplete(self):
        scene = self.write('main.scene', '<Scene prefab="Assets/a.prefab"/>')
        self.write('Engine/Core/Assets/a.prefab', '<Prefab/>')
        before = snapshot_inputs(scene, root=self.root)
        self.write('Assets/a.prefab', '<Prefab/>')
        comparison = compare_inputs(before, snapshot_inputs(scene, root=self.root))
        self.assertIn('Assets/a.prefab', comparison['changed_resolutions'])
        scene.write_text('<Scene prefab="Assets/missing.prefab"/>')
        after = snapshot_inputs(scene, root=self.root)
        self.assertFalse(compare_inputs(before, after)['complete'])

    def test_relocation_encoding_and_parse_errors(self):
        scene = self.write('main.scene', ' ' * 5000 +
                           '<Scene prefab="Z:/old/Engine/Core/Assets/a.prefab"/>')
        prefab = self.write('Engine/Core/Assets/a.prefab', '<Prefab/>')
        prefab.write_bytes('<Prefab/>'.encode('utf-16'))
        before = snapshot_inputs(scene, root=self.root)
        self.assertEqual(before['errors'], [])
        prefab.write_text('<Prefab')
        self.assertTrue(snapshot_inputs(scene, root=self.root)['errors'])

    def test_failed_capture_retains_before_and_after_identity(self):
        import subprocess
        from types import SimpleNamespace
        scene = self.write('main.scene', '<Scene/>')
        editor = self.write('editor.exe', 'test executable')
        route = self.write('route.txt', 'before')
        self.write('imgui.ini', 'layout')
        output = self.root / 'capture.jsonl'
        def fail(*args, **kwargs):
            route.write_text('after')
            raise subprocess.CalledProcessError(2, ['test editor'])
        argv = ['profile', str(output), '--scene', str(scene), '--editor', str(editor),
                '--input', str(route), '--frames', '30', '--stop-frames', '1']
        with patch.object(profile_tree_memory, 'ROOT', self.root), \
             patch.object(sys, 'argv', argv), \
             patch.object(subprocess, 'STARTUPINFO', return_value=SimpleNamespace(dwFlags=0), create=True), \
             patch.object(subprocess, 'STARTF_USESHOWWINDOW', 1, create=True), \
             patch.object(subprocess, 'run', side_effect=fail):
            with self.assertRaises(subprocess.CalledProcessError):
                profile_tree_memory.main()
        import json
        manifest = json.loads(output.with_suffix('.inputs.json').read_text())
        self.assertFalse(manifest['capture_succeeded'])
        self.assertFalse(manifest['comparison']['unchanged'])
        self.assertIn(str(route.resolve()), manifest['comparison']['changed_files'])

    def test_summary_only_preserves_historical_identity(self):
        import contextlib
        import io
        output = self.root / 'capture.jsonl'
        manifest = output.with_suffix('.inputs.json')
        manifest.write_text('historical inputs')
        with patch.object(sys, 'argv', ['profile', str(output), '--summarize-only']), \
             patch.object(profile_tree_memory, 'snapshot_inputs') as snapshot, \
             patch.object(profile_tree_memory, 'summarize', return_value={}), \
             contextlib.redirect_stdout(io.StringIO()):
            profile_tree_memory.main()
        snapshot.assert_not_called()
        self.assertEqual(manifest.read_text(), 'historical inputs')

    def test_completed_capture_rejects_changed_inputs(self):
        import contextlib
        import io
        import subprocess
        from types import SimpleNamespace
        scene = self.write('main.scene', '<Scene/>')
        editor = self.write('editor.exe', 'test executable')
        route = self.write('route.txt', 'before')
        self.write('imgui.ini', 'layout')
        output = self.root / 'capture.jsonl'
        summary = {'edit': {'frames': 1}, 'stopped': {'frames': 1},
                   'play': {'frames': 30, 'game_view_width': {'median': 1920},
                            'game_view_height': {'median': 1080}}}
        argv = ['profile', str(output), '--scene', str(scene), '--editor', str(editor),
                '--layout-seed', str(route), '--frames', '30', '--stop-frames', '1']
        with patch.object(profile_tree_memory, 'ROOT', self.root), \
             patch.object(sys, 'argv', argv), \
             patch.object(subprocess, 'STARTUPINFO', return_value=SimpleNamespace(dwFlags=0), create=True), \
             patch.object(subprocess, 'STARTF_USESHOWWINDOW', 1, create=True), \
             patch.object(subprocess, 'run', side_effect=lambda *a, **k: route.write_text('after')), \
             patch.object(profile_tree_memory, 'summarize', return_value=summary), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'inputs changed'):
                profile_tree_memory.main()
        self.assertTrue(output.with_suffix('.summary.json').exists())

    def test_explicit_layout_seed_is_preserved_and_workspace_changes_do_not_matter(self):
        import contextlib
        import io
        import json
        import subprocess
        from types import SimpleNamespace
        scene = self.write('main.scene', '<Scene/>')
        editor = self.write('editor.exe', 'test executable')
        workspace_layout = self.write('imgui.ini', 'user layout')
        seed = self.write('fixed.ini', 'fixed layout')
        output = self.root / 'capture.jsonl'
        summary = {'edit': {'frames': 1}, 'stopped': {'frames': 1}, 'play': {'frames': 30,
                   'game_view_width': {'median': 1920}, 'game_view_height': {'median': 1080}}}
        def run(*args, **kwargs):
            self.assertEqual(output.with_suffix('.ini').read_bytes(), seed.read_bytes())
            workspace_layout.write_text('user changed layout')
            output.with_suffix('.ini').write_text('Editor updated working layout')
        with patch.object(profile_tree_memory, 'ROOT', self.root), \
             patch.object(sys, 'argv', ['profile', str(output), '--scene', str(scene), '--editor', str(editor),
                                       '--layout-seed', str(seed), '--frames', '30', '--stop-frames', '1']), \
             patch.object(subprocess, 'STARTUPINFO', return_value=SimpleNamespace(dwFlags=0), create=True), \
             patch.object(subprocess, 'STARTF_USESHOWWINDOW', 1, create=True), \
             patch.object(subprocess, 'run', side_effect=run), \
             patch.object(profile_tree_memory, 'summarize', return_value=summary), \
             contextlib.redirect_stdout(io.StringIO()):
            profile_tree_memory.main()
        manifest = json.loads(output.with_suffix('.inputs.json').read_text())
        self.assertTrue(manifest['comparison']['unchanged'])
        self.assertEqual(output.with_suffix('.layout-seed.ini').read_bytes(), b'fixed layout')
        self.assertEqual(manifest['layout_seed']['source'], str(seed.resolve()))
        self.assertNotIn(str(workspace_layout.resolve()), manifest['before']['files'])

    def test_layout_seed_cannot_alias_output_layouts(self):
        output = self.root / 'capture.jsonl'
        for name in ('capture.ini', 'capture.layout-seed.ini'):
            with self.subTest(name=name):
                seed = self.write(name, 'keep this')
                with patch.object(sys, 'argv', ['profile', str(output), '--layout-seed', str(seed)]):
                    with self.assertRaises(SystemExit):
                        profile_tree_memory.main()
                self.assertEqual(seed.read_text(), 'keep this')


if __name__ == '__main__':
    unittest.main()
