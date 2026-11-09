import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts/Assets'))
from screen_foliage_order import simulate
from audit_foliage_overlap import raster_counts


def fragment(z, accepted=True):
    return np.array([0]), np.array([float(z)]), np.array([accepted])


class OrderTests(unittest.TestCase):
    def test_front_order_reduces_eligible_work(self):
        fragments = [fragment(1), fragment(2), fragment(3)]
        a, da = simulate(fragments, [0, 1, 2], 1)
        b, db = simulate(fragments, [2, 1, 0], 1)
        self.assertEqual(a['eligible_card_samples'], 1)
        self.assertEqual(b['eligible_card_samples'], 3)
        np.testing.assert_array_equal(da, db)

    def test_alpha_rejection_does_not_occlude(self):
        m, depth = simulate([fragment(1, False), fragment(2)], [0, 1], 1)
        self.assertEqual(m['eligible_card_samples'], 2)
        self.assertEqual(m['accepted_depth_writes'], 1)
        self.assertEqual(depth[0], 2)

    def test_hidden_empty_alpha_can_be_rejected_by_depth(self):
        m, _ = simulate([fragment(1), fragment(2, False)], [0, 1], 1)
        self.assertEqual(m['eligible_card_samples'], 1)

    def test_equal_depth_uses_less_equal(self):
        m, _ = simulate([fragment(1), fragment(1)], [0, 1], 1)
        self.assertEqual(m['accepted_depth_writes'], 2)

    def test_rejects_nonpermutation(self):
        with self.assertRaisesRegex(ValueError, 'permutation'):
            simulate([fragment(1), fragment(2)], [0, 0], 1)

    def test_rejects_invalid_depth(self):
        with self.assertRaisesRegex(ValueError, 'depth'):
            simulate([fragment(float('nan'))], [0], 1)

    def test_empty_triangle_has_no_work(self):
        f = (np.array([], dtype=int), np.array([]), np.array([], dtype=bool))
        m, d = simulate([f], [0], 1)
        self.assertEqual(m['eligible_card_samples'], 0)
        self.assertTrue(np.isinf(d[0]))

    def test_fragment_depth_is_perspective_correct(self):
        tri = np.zeros((1, 3, 8))
        tri[0, :, :3] = [[0, 0, 1], [4, 0, 2], [0, 4, 4]]
        texture = np.full((1, 1, 4), 255, dtype=np.uint8)
        captured = []
        def visit(index, pixels, depth, accepted):
            captured.append((pixels, depth, accepted))
        original = raster_counts(tri, texture, .32, 4)
        actual = raster_counts(tri, texture, .32, 4, fragment_visitor=visit)
        pixels, depth, accepted = captured[0]
        x = pixels % 4 + .5; y = pixels // 4 + .5
        expected = 1/((1-x/4-y/4)/1+(x/4)/2+(y/4)/4)
        np.testing.assert_allclose(depth, expected)
        self.assertTrue(accepted.all())
        np.testing.assert_array_equal(original[0], actual[0])
        np.testing.assert_array_equal(original[1], actual[1])


if __name__ == '__main__':
    unittest.main()
