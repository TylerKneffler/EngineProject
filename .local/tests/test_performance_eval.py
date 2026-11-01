import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("evaluate_performance", Path(__file__).resolve().parents[2] / "Scripts/evaluate_performance.py")
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)


class PerformanceEvaluationTests(unittest.TestCase):
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

    def test_partial_gpu_instrumentation_can_be_reported(self):
        result = evaluation.evaluate([dict(type="frame_details", playing=True, dt_ms=20,
                                          gpu_valid=True, gpu_opaque_ms=2)])
        self.assertIsNone(result["gpu_shadow_share_percent"])
        self.assertIn("2.00 ms", evaluation.markdown(result))


if __name__ == "__main__":
    unittest.main()
