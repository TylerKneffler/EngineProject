"""Evaluate editor JSONL recordings without treating smoothed UI FPS as frame data."""
import argparse
import json
import math
import statistics
from pathlib import Path


def summary(values):
    values = sorted(v for v in values if isinstance(v, (int, float)) and math.isfinite(v))
    if not values:
        return None
    return {"mean": statistics.mean(values), "p95": values[math.ceil(.95 * len(values)) - 1],
            "p99": values[math.ceil(.99 * len(values)) - 1], "max": values[-1]}


def evaluate(rows, path="", all_frames=False):
    frames = [r for r in rows if r.get("type") == "frame_details" or
              (r.get("type") is None and r.get("schema_version", 0) >= 1 and "frame_ms" in r and "playing" in r)]
    aliases = {"grass_total_ms": "grass_ms", "grass_tiles_built": "grass_built",
               "grass_placements_scanned": "grass_scanned", "render_submission_ms": "render_submit_ms"}
    frames = [dict(r, **{key: r[alias] for key, alias in aliases.items() if key not in r and alias in r}) for r in frames]
    if not all_frames and any(r.get("playing") for r in frames):
        frames = [r for r in frames if r.get("playing")]
    if not all_frames and any("measured" in r for r in frames):
        frames = [r for r in frames if r.get("measured")]
    if not frames:
        raise ValueError("Recording contains no frame_details records")
    intervals = [r.get("dt_ms", r.get("frame_ms", 0)) for r in frames]
    intervals = [v for v in intervals if isinstance(v, (int, float)) and math.isfinite(v) and v > 0]
    interval = summary(intervals)
    if not interval:
        raise ValueError("Recording contains no valid positive frame intervals")
    metrics = {k: summary([r[k] for r in frames if k in r])
               for k in set().union(*(r.keys() for r in frames))
               if k.endswith("_ms") or k.startswith(("instance_", "shadow_instance_"))}
    gpu = {}
    for index, row in enumerate(frames):
        if row.get("gpu_valid"):
            gpu[row.get("gpu_sample_id", f"row:{index}")] = row
    gpu_stages = {key: summary([r[key] for r in gpu.values() if key in r])
                  for key in ("gpu_shadow_ms", "gpu_terrain_ms", "gpu_portal_ms", "gpu_opaque_ms", "gpu_transparent_ms")}
    gpu_total = summary([sum(r.get(key, 0) for key in gpu_stages) for r in gpu.values()])
    hardware = [r for r in rows if r.get("type") == "sample"]
    if not all_frames and any("measured" in r for r in hardware):
        hardware = [r for r in hardware if r.get("measured")]
    hardware_metrics = {key: summary([r[key] for r in hardware if key in r]) for key in
                        ("process_cpu_pct", "process_cpu_core_equivalents", "main_thread_cpu_pct",
                         "cpu_user_pct", "cpu_kernel_pct", "editor_working_set_bytes", "gpu_memory_used_bytes")}
    shadow_share = (gpu_stages["gpu_shadow_ms"]["mean"] / gpu_total["mean"] * 100
                    if gpu_total and gpu_total["mean"] and gpu_stages["gpu_shadow_ms"] else None)
    shadow_cycles = [(r.get("shadow_submission_wall_ms", r.get("shadow_draw_ms", 0)),
                      r.get("shadow_submission_thread_cycles", 0)) for r in frames]
    shadow_cycles = [(wall, cycles) for wall, cycles in shadow_cycles if wall > 0 and cycles > 0]
    median_density = statistics.median(cycles / wall for wall, cycles in shadow_cycles) if shadow_cycles else 0
    median_wall = statistics.median(wall for wall, cycles in shadow_cycles) if shadow_cycles else 0
    low_cycle_spikes = [(wall, cycles) for wall, cycles in shadow_cycles
                        if wall >= max(1, median_wall * 2) and cycles / wall < median_density * .5]
    shadow_submission = {"thread_cycles": summary([cycles for wall, cycles in shadow_cycles]),
                         "median_cycles_per_ms": median_density,
                         "low_cycle_density_spikes": len(low_cycle_spikes)}
    phase_keys = ("render_startup_ms", "render_terrain_prepare_ms", "render_occlusion_ms",
                  "render_culling_ms", "render_sorting_ms", "render_material_ms", "render_batching_ms",
                  "render_upload_ms", "render_submission_ms", "render_retained_ui_ms", "render_portal_ms", "render_other_ms")
    measured = sum(metrics[key]["mean"] for key in phase_keys if metrics.get(key))
    main = metrics.get("render_cpu_total_ms") or metrics.get("main_submit_ms")
    warnings = ["CPU wall time and asynchronously resolved GPU stages overlap; never add them.",
                "Automatic instance and triangle counts are candidates before GPU culling and LOD selection.",
                "Worker execution and record packing overlap main-thread phases; do not add them to frame time."]
    if shadow_cycles:
        warnings.append("Low thread-cycle density during long shadow submissions can indicate scheduling or waits; CPU frequency changes also affect density.")
    if "shadow_submission_thread_cpu_ms" in metrics:
        warnings.append("Shadow thread CPU uses OS accounting; off-CPU residual includes scheduling and waits, and does not prove a GPU stall.")
    missing = [key for key in phase_keys[:3] if key not in metrics]
    if missing:
        warnings.append("Capture predates full main-pass breakdown: " + ", ".join(missing))
    if any(r.get("views", 1) > 1 for r in frames) and not any(r.get("render_metrics_scope") == "all_main_views" for r in frames):
        warnings.append("Legacy render sub-timers cover the last view while main_submit_ms covers all views.")
    if not hardware_metrics["process_cpu_pct"]:
        warnings.append("CPU percentages describe the system, not editor CPU usage; process/thread counters are absent.")
    viewport_pixels = summary([r["view_pixels"] for r in frames if "view_pixels" in r])
    if any("shadow_casters" in r for r in hardware) and not any(r.get("schema_version", 0) >= 2 for r in frames):
        warnings.append("Legacy eligible-shadow-caster counts may be overwritten by main-view preparation.")
    return {"capture": str(path), "frames": len(frames), "start_utc": frames[0].get("time_utc"),
            "end_utc": frames[-1].get("time_utc"), "effective_fps": 1000 / interval["mean"],
            "interval_ms": interval, "over_60fps_budget_percent": 100 * sum(v > 1000 / 60 for v in intervals) / len(intervals),
            "metrics": metrics, "unique_gpu_samples": len(gpu), "gpu_stages_ms": gpu_stages,
            "gpu_total_ms": gpu_total, "gpu_shadow_share_percent": shadow_share,
            "main_pass_unexplained_ms": max(0, main["mean"] - measured) if main else None,
            "grass_tiles_prepared": sum(r.get("grass_tiles_built", 0) for r in frames),
            "grass_placements_indexed": sum(r.get("grass_placements_scanned", 0) for r in frames),
            "shadow_submission": shadow_submission,
            "hardware": hardware_metrics, "view_pixels": viewport_pixels, "warnings": warnings}


def load(path, all_frames=False):
    rows, malformed = [], 0
    with Path(path).open(encoding="utf-8-sig") as stream:
        for line in stream:
            if not line.strip():
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                malformed += 1
    result = evaluate(rows, path, all_frames)
    if malformed:
        result["warnings"].append(f"Ignored {malformed} malformed or incomplete JSONL rows.")
    return result


def markdown(result, comparison=None):
    lines = ["# Performance evaluation", "", f"Capture: `{result['capture']}`", "",
             f"{result['frames']} frames; **{result['effective_fps']:.2f} FPS** from frame intervals.",
             f"Frame interval: {result['interval_ms']['mean']:.2f} ms average, {result['interval_ms']['p95']:.2f} ms P95, "
             f"{result['interval_ms']['p99']:.2f} ms P99.", "", "| CPU wall-time measurement | Average ms | P95 ms |", "|---|---:|---:|"]
    for key in ("frame_ms", "update_ms", "render_ms", "main_submit_ms", "shadow_submit_ms", "editor_ui_ms",
                "grass_total_ms", "grass_generation_ms", "grass_upload_ms", "physics_ms", "pose_ms", "constraints_ms", "audio_ms",
                "render_startup_ms", "render_terrain_prepare_ms", "render_occlusion_ms",
                "shadow_draw_ms", "shadow_geometry_draw_ms", "shadow_submission_thread_cpu_ms",
                "shadow_submission_off_cpu_ms", "shadow_submission_wall_ms", "shadow_max_batch_ms",
                "render_retained_ui_ms", "render_portal_ms", "render_worker_active_ms", "render_main_job_ms",
                "render_worker_wait_ms", "render_record_pack_ms", "render_record_wait_ms"):
        if value := result["metrics"].get(key):
            lines.append(f"| {key} | {value['mean']:.3f} | {value['p95']:.3f} |")
    if result["shadow_submission"]["thread_cycles"]:
        lines += ["", f"Shadow submission: {result['shadow_submission']['thread_cycles']['mean']:.0f} thread cycles on average; "
                  f"{result['shadow_submission']['low_cycle_density_spikes']} long submissions had unusually low cycle density."]
    lines += ["", "| Resolved GPU stage | Average ms | P95 ms |", "|---|---:|---:|"]
    for key, value in result["gpu_stages_ms"].items():
        if value:
            lines.append(f"| {key} | {value['mean']:.3f} | {value['p95']:.3f} |")
    if result["gpu_total_ms"]:
        lines += ["", f"{result['unique_gpu_samples']} unique resolved GPU samples; measured stages total "
                  f"{result['gpu_total_ms']['mean']:.2f} ms on average."]
        if result["gpu_shadow_share_percent"] is not None:
            lines.append(f"Shadows account for {result['gpu_shadow_share_percent']:.1f}% of measured GPU time.")
    storage = {key: value for key, value in result["metrics"].items()
               if key.startswith(("instance_", "shadow_instance_")) and value}
    if storage:
        lines += ["", "| Instance storage measurement | Average | P95 | Maximum |", "|---|---:|---:|---:|"]
        for key, value in sorted(storage.items()):
            lines.append(f"| {key} | {value['mean']:.1f} | {value['p95']:.0f} | {value['max']:.0f} |")
    if result["main_pass_unexplained_ms"] is not None:
        lines += ["", f"Main-pass time outside available sub-timers: {result['main_pass_unexplained_ms']:.2f} ms average."]
    lines += ["", f"Grass prepared {result['grass_tiles_prepared']} instance tiles and indexed "
              f"{result['grass_placements_indexed']} placements during this capture."]
    if comparison:
        change = (1 - result["interval_ms"]["mean"] / comparison["interval_ms"]["mean"]) * 100
        lines += ["", f"Comparison: {comparison['effective_fps']:.2f} to {result['effective_fps']:.2f} FPS; "
                  f"average frame interval decreased {change:.1f}%. View size, scene, and motion may differ."]
        if result.get("view_pixels") and comparison.get("view_pixels"):
            pixel_change=(result["view_pixels"]["mean"]/comparison["view_pixels"]["mean"]-1)*100
            lines += [f"Rendered pixel count changed {pixel_change:+.1f}%; this is not a controlled benchmark comparison."]
    lines += ["", "## Interpretation", ""]
    if result["gpu_shadow_share_percent"] and result["gpu_shadow_share_percent"] > 50:
        lines.append("- GPU shadows dominate measured GPU work; evaluate cascade resolution/count, shadow distance, and grass caster distance.")
    if result["main_pass_unexplained_ms"] and result["main_pass_unexplained_ms"] > 1:
        lines.append("- A substantial main-pass cost is unmeasured. Capture again with startup, terrain preparation, and occlusion sub-timers before attributing it to draw calls.")
    lines += ["", "## Measurement limits", ""] + [f"- {warning}" for warning in result["warnings"]]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--output", type=Path, help="Output prefix for .json and .md reports")
    parser.add_argument("--all-frames", action="store_true")
    args = parser.parse_args()
    result = load(args.capture, args.all_frames)
    comparison = load(args.compare, args.all_frames) if args.compare else None
    report = markdown(result, comparison)
    print(report)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.with_suffix(".json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        args.output.with_suffix(".md").write_text(report, encoding="utf-8")


if __name__ == "__main__":
    main()
