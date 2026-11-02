import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("evaluate_performance", Path(__file__).resolve().parents[2] / "Scripts/evaluate_performance.py")
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)


class PerformanceEvaluationTests(unittest.TestCase):
    def test_async_writer_loss_is_reported(self):
        result = evaluation.evaluate([dict(type="frame_details", playing=True, frame_ms=10),
                                      dict(type="writer_summary", dropped_records=3, failed_records=1)])
        self.assertEqual(result["recording_lost_records"], 4)
        self.assertTrue(any("may omit stalls" in warning for warning in result["warnings"]))

    def test_instance_upload_causes_and_late_metrics(self):
        rows = [dict(type="frame_details", playing=True, frame_ms=10),
                dict(type="frame_details", playing=True, frame_ms=20, instance_edited_bytes=64,
                     shadow_instance_metadata_bytes=144)]
        result = evaluation.evaluate(rows)
        self.assertEqual(result["metrics"]["instance_edited_bytes"]["max"], 64)
        self.assertIn("shadow_instance_metadata_bytes", evaluation.markdown(result))

    def test_render_helper_metric_aliases(self):
        result = evaluation.evaluate([dict(type="frame_details", playing=True, frame_ms=10,
                                          grass_ms=1, grass_built=2, grass_scanned=40, render_submit_ms=3)])
        self.assertEqual(result["grass_tiles_prepared"], 2)
        self.assertEqual(result["grass_placements_indexed"], 40)
        self.assertEqual(result["metrics"]["render_submission_ms"]["mean"], 3)

    def test_intervals_gpu_deduplication_and_missing_metrics(self):
        rows = [dict(type="frame_details", playing=True, dt_ms=10, frame_ms=8, gpu_valid=True,
                     gpu_sample_id=1, gpu_shadow_ms=4, main_submit_ms=6, render_submission_ms=2),
                dict(type="frame_details", playing=True, dt_ms=30, frame_ms=20, gpu_valid=True,
                     gpu_sample_id=1, gpu_shadow_ms=4, main_submit_ms=6, render_submission_ms=2),
                dict(type="frame_details", playing=True, dt_ms=20, frame_ms=18, gpu_valid=True,
                     gpu_sample_id=2, gpu_shadow_ms=8, main_submit_ms=6, render_submission_ms=2)]
        result = evaluation.evaluate(rows)
        self.assertEqual(result["effective_fps"], 50)
        self.assertEqual(result["unique_gpu_samples"], 2)
        self.assertEqual(result["gpu_total_ms"]["mean"], 6)
        self.assertEqual(result["main_pass_unexplained_ms"], 4)
        self.assertTrue(any("predates" in warning for warning in result["warnings"]))

    def test_play_filter_and_invalid_gpu_samples(self):
        rows = [dict(type="frame_details", playing=False, dt_ms=1000),
                dict(type="frame_details", playing=True, dt_ms=20, gpu_valid=False, gpu_shadow_ms=999)]
        result = evaluation.evaluate(rows)
        self.assertEqual(result["effective_fps"], 50)
        self.assertIsNone(result["gpu_total_ms"])
        self.assertEqual(result["frames"], 1)

    def test_benchmark_warmup_and_shadow_spike_fields(self):
        rows = [dict(type="frame_details", playing=True, measured=False, frame_ms=1000),
                dict(type="frame_details", playing=True, measured=True, frame_ms=20,
                     shadow_submission_thread_cpu_ms=2, shadow_submission_off_cpu_ms=3, shadow_max_batch_ms=4)]
        result = evaluation.evaluate(rows)
        self.assertEqual(result["frames"], 1)
        self.assertEqual(result["effective_fps"], 50)
        self.assertIn("shadow_max_batch_ms", evaluation.markdown(result))
        self.assertTrue(any("scheduling" in warning for warning in result["warnings"]))

    def test_shadow_cycle_density_identifies_wait_candidates(self):
        rows = [dict(type="frame_details", playing=True, frame_ms=20,
                     shadow_submission_wall_ms=1, shadow_submission_thread_cycles=3000000)] * 3
        rows += [dict(type="frame_details", playing=True, frame_ms=30,
                      shadow_submission_wall_ms=10, shadow_submission_thread_cycles=3000000)]
        result = evaluation.evaluate(rows)
        self.assertEqual(result["shadow_submission"]["low_cycle_density_spikes"], 1)
        self.assertIn("low cycle density", evaluation.markdown(result))

    def test_opt_in_editor_trace_and_measured_hardware(self):
        rows = [dict(schema_version=2, frame=0, playing=True, measured=False, frame_ms=100),
                dict(schema_version=2, frame=1, playing=True, measured=True, frame_ms=10),
                dict(type="sample", measured=False, process_cpu_pct=80),
                dict(type="sample", measured=True, process_cpu_pct=5)]
        result = evaluation.evaluate(rows)
        self.assertEqual(result["frames"], 1)
        self.assertEqual(result["effective_fps"], 100)
        self.assertEqual(result["hardware"]["process_cpu_pct"]["mean"], 5)

    def test_unfinished_warmup_is_not_reported_as_measured(self):
        with self.assertRaises(ValueError):
            evaluation.evaluate([dict(type="frame_details", measured=False, playing=True, frame_ms=20)])

    def test_native_diagnostics_exclude_disabled_frames(self):
        rows = [dict(type="frame_details", frame_ms=10, native_timing_enabled=False,
                     native_camera_map_calls=0, native_camera_map_ms=0),
                dict(type="frame_details", frame_ms=20, native_timing_enabled=True,
                     native_camera_map_calls=4, native_camera_map_ms=3,
                     native_camera_map_max_ms=2, native_camera_map_thread_cycles=1234,
                     native_camera_map_cycle_samples=4, native_compute_arena_writes=8)]
        result = evaluation.evaluate(rows)
        native = result["native_calls"]["native_camera_map"]
        self.assertEqual(result["native_timed_frames"], 1)
        self.assertEqual(result["metrics"]["native_compute_arena_writes"]["mean"], 8)
        self.assertEqual(native["calls"]["mean"], 4)
        self.assertEqual(native["wall_ms"]["mean"], 3)
        self.assertEqual(native["maximum_call_ms"]["max"], 2)
        self.assertIn("native_camera_map", evaluation.markdown(result))
        self.assertTrue(any("per-call" in warning for warning in result["warnings"]))

    def test_transform_preparation_fields(self):
        result = evaluation.evaluate([dict(type="frame_details", frame_ms=20,
            prepare_transforms_ms=2, prepare_transform_snapshot_ms=.5,
            prepare_transform_compute_ms=1, prepare_transform_commit_ms=.5,
            prepare_transform_local_updated=4, prepare_transform_world_updated=12,
            prepare_transform_world_reused=100, prepare_transform_levels=3, prepare_transform_structure_fallback=1)])
        self.assertEqual(result["metrics"]["prepare_transform_world_reused"]["mean"], 100)
        report = evaluation.markdown(result)
        self.assertIn("prepare_transform_compute_ms", report)
        self.assertIn("prepare_transform_world_updated", report)
        self.assertIn("prepare_transform_structure_fallback", report)
        self.assertTrue(any("lazy owner transform" in warning for warning in result["warnings"]))
        self.assertTrue(any("nested wall timings" in warning for warning in result["warnings"]))

    def test_spatial_index_preparation_fields(self):
        result = evaluation.evaluate([dict(type="frame_details", frame_ms=20,
            prepare_spatial_index_ms=2, prepare_spatial_compare_ms=.25,
            prepare_spatial_refit_ms=1.75, prepare_spatial_build_ms=0,
            prepare_spatial_changed_bounds=1, prepare_spatial_reused_bounds=999,
            prepare_spatial_dirty_leaves=1, prepare_spatial_refit_nodes=8,
            prepare_spatial_rebuilds=0, prepare_spatial_quality_rebuilds=0)])
        self.assertEqual(result["metrics"]["prepare_spatial_refit_nodes"]["mean"], 8)
        report = evaluation.markdown(result)
        self.assertIn("prepare_spatial_index_ms", report)
        self.assertIn("prepare_spatial_rebuilds", report)
        self.assertTrue(any("Spatial compare/refit/build" in warning for warning in result["warnings"]))

    def test_partial_gpu_instrumentation_can_be_reported(self):
        result = evaluation.evaluate([dict(type="frame_details", playing=True, dt_ms=20,
                                          gpu_valid=True, gpu_opaque_ms=2)])
        self.assertIsNone(result["gpu_shadow_share_percent"])
        self.assertIn("2.00 ms", evaluation.markdown(result))


if __name__ == "__main__":
    unittest.main()
