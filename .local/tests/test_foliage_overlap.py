from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts/Assets'))
from audit_foliage_overlap import raster_counts, summarize


class ProjectedOverlapTests(unittest.TestCase):
    def setUp(self):
        self.quad = np.zeros((2, 3, 66), dtype=float)
        self.quad[:, :, :3] = [[[1, 1, 1], [5, 1, 1], [5, 5, 1]],
                                [[1, 1, 1], [5, 5, 1], [1, 5, 1]]]
        self.texture = np.full((4, 4, 4), 255, dtype=np.uint8)

    def test_quad_shared_diagonal_has_single_ownership(self):
        cards, leaves = raster_counts(self.quad, self.texture, .5, 8)
        self.assertEqual(int(cards.sum()), 16)
        self.assertEqual(int(cards.max()), 1)
        np.testing.assert_array_equal(cards, leaves)
        self.assertEqual(summarize(cards, leaves)['excess_accepted_layers'], 0)

    def test_winding_does_not_change_two_sided_coverage(self):
        normal = raster_counts(self.quad, self.texture, .5, 8)
        reversed_winding = raster_counts(self.quad[:, ::-1], self.texture, .5, 8)
        for a, b in zip(normal, reversed_winding):
            np.testing.assert_array_equal(a, b)

    def test_depth_layers_are_counted_before_rejection(self):
        back = self.quad.copy()
        back[:, :, 2] = 2
        cards, leaves = raster_counts(np.concatenate((self.quad, back)), self.texture, .5, 8)
        result = summarize(cards, leaves)
        self.assertEqual(result['leaf_union_pixels'], 16)
        self.assertEqual(result['alpha_accepted_samples'], 32)
        self.assertEqual(result['accepted_layers_per_leaf_pixel'], 2)
        self.assertEqual(result['excess_accepted_layers'], 16)

    def test_triangle_order_does_not_change_overlap(self):
        triangles = np.concatenate((self.quad, self.quad))
        forward = raster_counts(triangles, self.texture, .5, 8)
        backward = raster_counts(triangles[::-1], self.texture, .5, 8)
        for a, b in zip(forward, backward):
            np.testing.assert_array_equal(a, b)

    def test_empty_alpha_counts_card_space_without_leaves(self):
        self.texture[:, :, 3] = 0
        cards, leaves = raster_counts(self.quad, self.texture, .5, 8)
        self.assertEqual(int(cards.sum()), 16)
        self.assertEqual(int(leaves.sum()), 0)
        with self.assertRaisesRegex(ValueError, 'Empty leaf projection'):
            summarize(cards, leaves)

    def test_perspective_correct_uv_interpolation(self):
        tri = np.zeros((1, 3, 66))
        tri[0, :, :3] = [[0, 0, 1], [4, 0, 4], [0, 4, 1]]
        tri[0, 1, 6] = 1
        self.texture[:, :, 3] = 0
        self.texture[:, 0, 3] = 255
        _, perspective = raster_counts(tri, self.texture, .5, 8, True)
        _, affine = raster_counts(tri, self.texture, .5, 8, False)
        self.assertEqual(perspective[1, 1], 1)
        self.assertEqual(affine[1, 1], 0)

    def test_rejects_camera_plane_crossing(self):
        self.quad[0, 0, 2] = -.1
        with self.assertRaisesRegex(ValueError, 'camera plane'):
            raster_counts(self.quad, self.texture, .5, 8)

    def test_degenerate_triangles_contribute_nothing(self):
        self.quad[:, 2] = self.quad[:, 1]
        cards, leaves = raster_counts(self.quad, self.texture, .5, 8)
        self.assertEqual(int(cards.sum()+leaves.sum()), 0)


if __name__ == '__main__':
    unittest.main()
