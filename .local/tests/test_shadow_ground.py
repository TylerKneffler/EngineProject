"""Projection, terrain visibility and image sampling checks for shadow analysis."""
import importlib.util
import copy
from pathlib import Path
import tempfile
import unittest
import sys

sys.dont_write_bytecode = True

import numpy as np

SPEC = importlib.util.spec_from_file_location(
    "shadow_ground", Path(__file__).resolve().parents[2] / "Scripts/analyze_shadow_ground.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GroundAnalysisTests(unittest.TestCase):
    def test_resolution_comparison_requires_matching_route_and_quality(self):
        reference = [dict(phase=phase, frame=frame, camera=[0, 1, 0],
                          view_projection_column_major=list(range(16)), width=100, height=100,
                          time=0., walk_view="entrance", forest_caster_range=90,
                          forest_caster_fade_width=1, oak_wind_meshes=0, forced_full_refresh=False)
                     for phase in ("frozen", "animated") for frame in (0, 4)]
        current = copy.deepcopy(reference)
        for row in current:
            row.update(width=200, height=200)
        MODULE.matched_resolution_phases(current, reference)
        for key, value in (("camera", [0, 2, 0]), ("time", .25),
                           ("forest_caster_range", 100000), ("forest_caster_fade_width", 8),
                           ("oak_wind_meshes", 340), ("height", 100)):
            altered = copy.deepcopy(current)
            for row in altered:
                row[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "Unmatched resolution"):
                MODULE.matched_resolution_phases(altered, reference)
        del current[0]["oak_wind_meshes"]
        with self.assertRaisesRegex(ValueError, "Unmatched resolution oak_wind_meshes"):
            MODULE.matched_resolution_phases(current, reference)

    def test_retained_reference_rejects_stale_depth_error(self):
        report = {"partial_depth_clear_supported": True, "paired_comparisons": {"unrestricted_vs_unrestricted-full": {
            "all_samples": {"maximum_rgb": 36.75}}}}
        with self.assertRaisesRegex(ValueError, "Retained shadow reference error"):
            MODULE.check_retained_reference(report)
        report["paired_comparisons"]["unrestricted_vs_unrestricted-full"]["all_samples"]["maximum_rgb"] = 1.5
        MODULE.check_retained_reference(report)
        report["partial_depth_clear_supported"] = False
        self.assertFalse(MODULE.check_retained_reference(report))
        with self.assertRaisesRegex(ValueError, "Missing matched"):
            MODULE.check_retained_reference({"paired_comparisons": {}})

    def test_attribution_rejects_changed_wind_time(self):
        rows = [dict(phase=phase, frame=frame, camera=[0, 1, 0],
                     view_projection_column_major=list(range(16)), width=100, height=100, time=0.)
                for phase in ("frozen", "animated", "unrestricted") for frame in (0, 4)]
        self.assertEqual(len(MODULE.matched_phases(rows)), 3)
        rows[-1]["time"] = .01
        with self.assertRaisesRegex(ValueError, "Unmatched time"):
            MODULE.matched_phases(rows)

    def test_paired_statistics_compare_matching_frames(self):
        first = np.zeros((2, 3, 3))
        second = first.copy()
        second[1, 2, 1] = 20
        result = MODULE.paired_statistics(first, second)
        self.assertEqual(result["maximum_rgb"], 20)
        self.assertEqual(result["samples_over_8"], 1)
        self.assertAlmostEqual(result["mean_rgb"], 20/6)

    def test_attribution_rejects_mixed_walking_views(self):
        rows = [dict(phase=phase, frame=frame, camera=[0, 1, 0],
                     view_projection_column_major=list(range(16)), width=100, height=100,
                     time=0., walk_view="entrance")
                for phase in ("frozen", "animated") for frame in (0, 4)]
        MODULE.matched_phases(rows)
        rows[-1]["walk_view"] = "overlook"
        with self.assertRaisesRegex(ValueError, "Unmatched walk_view"):
            MODULE.matched_phases(rows)
        for row in rows:
            row["walk_view"] = "overlook" if row["frame"] == 4 else "entrance"
        with self.assertRaisesRegex(ValueError, "Unmatched walk_view"):
            MODULE.matched_phases(rows)

    def test_column_major_translation_and_image_y_direction(self):
        matrix = np.eye(4)
        matrix[0, 3] = .2
        pixels, valid = MODULE.project(np.array([[0, .2, .5], [0, 0, 2]]),
                                       matrix.flatten(order="F"), 100, 100)
        np.testing.assert_allclose(pixels[0], [59.5, 39.5])
        self.assertEqual(valid.tolist(), [True, False])

    def test_terrain_contact_is_visible_but_nearer_triangle_occludes(self):
        triangle = np.array([[[-2, 0, -2], [2, 0, -2], [0, 0, 2]]], dtype=float)
        point, eye = np.array([0, 0, 0]), np.array([0, 2, 0])
        self.assertTrue(MODULE.terrain_visible(point, eye, triangle))
        occluder = triangle + [0, 1, 0]
        self.assertFalse(MODULE.terrain_visible(point, eye, np.concatenate((triangle, occluder))))
        self.assertTrue(MODULE.terrain_visible(point, eye, triangle + [10, 1, 0]))

    def test_bilinear_patch_preserves_linear_image_signal(self):
        y, x = np.mgrid[:10, :10]
        image = np.repeat((x + 2*y)[:, :, None], 3, axis=2)
        np.testing.assert_allclose(MODULE.sample_patch(image, np.array([[4.25, 4.5]])),
                                   [[13.25, 13.25, 13.25]])

    def test_temporal_statistics_include_an_isolated_large_change(self):
        values = np.zeros((3, 2, 3))
        values[1, 0, 1] = 20
        result = MODULE.temporal_statistics(values)
        self.assertEqual(result["maximum_step_rgb"], 20)
        self.assertEqual(result["samples_excursion_over_8"], 1)

    def test_world_patch_stays_on_sloped_surface_with_fixed_extent(self):
        triangle = np.array([[[-3, -3, -3], [3, 3, -3], [0, 0, 6]]], dtype=float)
        patches, valid = MODULE.terrain_patches(triangle, .25)
        self.assertTrue(valid[0])
        np.testing.assert_allclose(patches[0, 4], triangle[0].mean(axis=0))
        np.testing.assert_allclose(patches[0, :, 0], patches[0, :, 1])
        self.assertAlmostEqual(np.linalg.norm(patches[0, 0]-patches[0, 8]), np.sqrt(2)*.5)
        translated, valid = MODULE.terrain_patches(triangle+[2, 5, -7], .25)
        np.testing.assert_allclose(translated, patches+[2, 5, -7])
        self.assertTrue(valid[0])

    def test_world_patch_rejects_edges_degenerate_geometry_and_invalid_radius(self):
        triangles = np.array([[[0, 0, 0], [.1, 0, 0], [0, 0, .1]],
                              [[0, 0, 0], [0, 0, 0], [0, 0, 0]]], dtype=float)
        _, valid = MODULE.terrain_patches(triangles, .25)
        self.assertEqual(valid.tolist(), [False, False])
        for radius in (0, -1, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                MODULE.terrain_patches(triangles, radius)

    def test_fixed_world_samples_remove_zoom_dependent_patch_extent(self):
        # Exact quadratic samples on integer pixel locations isolate extent
        # changes from bilinear reconstruction error.
        y, x = np.mgrid[:101, :101]
        world_values, display_values = [], []
        for pixels_per_metre in (10, 20):
            image = np.repeat((((x-50)/pixels_per_metre)**2)[:, :, None], 3, axis=2)
            offsets = np.array([(a, b) for a in (-.1, 0, .1) for b in (-.1, 0, .1)])
            pixels = 50+offsets*pixels_per_metre
            world_values.append(MODULE.sample_bilinear(image, pixels).mean(axis=0))
            display_values.append(MODULE.sample_patch(image, np.array([[50., 50.]]))[0])
        np.testing.assert_allclose(world_values[0], world_values[1])
        np.testing.assert_allclose(display_values[0], display_values[1]*4)

    def test_projected_patch_resolution_is_independent_of_screen_rotation(self):
        pixels = np.array([(x, y) for x in (-2, 0, 2) for y in (-.25, 0, .25)])
        rotation = np.array([[.6, -.8], [.8, .6]])
        for patch in (pixels, pixels @ rotation.T + [80, 50]):
            self.assertAlmostEqual(MODULE.projected_patch_minor_axis(patch[None])[0], .5)

    def test_ppm_validates_payload_size(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"sample.ppm"
            path.write_bytes(b"P6\n2 1\n255\n" + bytes([1, 2, 3, 4, 5, 6]))
            self.assertEqual(MODULE.read_ppm(path).tolist(), [[[1, 2, 3], [4, 5, 6]]])
            path.write_bytes(b"P6\n2 1\n255\n\x01")
            with self.assertRaises(ValueError):
                MODULE.read_ppm(path)


if __name__ == "__main__":
    unittest.main()
