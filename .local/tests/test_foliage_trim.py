from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts/Assets'))
from screen_foliage_trim import bilinear, crop_bounds, mip_chain, quadrature, screen


class FoliageTrimTests(unittest.TestCase):
    def triangles(self):
        triangles = np.zeros((1, 3, 66))
        triangles[0, :, :3] = [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        triangles[0, :, 6:8] = [[.25, .20], [.75, .20], [.5, .25]]
        return triangles

    def test_native_alpha_average_and_rounding(self):
        levels = mip_chain(np.array([[0, 255], [255, 0]], dtype=np.uint8))
        self.assertEqual(levels[-1].tolist(), [[128]])

    def test_odd_dimensions_follow_native_floor_and_edge_replication(self):
        levels = mip_chain(np.arange(15, dtype=np.uint8).reshape(3, 5))
        self.assertEqual([a.shape for a in levels], [(3, 5), (1, 2), (1, 1)])
        self.assertEqual(levels[1].tolist(), [[3, 5]])
        self.assertEqual(levels[2].tolist(), [[4]])

    def test_bilinear_centres_and_repeat_seams(self):
        alpha = np.array([[0, 255]], dtype=np.uint8)
        values = bilinear(alpha, np.array([[.25, .5], [.75, .5], [0, .5], [1, .5], [-.25, .5]]))
        np.testing.assert_allclose(values, [0, 1, .5, .5, 1])

    def test_crop_guards_opposite_repeat_edge(self):
        alpha = np.zeros((8, 8), dtype=np.uint8)
        alpha[3:5, -1] = 255
        bounds = crop_bounds(alpha, .32)
        self.assertEqual(bounds[0], 0)
        self.assertEqual(bounds[2], 1)

    def test_coarse_mip_rejects_base_transparent_trim(self):
        levels = mip_chain(np.array([[0, 0], [255, 255]], dtype=np.uint8))
        result = screen(self.triangles(), levels, .32, np.array([0, .5, 1, 1]), 8)
        self.assertTrue(result['rejected'])
        self.assertIsNone(result['levels'][0]['witness'])
        self.assertIsNotNone(result['levels'][1]['witness'])
        self.assertAlmostEqual(result['removed_surface_fraction'], 1)

    def test_full_crop_removes_nothing(self):
        result = screen(self.triangles(), [np.full((1, 1), 255)], .32, np.array([0, 0, 1, 1]), 8)
        self.assertFalse(result['rejected'])
        self.assertEqual(result['removed_surface_fraction'], 0)

    def test_quadrature_has_equal_area_cell_count(self):
        for n in (1, 4, 16):
            bary = quadrature(n)
            self.assertEqual(len(bary), n*n)
            np.testing.assert_allclose(bary.sum(1), 1)
            self.assertTrue((bary > 0).all())

    def test_rejects_empty_or_degenerate_input(self):
        with self.assertRaises(ValueError):
            mip_chain(np.zeros((0, 0)))
        with self.assertRaises(ValueError):
            quadrature(0)
        tri = self.triangles()
        tri[:, :, :3] = 0
        with self.assertRaises(ValueError):
            screen(tri, [np.full((1, 1), 255)], .32, np.array([0, 0, 1, 1]), 4)


if __name__ == '__main__':
    unittest.main()
