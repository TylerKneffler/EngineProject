from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts/Assets'))
from build_foliage_interior_trim import Support, plan, split, trim
from screen_foliage_trim import mip_chain


class InteriorTrimTests(unittest.TestCase):
    def uv(self):
        return np.array([[.2, .2], [.8, .2], [.2, .8]])

    def test_empty_support_removes_whole_triangle(self):
        support = Support([np.zeros((8, 8), dtype=np.uint8)], .32)
        kept, removed, cut = plan(self.uv(), support, 3)
        self.assertEqual(kept, [])
        self.assertEqual(removed, 1)
        self.assertTrue(cut)

    def test_full_support_collapses_subdivision(self):
        support = Support([np.full((8, 8), 255, dtype=np.uint8)], .32)
        kept, removed, cut = plan(self.uv(), support, 3)
        self.assertEqual(len(kept), 1)
        np.testing.assert_array_equal(kept[0], np.eye(3))
        self.assertEqual(removed, 0)
        self.assertFalse(cut)

    def test_repeat_boundary_reads_opposite_texel(self):
        alpha = np.zeros((4, 4), dtype=np.uint8)
        alpha[:, -1] = 255
        support = Support([alpha], .32)
        self.assertTrue(support.possible(np.array([[0, .5]]*3)))
        self.assertFalse(support.possible(np.array([[.5, .5]]*3)))

    def test_coarse_mip_prevents_base_only_cut(self):
        alpha = np.zeros((4, 4), dtype=np.uint8)
        alpha[2:] = 255
        uv = np.array([[.2, .2], [.25, .2], [.2, .25]])
        self.assertFalse(Support([alpha], .32).possible(uv))
        self.assertTrue(Support(mip_chain(alpha), .32).possible(uv))

    def test_children_partition_barycentric_area(self):
        children = split(np.eye(3))
        self.assertEqual(len(children), 4)
        for child in children:
            np.testing.assert_allclose(child.sum(1), 1)
            self.assertAlmostEqual(abs(np.linalg.det(child)), .25)

    def test_partial_subdivision_reports_removed_area(self):
        class HalfPlane:
            def possible(self, uv):
                return uv[:, 0].max() >= .6
        kept, removed, cut = plan(np.array([[0, 0], [1, 0], [0, 1]]), HalfPlane(), 1)
        self.assertEqual(len(kept), 1)
        self.assertEqual(removed, .75)
        self.assertTrue(cut)

    def fixture(self):
        triangles = np.zeros((2, 3, 66), dtype=np.float32)
        triangles[0, :, :3] = [[-2, -2, 0], [2, -2, 0], [-2, 2, 0]]
        triangles[1, :, :3] = [[0, 0, .1], [1, 0, .1], [0, 1, .1]]
        # Give the inner triangle an interior Z too, so only the first owns extrema.
        triangles[0, 2, 2] = .2
        triangles[:, :, 6:8] = self.uv()
        return triangles

    def test_extrema_protection_retains_bounds(self):
        source = self.fixture()
        result, metrics = trim(source, Support([np.zeros((4, 4), dtype=np.uint8)], .32), 1)
        self.assertEqual(len(result), 1)
        np.testing.assert_array_equal(result[0], source[0])
        self.assertEqual(metrics['changed_source_triangles'], 1)
        self.assertGreater(metrics['removed_surface_fraction'], 0)

    def test_rejects_varying_animation_fields(self):
        source = self.fixture()
        source[1, 1, 18] = 1
        with self.assertRaisesRegex(ValueError, 'animation fields'):
            trim(source, Support([np.zeros((4, 4), dtype=np.uint8)], .32), 1)

    def test_rejects_unsupported_uvs_and_nonfinite_vertices(self):
        support = Support([np.zeros((4, 4), dtype=np.uint8)], .32)
        with self.assertRaises(ValueError):
            support.possible(np.array([[-.1, .5]]*3))
        source = self.fixture()
        source[0, 0, 0] = float('nan')
        with self.assertRaises(ValueError):
            trim(source, support, 1)


if __name__ == '__main__':
    unittest.main()
