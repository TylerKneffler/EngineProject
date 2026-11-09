import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts/Assets'))
from screen_foliage_order_bank import bank_orders, directions, local_axis, select, unit


class BankTests(unittest.TestCase):
    def test_bank_unit_vectors_and_opposites(self):
        for count in (6, 14):
            axes = directions(count)
            self.assertEqual(axes.shape, (count, 3))
            np.testing.assert_allclose(np.linalg.norm(axes, axis=1), 1)
            for axis in axes:
                self.assertTrue(np.any(np.linalg.norm(axes+axis, axis=1) < 1e-12))

    def test_selects_matching_direction(self):
        axes = directions(14)
        for index, axis in enumerate(axes):
            self.assertEqual(select(axes, axis), index)

    def test_stable_permutation_and_opposite_depth_order(self):
        tri = np.zeros((3, 3, 66))
        tri[:, :, 0] = np.array([1, 2, 1])[:, None]
        orders = bank_orders(tri, np.array([[1, 0, 0], [-1, 0, 0]]))
        self.assertEqual(orders[0].tolist(), [1, 0, 2])
        self.assertEqual(orders[1].tolist(), [0, 2, 1])
        for order in orders:
            self.assertEqual(sorted(order), [0, 1, 2])

    def test_rotation_maps_world_sort_axis(self):
        rotation = np.array([[0, 0, 1], [0, 1, 0], [-1, 0, 0]])
        np.testing.assert_allclose(local_axis(rotation, [1, 0, 0]), [0, 0, 1])

    def test_nonuniform_scale_uses_transpose(self):
        linear = np.diag([2., 1., .5])
        np.testing.assert_allclose(local_axis(linear, [1, 1, 0]), unit([2, 1, 0]))
        points = np.array([[2., 1., 0.], [1., 4., 0.], [-1., 1., 3.]])
        world_scores = points @ linear.T @ unit([1, 1, 0])
        local_scores = points @ local_axis(linear, [1, 1, 0])
        np.testing.assert_array_equal(np.argsort(world_scores), np.argsort(local_scores))

    def test_reflection_preserves_sort_axis(self):
        np.testing.assert_allclose(local_axis(np.diag([-1, 1, 1]), [1, 0, 0]), [-1, 0, 0])

    def test_rejects_invalid_transforms(self):
        for matrix in (np.zeros((3, 3)), np.eye(4), np.full((3, 3), np.nan)):
            with self.assertRaises(ValueError):
                local_axis(matrix, [1, 0, 0])

    def test_rejects_invalid_direction_and_bank(self):
        for axis in ([0, 0, 0], [np.nan, 0, 1], [1, 0]):
            with self.assertRaises(ValueError):
                unit(axis)
        with self.assertRaises(ValueError):
            directions(8)


if __name__ == '__main__':
    unittest.main()
