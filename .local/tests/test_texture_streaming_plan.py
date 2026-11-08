import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('planner', ROOT / 'Scripts/Assets/plan_texture_streaming.py')
planner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(planner)


class StreamingPolicyTests(unittest.TestCase):
    def test_runtime_uploads_do_not_drain_gpu(self):
        paths = [('Engine/Core/Renderers/DX12/DX12GraphicsTexture.cpp',
                  'D3D12TextureFactory::CreateTexture2D(', 'D3D12TextureFactory::CreateDepthTexture2D('),
                 ('Engine/Core/Renderers/Vulkan/VulkanGraphicsTexture.cpp',
                  'VulkanTextureSystem::Upload(', 'VulkanTextureSystem::EstimateAllocationBytes('),
                 ('Engine/Core/Compoonents/Materials/Texture.cpp',
                  'void Texture::StreamFrame(', 'bool Texture::PrepareImmediate(')]
        for path, start, end in paths:
            source = (ROOT / path).read_text()
            runtime = source[source.index(start):source.index(end)]
            for forbidden in ('WaitForSingleObject(', 'vkQueueWaitIdle(', 'vkDeviceWaitIdle(', 'WaitIdle('):
                self.assertNotIn(forbidden, runtime, path)

    def run_route(self, frames, budget=12 * 1048576, hold=3):
        return planner.simulate(dict(textures=[dict(id=k, width=1024, height=1024)
                                              for k in ('a', 'b')], frames=[dict(demands=d) for d in frames]),
                                budget, 6 * 1048576, hold, 1)['frames']

    def test_rapid_turn_keeps_recent_mips(self):
        rows = self.run_route([{'a': 512}, {'b': 512}, {'a': 512}])
        self.assertEqual(rows[0]['resident_mips']['a'], 0)
        self.assertEqual(rows[1]['resident_mips']['a'], 0)
        self.assertEqual(rows[2]['upload_bytes'], 0)

    def test_teleport_evicts_after_hold(self):
        rows = self.run_route([{'a': 512}] + [{'b': 512}] * 5)
        self.assertEqual(rows[-1]['resident_mips']['a'], 10)
        self.assertEqual(rows[-1]['resident_mips']['b'], 0)
        self.assertGreater(sum(r['evictions'] for r in rows), 0)

    def test_pressure_and_replacement_overlap(self):
        budget = 2 * 1048576
        rows = self.run_route([{'a': 1024}, {'b': 1024}] * 10, budget)
        for row in rows:
            self.assertLessEqual(row['resident_bytes'], budget)
            self.assertLessEqual(row['peak_replacement_bytes'], budget)
            self.assertLessEqual(row['upload_bytes'], 6 * 1048576)
            self.assertGreater(row['unmet_textures'], 0)

    def test_visible_receding_texture_eventually_coarsens(self):
        rows = self.run_route([{'a': 512}] + [{'a': 1}] * 5)
        self.assertEqual(rows[1]['resident_mips']['a'], 0)
        self.assertEqual(rows[-1]['resident_mips']['a'], 9)

    def test_rectangular_chain_and_invalid_floor(self):
        self.assertEqual(planner.chain(dict(width=3, height=1)), [16, 4])
        with self.assertRaises(ValueError):
            self.run_route([{}], 1)

    def test_invalid_demand(self):
        for demand in ({'unknown': 1}, {'a': float('nan')}, {'a': -1}):
            with self.assertRaises(ValueError):
                self.run_route([demand])


if __name__ == '__main__':
    unittest.main()
