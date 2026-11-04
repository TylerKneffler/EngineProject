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


MATCHED_ROUTE_FIELDS = ("renderer", "benchmark_scope", "route", "route_version",
                        "view_width", "view_height", "shadow_cascades",
                        "shadow_atlas_resolution")
GPU_ENGINE_CATEGORIES = ("3D", "Compute", "Copy", "Video decode", "Video encode", "Other")


def capture_identity(frames):
    identity = {}
    for key in MATCHED_ROUTE_FIELDS:
        values = {frame.get(key) for frame in frames}
        identity[key] = next(iter(values)) if len(values) == 1 else None
    return identity


def evaluate(rows, path="", all_frames=False):
    frames = [r for r in rows if r.get("type") == "frame_details" or
              (r.get("type") is None and r.get("schema_version", 0) >= 1 and "frame_ms" in r and "playing" in r)]
    aliases = {"grass_total_ms": "grass_ms", "grass_tiles_built": "grass_built",
               "grass_placements_scanned": "grass_scanned", "render_submission_ms": "render_submit_ms"}
    frames = [dict(r, **{key: r[alias] for key, alias in aliases.items() if key not in r and alias in r}) for r in frames]
    session_frames = frames
    session_intervals = summary([r.get("dt_ms", r.get("frame_ms", 0)) for r in session_frames
                                 if isinstance(r.get("dt_ms", r.get("frame_ms", 0)), (int, float))
                                 and r.get("dt_ms", r.get("frame_ms", 0)) > 0])
    stalls = []
    for index, row in enumerate(session_frames):
        if not isinstance(row.get("frame_ms"), (int, float)) or not math.isfinite(row["frame_ms"]) or row["frame_ms"] < 1000:
            continue
        stages = {key: row.get(key, 0) for key in
                  ("pending_load_ms", "update_ms", "render_ms", "pacing_ms", "background_ms")}
        stages = {key: value for key, value in stages.items() if isinstance(value, (int, float)) and math.isfinite(value)}
        stage = max(stages, key=stages.get) if stages and max(stages.values()) > 0 else None
        stopping = bool(row.get("playing") and index + 1 < len(session_frames)
                        and session_frames[index + 1].get("playing") is False)
        stalls.append(dict(frame=row.get("frame"), time_utc=row.get("time_utc"),
                           frame_ms=row["frame_ms"], stage=stage,
                           stage_ms=stages.get(stage, 0), play_stop_transition=stopping,
                           activity=row.get("frame_activity", [])))
    activity_frames = [dict(frame=row.get("frame"), frame_ms=row.get("frame_ms", 0),
                            activity=row.get("frame_activity", [])) for row in session_frames
                       if row.get("frame_activity")]
    selection = "all recorded frames"
    if not all_frames and any(r.get("playing") for r in frames):
        frames = [r for r in frames if r.get("playing")]
        selection = "Play frames"
    if not all_frames and any("measured" in r for r in frames):
        frames = [r for r in frames if r.get("measured")]
        selection = "measured Play frames" if selection == "Play frames" else "measured frames"
    if not all_frames and any("recurring_frame" in r for r in session_frames):
        # dt_ms measures the preceding interval. Exclude the sample after a
        # tagged hitch as well as the hitch frame itself.
        recurring = {id(row) for index, row in enumerate(session_frames)
                     if row.get("recurring_frame") and
                     (index == 0 or session_frames[index - 1].get("recurring_frame"))}
        frames = [row for row in frames if id(row) in recurring]
        selection = "recurring " + selection
    if not frames:
        raise ValueError("Recording contains no frame_details records")
    intervals = [r.get("dt_ms", r.get("frame_ms", 0)) for r in frames]
    intervals = [v for v in intervals if isinstance(v, (int, float)) and math.isfinite(v) and v > 0]
    interval = summary(intervals)
    if not interval:
        raise ValueError("Recording contains no valid positive frame intervals")
    metrics = {k: summary([r[k] for r in frames if k in r])
               for k in set().union(*(r.keys() for r in frames))
               if k.endswith("_ms") or k.startswith(("instance_", "shadow_instance_", "shadow_state_", "prepare_transform_", "prepare_spatial_", "prepare_item_", "prepare_instanced_", "render_batch_", "render_cluster_", "render_instance_tile_", "writer_queue_", "worker_queue_"))
               or k in ("native_compute_arena_writes", "native_compute_discard_maps",
                        "pipeline_queue_waits", "pipeline_retained_frames")}
    ui_panels = {}
    for row in frames:
        # Multiple documents can share a title; combine their cost per frame.
        panel_frame = {}
        for panel in row.get("ui_panels", []):
            title, value = panel.get("title"), panel.get("draw_ms")
            if isinstance(title, str) and isinstance(value, (int, float)) and math.isfinite(value):
                panel_frame[title] = panel_frame.get(title, 0) + value
        for title, value in panel_frame.items():
            ui_panels.setdefault(title, []).append(value)
    ui_panels = {title: dict(summary(values), frames=len(values)) for title, values in ui_panels.items()}
    stage_work = {}
    for stage in ("pacing", "background", "update", "render", "pending_load"):
        values = [item for row in frames for item in row.get("stage_work", [])
                  if isinstance(item, dict) and item.get("stage") == stage]
        if values:
            stage_work[stage] = {key: summary([item[key] for item in values if key in item])
                                 for key in ("wall_ms", "main_busy_ms", "worker_execution_ms",
                                             "queue_delay_ms", "dependency_wait_ms",
                                             "scheduler_wait_ms", "render_thread_stall_ms")}
    gpu = {}
    for index, row in enumerate(frames):
        if row.get("gpu_valid"):
            gpu[row.get("gpu_sample_id", f"row:{index}")] = row
    gpu_stages = {key: summary([r[key] for r in gpu.values() if key in r])
                  for key in ("gpu_shadow_ms", "gpu_terrain_ms", "gpu_grass_ms", "gpu_portal_ms", "gpu_opaque_ms", "gpu_transparent_ms")}
    gpu_total = summary([sum(r.get(key, 0) for key in gpu_stages) for r in gpu.values()])
    hardware = [r for r in rows if r.get("type") == "sample"]
    if not all_frames and any("measured" in r for r in hardware):
        hardware = [r for r in hardware if r.get("measured")]
    hardware_metrics = {key: summary([r[key] for r in hardware if key in r]) for key in
                        ("process_cpu_pct", "process_cpu_core_equivalents", "main_thread_cpu_pct",
                         "cpu_user_pct", "cpu_kernel_pct", "editor_working_set_bytes", "gpu_memory_used_bytes")}
    cpu_cores = {}
    gpu_engines = {}
    for row in hardware:
        for index, load in enumerate(row.get("cpu_cores_pct", [])):
            if (isinstance(load, list) and len(load) == 2 and
                    all(isinstance(v, (int, float)) and math.isfinite(v) for v in load)):
                cpu_cores.setdefault(index, []).append(sum(load))
        for engine in row.get("gpu_engine_instances", []):
            if (isinstance(engine, list) and len(engine) == 3 and
                    isinstance(engine[0], str) and isinstance(engine[1], int) and
                    isinstance(engine[2], (int, float)) and math.isfinite(engine[2])):
                gpu_engines.setdefault((engine[0], engine[1]), []).append(engine[2])
    cpu_cores = {str(index): summary(values) for index, values in sorted(cpu_cores.items())}
    gpu_engines = {f"{identity} / {GPU_ENGINE_CATEGORIES[category] if 0 <= category < len(GPU_ENGINE_CATEGORIES) else 'Other'}": summary(values)
                   for (identity, category), values in sorted(gpu_engines.items())}
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
    render_balance = None
    render_cpu = metrics.get("render_cpu_total_ms")
    if gpu_total and render_cpu and render_cpu["mean"] > 0:
        ratio = gpu_total["mean"] / render_cpu["mean"]
        dominant = max(((key, value["mean"]) for key, value in gpu_stages.items() if value),
                       key=lambda item: item[1], default=None)
        render_balance = {"gpu_to_cpu_ratio": ratio,
                          "classification": "GPU-heavy" if ratio >= 2 else
                                            "CPU-render-heavy" if ratio <= .5 else "mixed",
                          "largest_gpu_stage": dominant[0] if dominant else None,
                          "largest_gpu_stage_share_percent":
                              100 * dominant[1] / gpu_total["mean"] if dominant and gpu_total["mean"] else None}
    warnings = ["CPU wall time and asynchronously resolved GPU stages overlap; never add them.",
                "Automatic instance and triangle counts are candidates before GPU culling and LOD selection.",
                "Worker execution and record packing overlap main-thread phases; do not add them to frame time."]
    if shadow_cycles:
        warnings.append("Low thread-cycle density during long shadow submissions can indicate scheduling or waits; CPU frequency changes also affect density.")
    if "shadow_submission_thread_cpu_ms" in metrics:
        warnings.append("Shadow thread CPU uses OS accounting; off-CPU residual includes scheduling and waits, and does not prove a GPU stall.")
    native_frames = [r for r in frames if r.get("native_timing_enabled")]
    native_calls = {}
    native_stall_frames = {}
    for phase in ("other", "camera", "shadow"):
        for operation in ("map", "unmap", "upload", "bind", "dispatch", "indirect_args", "draw", "resource_create", "readback", "fence_wait"):
            prefix = f"native_{phase}_{operation}"
            samples = [r for r in native_frames if prefix + "_calls" in r]
            if not samples:
                continue
            native_calls[prefix] = {
                "frames": len(samples),
                "calls": summary([r[prefix + "_calls"] for r in samples]),
                "wall_ms": summary([r.get(prefix + "_ms", 0) for r in samples]),
                "maximum_call_ms": summary([r.get(prefix + "_max_ms", 0) for r in samples]),
                "cycles_at_max_call": max(samples, key=lambda r: r.get(prefix + "_max_ms", 0)).get(prefix + "_max_call_cycles", 0),
                "thread_cycles": summary([r.get(prefix + "_thread_cycles", 0) for r in samples]),
                "cycle_samples": sum(r.get(prefix + "_cycle_samples", 0) for r in samples)}
            stalls = [r for r in samples if r.get(prefix + "_max_ms", 0) >= 5.0]
            if stalls:
                native_stall_frames[prefix] = len(stalls)
    if native_frames:
        warnings.append("Native graphics-call timing is opt-in and adds per-call clock/cycle-query overhead. Compare performance with it disabled; long calls with low cycles are wait candidates, not proof of GPU synchronization.")
    if any(r.get("editor_play_pipeline") for r in frames):
        warnings.append("Editor Play simulation runs on its owner worker and overlaps graphics submission. update_ms measures only the serial update scope; simulation_ms and simulation_overlap_ms overlap render_ms and must not be added to it. Overlap is wall time, not a measurement of simultaneous CPU busy time. Main/shadow submission scopes measure packet replay in this mode; compare pipeline_record_ms too, rather than interpreting shorter replay scopes as saved total work.")
    if "prepare_transforms_ms" in metrics:
        warnings.append("Transform capture, compute and commit partition prepare_transforms_ms, which is already inside scene_prepare_ms; these nested wall timings must not be added to the frame total.")
    fallback = metrics.get("prepare_transform_structure_fallback")
    if fallback and fallback["max"]:
        warnings.append("Structure-change frames use lazy owner transform validation inside item/bone preparation; dedicated transform counters cover the batch path only.")
    if "prepare_spatial_retained_slots" in metrics:
        warnings.append("Retained spatial slots include inactive historical bounds and free slots; only active frame items can submit draws. Capacity is a scene-lifetime high-water mark, not an object or draw count.")
    if "render_batch_build_ms" in metrics:
        warnings.append("Batch snapshot, grouping and synchronization are nested in render_batching_ms. Worker grouping overlaps owner draw statistics and synchronization waits, so its duration must not be added to batching wall time. Cache hits count views where every active key reused its assignment, not individual groups. Reused-key counts include partial hits during membership changes.")
    if "prepare_spatial_index_ms" in metrics:
        warnings.append("Spatial compare/refit/build timings partition prepare_spatial_index_ms, already contained in prepare_spatial_ms and scene preparation. Refit jobs can overlap worker execution counters.")
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
    writers = [r for r in rows if r.get("type") == "writer_summary"]
    recording_loss = sum(r.get("dropped_records", 0) + r.get("failed_records", 0) for r in writers)
    recording_loss = max(recording_loss, max((r.get("recording_dropped_records", 0) + r.get("recording_failed_records", 0)
                                             for r in hardware), default=0))
    if recording_loss:
        warnings.append(f"Recording writer dropped or failed {recording_loss} records; frame statistics may omit stalls.")
    if stalls:
        warnings.append("Full-recording stalls are reported before Play/measured filtering. A Stop freeze can fall outside Play-only interval FPS because dt_ms describes the preceding frame and arrives on the next stopped frame. pending_load_ms includes post-frame scene restoration, not only file loads.")
    if activity_frames:
        warnings.append("Recurring-frame statistics exclude tagged editor activities and the following interval. Activity tags identify frames containing import, save, bake, load, or initial draw work; overlapping activity is listed together. Startup work before the first recorded frame is outside this capture.")
    return {"capture": str(path), "frames": len(frames), "frame_selection": selection,
            "session": {"frames": len(session_frames), "interval_ms": session_intervals,
                        "effective_fps": 1000 / session_intervals["mean"] if session_intervals else None,
                        "stalls": sorted(stalls, key=lambda row: row["frame_ms"], reverse=True),
                        "activity_frames": activity_frames},
            "capture_identity": capture_identity(frames),
            "start_utc": frames[0].get("time_utc"),
            "end_utc": frames[-1].get("time_utc"), "effective_fps": 1000 / interval["mean"],
            "interval_ms": interval, "over_60fps_budget_percent": 100 * sum(v > 1000 / 60 for v in intervals) / len(intervals),
            "metrics": metrics, "stage_work": stage_work, "ui_panels_ms": ui_panels,
            "unique_gpu_samples": len(gpu), "gpu_stages_ms": gpu_stages,
            "gpu_total_ms": gpu_total, "gpu_shadow_share_percent": shadow_share,
            "render_work_balance": render_balance,
            "main_pass_unexplained_ms": max(0, main["mean"] - measured) if main else None,
            "grass_tiles_prepared": sum(r.get("grass_tiles_built", 0) for r in frames),
            "grass_placements_indexed": sum(r.get("grass_placements_scanned", 0) for r in frames),
            "shadow_submission": shadow_submission,
            "hardware": hardware_metrics, "cpu_cores": cpu_cores,
            "gpu_engines": gpu_engines, "view_pixels": viewport_pixels,
            "recording_lost_records": recording_loss, "native_timed_frames": len(native_frames),
            "native_calls": native_calls,
            "native_stall_frames_5ms": native_stall_frames,
            "warnings": warnings}


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
             f"{result['frames']} {result.get('frame_selection', 'frames')}; **{result['effective_fps']:.2f} FPS** from frame intervals.",
             f"Frame interval: {result['interval_ms']['mean']:.2f} ms average, {result['interval_ms']['p95']:.2f} ms P95, "
             f"{result['interval_ms']['p99']:.2f} ms P99."]
    session = result.get("session", {})
    if session.get("stalls"):
        if session.get("effective_fps"):
            lines += ["", f"Full recording, including transitions: {session['frames']} frames, {session['effective_fps']:.2f} FPS from recorded intervals."]
        lines += ["", "| Blocking frame | Total ms | Dominant recorded stage | Stage ms | Activity | Play/Stop transition |", "|---|---:|---|---:|---|---|"]
        for stall in session["stalls"][:10]:
            lines.append(f"| {stall['frame']} | {stall['frame_ms']:.1f} | {stall['stage'] or 'unattributed'} | {stall['stage_ms']:.1f} | {', '.join(stall['activity']) or 'recurring'} | {'yes' if stall['play_stop_transition'] else 'no'} |")
    if session.get("activity_frames"):
        lines += ["", "| Activity frame | Total ms | Activity |", "|---|---:|---|"]
        for hitch in sorted(session["activity_frames"], key=lambda row: row["frame_ms"], reverse=True)[:10]:
            lines.append(f"| {hitch['frame']} | {hitch['frame_ms']:.1f} | {', '.join(hitch['activity'])} |")
    lines += ["", "| CPU wall-time measurement | Average ms | P95 ms |", "|---|---:|---:|"]
    for key in ("frame_ms", "update_ms", "render_ms", "main_submit_ms", "shadow_submit_ms", "editor_ui_ms",
                "ui_thread_cpu_ms", "render_thread_cpu_ms", "writer_queue_records", "writer_queue_bytes", "worker_queue_pending",
                "simulation_ms", "simulation_cpu_ms", "simulation_wait_ms", "simulation_overlap_ms",
                "simulation_gpu_wait_ms", "pipeline_record_ms", "pipeline_compute_record_ms", "pipeline_snapshot_ms", "pipeline_view_record_ms", "pipeline_record_other_ms", "pipeline_queue_wait_ms",
                "grass_total_ms", "grass_generation_ms", "grass_upload_ms", "physics_ms", "pose_ms", "constraints_ms", "audio_ms",
                "render_startup_ms", "render_cluster_prepare_ms", "render_terrain_prepare_ms", "render_occlusion_ms",
                "shadow_draw_ms", "shadow_geometry_draw_ms", "shadow_submission_thread_cpu_ms",
                "shadow_submission_off_cpu_ms", "shadow_submission_wall_ms", "shadow_max_batch_ms",
                "render_retained_ui_ms", "render_portal_ms", "render_worker_active_ms", "render_main_job_ms",
                "render_worker_wait_ms", "render_record_pack_ms", "render_record_wait_ms", "prepare_transforms_ms",
                "prepare_transform_snapshot_ms", "prepare_transform_compute_ms", "prepare_transform_commit_ms",
                "scene_prepare_ms", "prepare_items_ms", "prepare_item_validation_ms", "prepare_item_textures_ms",
                "prepare_item_shaders_ms", "prepare_item_mesh_state_ms", "prepare_item_bounds_ms",
                "prepare_item_lod_ms", "prepare_item_build_ms",
                "prepare_spatial_index_ms", "prepare_spatial_compare_ms", "prepare_spatial_refit_ms", "prepare_spatial_build_ms"):
        if value := result["metrics"].get(key):
            lines.append(f"| {key} | {value['mean']:.3f} | {value['p95']:.3f} |")
    if result.get("ui_panels_ms"):
        lines += ["", "UI panel times are nested in editor_ui_ms; statistics use frames where that panel was present.",
                  "", "| UI panel | Frames | Mean ms | P95 ms | P99 ms |", "|---|---:|---:|---:|---:|"]
        for title, value in sorted(result["ui_panels_ms"].items(), key=lambda item: item[1]["mean"], reverse=True):
            title = title.replace("|", "\\|").replace("\n", " ").replace("\r", " ")
            lines.append(f"| {title} | {value['frames']} | {value['mean']:.3f} | {value['p95']:.3f} | {value['p99']:.3f} |")
        for key in ("ui_panels_ms", "ui_other_ms"):
            if value := result["metrics"].get(key):
                lines.append(f"\n{key}: mean {value['mean']:.3f} ms, P95 {value['p95']:.3f} ms.")
    if sources := result["metrics"].get("prepare_item_shader_resolutions"):
        requests = result["metrics"].get("prepare_item_shader_requests")
        lines.append(f"\nShader preparation: {sources['mean']:.2f} unique source checks per frame" +
                     (f" for {requests['mean']:.2f} requests." if requests else "."))
    if result["metrics"].get("render_instance_tile_prepare_ms"):
        lines += ["", "| Instance tile preparation | Mean | P95 |", "|---|---:|---:|"]
        for key in ("render_instance_tile_prepare_ms", "render_instance_tile_reused", "render_instance_tile_built", "render_instance_tile_worker_passes"):
            if value := result["metrics"].get(key):
                lines.append(f"| {key} | {value['mean']:.3f} | {value['p95']:.3f} |")
    if waits := result["metrics"].get("pipeline_queue_waits"):
        retained = result["metrics"].get("pipeline_retained_frames")
        lines.append(f"\nPlay queue waits: {waits['mean']:.3f} per frame; "
                     f"retained frames: {retained['mean']:.2f} average, "
                     f"{retained['max']:.0f} maximum." if retained else
                     f"\nPlay queue waits: {waits['mean']:.3f} per frame.")
    if result.get("stage_work"):
        lines += ["", "| Frame stage | Wall ms | Main CPU ms | Worker execution ms | Queue delay ms |", "|---|---:|---:|---:|---:|"]
        for name, work in result["stage_work"].items():
            value = lambda key: work[key]["mean"] if work.get(key) else 0
            lines.append(f"| {name} | {value('wall_ms'):.3f} | {value('main_busy_ms'):.3f} | {value('worker_execution_ms'):.3f} | {value('queue_delay_ms'):.3f} |")
    for key in ("prepare_transform_local_updated", "prepare_transform_world_updated", "prepare_transform_world_reused", "prepare_transform_levels", "prepare_transform_structure_fallback",
                "prepare_spatial_active_slots", "prepare_spatial_retained_slots",
                "prepare_spatial_changed_bounds", "prepare_spatial_reused_bounds", "prepare_spatial_dirty_leaves",
                "prepare_spatial_refit_nodes", "prepare_spatial_rebuilds", "prepare_spatial_quality_rebuilds"):
        if value := result["metrics"].get(key):
            lines += ["", f"{key}: {value['mean']:.1f} per frame; maximum {value['max']:.0f}."]
    for key in ("native_compute_arena_writes", "native_compute_discard_maps"):
        if value := result["metrics"].get(key):
            lines += ["", f"{key}: {value['mean']:.1f} per frame; maximum {value['max']:.0f}."]
    if result.get("native_calls"):
        lines += ["", f"Native call diagnostics: {result['native_timed_frames']} instrumented frames.",
                  "", "| Native call group | Calls/frame | Wall ms/frame | Worst call ms | Cycles in worst call |", "|---|---:|---:|---:|---:|"]
        for key, value in sorted(result["native_calls"].items()):
            if value["calls"]["max"]:
                lines.append(f"| {key} | {value['calls']['mean']:.1f} | {value['wall_ms']['mean']:.3f} | {value['maximum_call_ms']['max']:.3f} | {value['cycles_at_max_call']:.0f} |")
        if result.get("native_stall_frames_5ms"):
            groups = ", ".join(f"{key}: {count}" for key, count in
                               sorted(result["native_stall_frames_5ms"].items()))
            lines.append(f"Frames with a native call of at least 5 ms: {groups}.")
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
    if result.get("cpu_cores"):
        lines += ["", "| Logical CPU | Mean usage % | P95 usage % |",
                  "|---|---:|---:|"]
        for core, usage in result["cpu_cores"].items():
            lines.append(f"| {core} | {usage['mean']:.1f} | {usage['p95']:.1f} |")
    if result.get("gpu_engines"):
        lines += ["", "| GPU engine (adapter LUID / category) | Mean usage % | P95 usage % |",
                  "|---|---:|---:|"]
        for engine, usage in result["gpu_engines"].items():
            lines.append(f"| {engine} | {usage['mean']:.1f} | {usage['p95']:.1f} |")
    if balance := result.get("render_work_balance"):
        lines += ["", f"Render work balance: **{balance['classification']}**; measured GPU stages total "
                  f"{balance['gpu_to_cpu_ratio']:.1f} times the CPU render wall time on average."]
        if balance["largest_gpu_stage"]:
            lines.append(f"Largest GPU stage: {balance['largest_gpu_stage']} "
                         f"({balance['largest_gpu_stage_share_percent']:.1f}% of measured GPU stage time).")
        lines.append("GPU stages resolve asynchronously; this comparison describes measured work, not device utilization or an exact frame critical path.")
    storage = {key: value for key, value in result["metrics"].items()
               if key.startswith(("instance_", "shadow_instance_")) and value}
    if storage:
        lines += ["", "| Instance storage measurement | Average | P95 | Maximum |", "|---|---:|---:|---:|"]
        for key, value in sorted(storage.items()):
            lines.append(f"| {key} | {value['mean']:.1f} | {value['p95']:.0f} | {value['max']:.0f} |")
    batches = {key: value for key, value in result["metrics"].items()
               if key.startswith("render_batch_") and value}
    if batches:
        lines += ["", "| Batch preparation measurement | Average | P95 | Maximum |", "|---|---:|---:|---:|"]
        for key, value in sorted(batches.items()):
            lines.append(f"| {key} | {value['mean']:.3f} | {value['p95']:.3f} | {value['max']:.3f} |")
    if result["main_pass_unexplained_ms"] is not None:
        lines += ["", f"Main-pass time outside available sub-timers: {result['main_pass_unexplained_ms']:.2f} ms average."]
    lines += ["", f"Grass prepared {result['grass_tiles_prepared']} instance tiles and indexed "
              f"{result['grass_placements_indexed']} placements during this capture."]
    if comparison:
        change = (1 - result["interval_ms"]["mean"] / comparison["interval_ms"]["mean"]) * 100
        lines += ["", f"Comparison: {comparison['effective_fps']:.2f} to {result['effective_fps']:.2f} FPS; "
                  f"average frame interval decreased {change:.1f}%."]
        current_identity = result.get("capture_identity", {})
        previous_identity = comparison.get("capture_identity", {})
        missing = [key for key in MATCHED_ROUTE_FIELDS
                   if current_identity.get(key) is None or previous_identity.get(key) is None]
        mismatched = [key for key in MATCHED_ROUTE_FIELDS
                      if key not in missing and current_identity[key] != previous_identity[key]]
        if missing or mismatched:
            lines.append("Capture conditions are not verified as matched: " +
                         ", ".join(mismatched + missing) + ". GPU stage deltas are omitted.")
        else:
            lines += ["Matched renderer, route, view size, and shadow settings. Motion and hardware load still require review.",
                      "", "| GPU stage | Baseline ms | Current ms | Change |", "|---|---:|---:|---:|"]
            for key, current in result["gpu_stages_ms"].items():
                previous = comparison["gpu_stages_ms"].get(key)
                if current and previous:
                    lines.append(f"| {key} | {previous['mean']:.3f} | {current['mean']:.3f} | "
                                 f"{current['mean'] - previous['mean']:+.3f} ms |")
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
