import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts/Assets'))
from screen_foliage_interior import prune, coverage
from audit_foliage_overlap import raster_counts


def case(*hits):
    arrays = [np.array(h, dtype=np.int64) for h in hits]
    counts = np.zeros(8, dtype=np.uint32)
    for pixels in arrays:
        np.add.at(counts, pixels, 1)
    return arrays, counts


class InteriorTests(unittest.TestCase):
    def test_joint_removal_retains_last_cover(self):
        data = case([1, 2], [1, 2], [3])
        kept = prune([data], np.zeros(3, dtype=bool))
        self.assertEqual(kept.tolist(), [False, True, True])
        self.assertEqual(coverage(*data, kept)['lost_pixels'], 0)

    def test_unique_in_any_training_view_is_retained(self):
        kept = prune([case([1], [1]), case([2], [3])], [False, False])
        self.assertTrue(kept.all())

    def test_protected_extrema_retained(self):
        kept = prune([case([1], [1])], [True, False])
        self.assertEqual(kept.tolist(), [True, False])

    def test_unseen_triangle_can_fail_holdout(self):
        kept = prune([case([], [1])], [False, False])
        metrics = coverage(*case([2], [1]), kept)
        self.assertEqual(metrics['lost_pixels'], 1)
        self.assertEqual(metrics['lost_fraction'], .5)

    def test_rejects_inconsistent_counts(self):
        hits, counts = case([1]); counts[1] = 0
        with self.assertRaisesRegex(ValueError, 'disagree'):
            prune([(hits, counts)], [False])

    def test_rejects_duplicate_triangle_pixels(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            prune([case([1, 1])], [False])

    def test_rejects_empty_training(self):
        with self.assertRaises(ValueError):
            prune([], [False])

    def test_raster_visitor_reconstructs_accepted_counts(self):
        tri = np.zeros((2, 3, 8))
        tri[:, :, :3] = [[[0, 0, 1], [4, 0, 1], [0, 4, 1]],
                         [[0, 0, 2], [4, 0, 2], [0, 4, 2]]]
        texture = np.full((2, 2, 4), 255, dtype=np.uint8)
        observed = np.zeros(16, dtype=np.uint32)
        def visit(index, pixels):
            np.add.at(observed, pixels, 1)
        a, b = raster_counts(tri, texture, .32, 4, accepted_visitor=visit)
        original = raster_counts(tri, texture, .32, 4)
        np.testing.assert_array_equal(observed.reshape(4, 4), b)
        np.testing.assert_array_equal(a, original[0])
        np.testing.assert_array_equal(b, original[1])


if __name__ == '__main__':
    unittest.main()
