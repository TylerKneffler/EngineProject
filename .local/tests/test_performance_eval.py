"""Regression coverage for panel attribution in recorded frame evaluations."""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location(
    "performance_eval", Path(__file__).resolve().parents[1] / "scripts" / "evaluate_performance.py")
EVAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVAL)


def frame(panels=None, **fields):
    result = dict(type="frame_details", playing=True, measured=True,
                  frame_ms=10., dt_ms=10., editor_ui_ms=4.)
    if panels is not None:
        result["ui_panels"] = panels
    result.update(fields)
    return result


class UiPanelEvaluationTests(unittest.TestCase):
    def test_duplicate_documents_are_summed_per_frame(self):
        result = EVAL.evaluate([
            frame([dict(title="Document", draw_ms=1.), dict(title="Document", draw_ms=2.)]),
            frame([dict(title="Document", draw_ms=5.)]),
        ])
        panel = result["ui_panels_ms"]["Document"]
        self.assertEqual(panel["mean"], 4.)
        self.assertEqual(panel["p95"], 5.)
        self.assertEqual(panel["frames"], 2)

    def test_warmup_and_invalid_samples_excluded(self):
        result = EVAL.evaluate([
            frame([dict(title="Warmup", draw_ms=100.)], measured=False),
            frame([dict(title="Game", draw_ms=.2), dict(title="Invalid", draw_ms=float("nan"))]),
            frame(),
        ])
        self.assertEqual(set(result["ui_panels_ms"]), {"Game"})
        self.assertEqual(result["ui_panels_ms"]["Game"]["frames"], 1)

    def test_legacy_trace_remains_supported(self):
        self.assertEqual(EVAL.evaluate([frame()])["ui_panels_ms"], {})

    def test_report_preserves_names_without_breaking_table(self):
        result = EVAL.evaluate([frame([dict(title='Asset | "document"\nview', draw_ms=.25)])])
        report = EVAL.markdown(result)
        self.assertIn('Asset \\| "document" view', report)
        self.assertIn("nested in editor_ui_ms", report)


if __name__ == "__main__":
    unittest.main()
