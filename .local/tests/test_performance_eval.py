import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("evaluate_performance", Path(__file__).resolve().parents[2] / "Scripts/evaluate_performance.py")
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)


class PerformanceEvaluationTests(unittest.TestCase):
    def test_editor_pipeline_reports_overlapping_work(self):
        result = evaluation.evaluate([dict(type="frame_details", playing=True,
            frame_ms=16, dt_ms=16, update_ms=0, render_ms=16,
            editor_play_pipeline=True, simulation_ms=8,
            simulation_cpu_ms=6, simulation_wait_ms=1,
            simulation_overlap_ms=7, pipeline_record_ms=2)])
        self.assertEqual(result["metrics"]["simulation_ms"]["mean"], 8)
        self.assertEqual(result["effective_fps"], 62.5)
        self.assertIn("simulation_overlap_ms", evaluation.markdown(result))
        self.assertTrue(any("must not be added" in text for text in result["warnings"]))

    def test_stop_freeze_survives_play_filtering(self):
        rows = [dict(type="frame_details", frame=7, playing=True, dt_ms=16,
                     frame_ms=55016, pending_load_ms=55000, render_ms=10),
                dict(type="frame_details", frame=8, playing=False, dt_ms=55016, frame_ms=16)]
        result = evaluation.evaluate(rows)
        self.assertEqual(result["frames"], 1)
        self.assertEqual(result["effective_fps"], 62.5)
        self.assertEqual(result["session"]["frames"], 2)
        self.assertAlmostEqual(result["session"]["effective_fps"], 1000 / 27516)
        stall = result["session"]["stalls"][0]
        self.assertEqual(stall["stage"], "pending_load_ms")
        self.assertEqual(stall["stage_ms"], 55000)
        self.assertTrue(stall["play_stop_transition"])
        self.assertIn("55000.0", evaluation.markdown(result))
        self.assertTrue(any("Stop freeze" in warning for warning in result["warnings"]))
        self.assertEqual(evaluation.evaluate(rows, all_frames=True)["session"]["stalls"], result["session"]["stalls"])

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

    def test_per_core_and_gpu_engine_readings_use_measured_samples(self):
        rows = [dict(type="frame_details", measured=True, frame_ms=10),
                dict(type="sample", measured=False, cpu_cores_pct=[[99, 0]],
                     gpu_engine_instances=[["luid_1_phys_0_eng_0", 0, 99]]),
                dict(type="sample", measured=True, cpu_cores_pct=[[20, 5], [10, 10]],
                     gpu_engine_instances=[["luid_1_phys_0_eng_0", 0, 30]]),
                dict(type="sample", measured=True, cpu_cores_pct=[[40, 10], [0, 20]],
                     gpu_engine_instances=[["luid_1_phys_0_eng_0", 0, 50],
                                           ["luid_1_phys_0_eng_1", 1, 75]])]
        result = evaluation.evaluate(rows)
        self.assertEqual(result["cpu_cores"]["0"]["mean"], 37.5)
        self.assertEqual(result["cpu_cores"]["1"]["mean"], 20)
        self.assertEqual(result["gpu_engines"]["luid_1_phys_0_eng_0 / 3D"]["mean"], 40)
        self.assertEqual(result["gpu_engines"]["luid_1_phys_0_eng_1 / Compute"]["mean"], 75)
        report = evaluation.markdown(result)
        self.assertIn("Logical CPU", report)
        self.assertIn("luid_1_phys_0_eng_0", report)

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
            prepare_spatial_active_slots=100, prepare_spatial_retained_slots=1000,
            prepare_spatial_changed_bounds=1, prepare_spatial_reused_bounds=999,
            prepare_spatial_dirty_leaves=1, prepare_spatial_refit_nodes=8,
            prepare_spatial_rebuilds=0, prepare_spatial_quality_rebuilds=0)])
        self.assertEqual(result["metrics"]["prepare_spatial_refit_nodes"]["mean"], 8)
        report = evaluation.markdown(result)
        self.assertIn("prepare_spatial_index_ms", report)
        self.assertIn("prepare_spatial_rebuilds", report)
        self.assertIn("prepare_spatial_active_slots", report)
        self.assertTrue(any("inactive historical bounds" in warning for warning in result["warnings"]))
        self.assertEqual(result["metrics"]["prepare_spatial_retained_slots"]["mean"], 1000)
        self.assertTrue(any("Spatial compare/refit/build" in warning for warning in result["warnings"]))

    def test_worker_batch_preparation_fields(self):
        result = evaluation.evaluate([dict(type="frame_details", frame_ms=20,
            render_batch_snapshot_ms=.1, render_batch_build_ms=.2, render_batch_wait_ms=.05,
            render_batch_eligible_draws=512, render_batch_cache_hits=1, render_batch_worker_jobs=1, render_batch_reused_keys=512)])
        self.assertEqual(result["metrics"]["render_batch_eligible_draws"]["mean"], 512)
        self.assertIn("render_batch_cache_hits", evaluation.markdown(result))
        self.assertTrue(any("Worker grouping overlaps" in warning for warning in result["warnings"]))

    def test_partial_gpu_instrumentation_can_be_reported(self):
        result = evaluation.evaluate([dict(type="frame_details", playing=True, dt_ms=20,
                                          gpu_valid=True, gpu_opaque_ms=2)])
        self.assertIsNone(result["gpu_shadow_share_percent"])
        self.assertIn("2.00 ms", evaluation.markdown(result))

    def test_render_work_balance_uses_unique_gpu_samples(self):
        rows = [dict(type="frame_details", frame_ms=20, render_cpu_total_ms=10,
                     gpu_valid=True, gpu_sample_id=1, gpu_opaque_ms=60, gpu_shadow_ms=10),
                dict(type="frame_details", frame_ms=20, render_cpu_total_ms=10,
                     gpu_valid=True, gpu_sample_id=1, gpu_opaque_ms=60, gpu_shadow_ms=10),
                dict(type="frame_details", frame_ms=20, render_cpu_total_ms=10,
                     gpu_valid=True, gpu_sample_id=2, gpu_opaque_ms=40, gpu_shadow_ms=10)]
        balance = evaluation.evaluate(rows)["render_work_balance"]
        self.assertEqual(balance["classification"], "GPU-heavy")
        self.assertEqual(balance["gpu_to_cpu_ratio"], 6)
        self.assertEqual(balance["largest_gpu_stage"], "gpu_opaque_ms")
        self.assertIn("GPU-heavy", evaluation.markdown(evaluation.evaluate(rows)))
        self.assertIsNone(evaluation.evaluate([dict(type="frame_details", frame_ms=20,
                                                    render_cpu_total_ms=10)])["render_work_balance"])

    def test_grass_gpu_stage_is_separate_from_opaque(self):
        result = evaluation.evaluate([dict(type="frame_details", frame_ms=20,
                                           render_cpu_total_ms=5, gpu_valid=True,
                                           gpu_grass_ms=8, gpu_opaque_ms=3,
                                           gpu_terrain_ms=2)])
        self.assertEqual(result["gpu_total_ms"]["mean"], 13)
        self.assertEqual(result["render_work_balance"]["largest_gpu_stage"], "gpu_grass_ms")
        self.assertIn("gpu_grass_ms", evaluation.markdown(result))

    def test_gpu_comparison_requires_matching_capture_conditions(self):
        identity = dict(renderer="dx11", benchmark_scope="render", route="resident",
                        route_version="v1", view_width=1848, view_height=1069,
                        shadow_cascades=4, shadow_atlas_resolution=2048)
        frame = dict(type="frame_details", playing=True, measured=True,
                     frame_ms=20, gpu_valid=True, gpu_sample_id=1,
                     gpu_grass_ms=8, **identity)
        baseline = evaluation.evaluate([frame])
        current = evaluation.evaluate([dict(frame, gpu_grass_ms=6)])
        report = evaluation.markdown(current, baseline)
        self.assertIn("Matched renderer, route", report)
        self.assertIn("| gpu_grass_ms | 8.000 | 6.000 | -2.000 ms |", report)
        changed = evaluation.evaluate([dict(frame, gpu_grass_ms=6, view_width=924)])
        report = evaluation.markdown(changed, baseline)
        self.assertIn("not verified as matched: view_width", report)
        self.assertNotIn("-2.000 ms", report)


if __name__ == "__main__":
    unittest.main()
