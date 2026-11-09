from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts/Assets'))
from build_foliage_contour import cell_upper_bounds, contour_plan, support_grid
from screen_foliage_trim import bilinear


class ContourTests(unittest.TestCase):
    def uv(self):
        return np.array([[.05, .05], [.95, .05], [.05, .95]])

    def test_bounds_dominate_dense_bilinear_samples(self):
        rng = np.random.default_rng(8021)
        for shape in ((3, 5), (8, 8), (1, 4)):
            alpha = rng.integers(0, 256, shape, dtype=np.uint8)
            for size in (3, 8):
                bounds = cell_upper_bounds(alpha, size)
                uv = rng.random((2000, 2))
                samples = bilinear(alpha, uv)
                ij = np.floor(uv*size).astype(int)
                self.assertTrue(np.all(samples <= bounds[ij[:, 1], ij[:, 0]]+1e-12))

    def test_constant_alpha_bound_is_exact(self):
        np.testing.assert_allclose(cell_upper_bounds(np.full((7, 3), 82, dtype=np.uint8), 8), 82/255)

    def test_bound_includes_repeat_seam(self):
        alpha = np.zeros((4, 4), dtype=np.uint8)
        alpha[:, -1] = 255
        bounds = cell_upper_bounds(alpha, 16)
        self.assertGreaterEqual(bounds[8, 0], .5)
        self.assertEqual(bounds[8, 7], 0)

    def test_coarse_support_prevents_base_only_removal(self):
        grid = support_grid([np.zeros((8, 8), dtype=np.uint8), np.full((1, 1), 128, dtype=np.uint8)], .32, 16)
        self.assertTrue(grid.all())

    def test_full_grid_preserves_original_triangle(self):
        for mode in ('triangle', 'single', 'hull'):
            kept, removed, cut = contour_plan(self.uv(), np.ones((8, 8), dtype=bool), mode)
            np.testing.assert_array_equal(kept[0], np.eye(3))
            self.assertEqual(removed, 0)
            self.assertFalse(cut)

    def test_sat_rejects_supported_cells_outside_triangle(self):
        grid = np.zeros((8, 8), dtype=bool)
        grid[6:, 6:] = True
        kept, removed, cut = contour_plan(self.uv(), grid, 'hull')
        self.assertEqual(kept, [])
        self.assertEqual(removed, 1)
        self.assertTrue(cut)

    def test_contour_preserves_supported_cell_and_accounts_area(self):
        grid = np.zeros((8, 8), dtype=bool)
        grid[2, 2] = True
        for mode, maximum in (('triangle', 1), ('single', 2), ('hull', 2)):
            kept, removed, cut = contour_plan(self.uv(), grid, mode)
            self.assertTrue(cut)
            self.assertLessEqual(len(kept), maximum)
            self.assertAlmostEqual(sum(abs(np.linalg.det(t)) for t in kept)+removed, 1)
            for t in kept:
                np.testing.assert_allclose(t.sum(1), 1)
                self.assertTrue((t >= -1e-12).all())
            # The occupied cell centre must lie in at least one retained triangle.
            centre = np.array([2.5/8, 2.5/8, 1])
            contained = False
            for t in kept:
                vertices = np.column_stack((t@self.uv(), np.ones(3)))
                weights = np.linalg.solve(vertices.T, centre)
                contained |= bool((weights >= -1e-12).all())
            self.assertTrue(contained)

    def test_winding_does_not_change_retained_area(self):
        grid = np.zeros((8, 8), dtype=bool)
        grid[2, 2] = True
        a = contour_plan(self.uv(), grid, 'hull')
        b = contour_plan(self.uv()[::-1], grid, 'hull')
        self.assertAlmostEqual(a[1], b[1])

    def test_invalid_inputs_rejected(self):
        with self.assertRaises(ValueError):
            cell_upper_bounds(np.zeros((4, 4)), 0)
        with self.assertRaises(ValueError):
            support_grid([np.zeros((4, 4))], float('nan'), 8)
        with self.assertRaises(ValueError):
            contour_plan(self.uv()-1, np.zeros((8, 8), dtype=bool), 'hull')


if __name__ == '__main__':
    unittest.main()
