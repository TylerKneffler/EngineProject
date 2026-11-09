import sys
from pathlib import Path
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Scripts/Assets'))
from audit_foliage_cards import coverage


class CardCoverageTests(unittest.TestCase):
    def setUp(self):
        self.triangles = np.zeros((1, 3, 66))
        self.triangles[0, :, 6:8] = [[0, 0], [1, 0], [0, 1]]

    def test_opaque_and_empty(self):
        for alpha, expected in ((255, 1), (0, 0)):
            image = np.full((4, 4, 4), alpha, dtype=np.uint8)
            self.assertEqual(coverage(self.triangles, image, .5, 8)[0], expected)

    def test_triangle_area_instead_of_texture_rectangle(self):
        image = np.zeros((128, 128, 4), dtype=np.uint8)
        image[:, :64, 3] = 255
        # x < .5 occupies 3/4 of this right triangle, not 1/2.
        self.assertAlmostEqual(coverage(self.triangles, image, .5, 16)[0], .75)

    def test_repeat_addressing(self):
        image = np.zeros((8, 8, 4), dtype=np.uint8)
        image[:, :4, 3] = 255
        baseline = coverage(self.triangles, image, .5, 8)
        self.triangles[:, :, 6:8] -= 2
        np.testing.assert_array_equal(coverage(self.triangles, image, .5, 8), baseline)

    def test_invalid_resolution(self):
        with self.assertRaises(ValueError):
            coverage(self.triangles, np.zeros((1, 1, 4)), .5, 0)


if __name__ == '__main__':
    unittest.main()
