"""Summarize controlled forest diagnostics; GPU samples are deduplicated by the recorder."""
import argparse
import hashlib
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[1]
CASE_FIELDS = ("backend", "view", "mode", "impostor_distance", "width", "height", "shadows",
               "build_configuration", "assertions_enabled", "route", "camera_position", "camera_target",
               "warmup_frames", "measurement_frames", "cpu_profile", "workload_counters_requested", "scissor_mode")


def case_key(row):
    # Missing identity remains visible in historical summaries, but cannot match.
    return tuple(json.dumps(row.get(field), sort_keys=True) for field in CASE_FIELDS)


def load_manifest(directory):
    path = directory / "provenance.json"
    if not path.exists():
        raise ValueError(f"Missing capture provenance: {path}; rerun with run_forest_performance.py")
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema") != 1 or not value.get("inputs_unchanged") or not value.get("executable_unchanged") or value.get("returncode") != 0:
        raise ValueError(f"Incomplete or changed capture provenance: {path}")
    for field in ("executable_sha256", "inputs_sha256", "environment", "platform", "cmake_cache_sha256"):
        if field not in value or value[field] is None:
            raise ValueError(f"Missing {field} in {path}")
    recordings = value.get("recordings_sha256")
    if not recordings:
        raise ValueError(f"Missing recording identity in {path}")
    for name, digest in recordings.items():
        recording = directory / name
        if recording.parent != directory or not recording.is_file() or hashlib.sha256(recording.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Recording changed after capture: {name}")
    if set(recordings) != {p.name for p in directory.glob("*.jsonl")}:
        raise ValueError(f"Capture contains recordings from another run: {directory}")
    return value


def validate_comparison(current, baseline, rows):
    for field in ("inputs_sha256", "environment", "platform", "cmake_cache_sha256"):
        if current[field] != baseline[field]:
            raise ValueError(f"Cannot make matched comparison: {field} differs")
    for row in rows:
        if any(row.get(field) is None for field in CASE_FIELDS) or row.get("build_configuration") == "unknown":
            raise ValueError("Cannot match historical rows with incomplete build/route/settings identity")


def isolate_costs(summary):
    """Measured vertex/front-end control and incremental raster/fragment cost.

    This is an ablation, not a vendor claim of additive shader execution clocks.
    Identical VS/IA counts and zero PS work are required before reporting a pair.
    """
    fields = tuple(field for field in CASE_FIELDS if field not in ("mode", "scissor_mode")) + ("executable_path",)
    key = lambda row: tuple(json.dumps(row.get(field), sort_keys=True) for field in fields)
    controls = {(row["mode"], key(row)): row for row in summary if row["mode"].endswith("-vertex-only")}
    result = []
    for row in summary:
        if row["mode"] not in ("bark", "leaves"):
            continue
        control = controls.get((row["mode"] + "-vertex-only", key(row)))
        if not control:
            continue
        if not row.get("workload_counters_requested"):
            continue
        if min(row["workload_samples"], control["workload_samples"]) < 20:
            raise ValueError("Insufficient completed workload samples for cost isolation")
        category = "tree_bark" if row["mode"] == "bark" else "tree_leaves"
        prefix = "gpu_" + category
        a, b = row["workload_mean"], control["workload_mean"]
        for suffix in ("_vs_invocations", "_primitives"):
            if a[prefix+suffix] <= 0 or a[prefix+suffix] != b[prefix+suffix]:
                raise ValueError("Vertex-only control changed executed geometry")
        if b[prefix+"_ps_invocations"] != 0 or a[prefix+"_ps_invocations"] <= 0:
            raise ValueError("Vertex-only control did not isolate fragment work")
        full = row["phase_mean_ms"][prefix+"_ms"]
        front = control["phase_mean_ms"][prefix+"_ms"]
        result.append(dict(backend=row["backend"], view=row["view"], category=category,
            width=row["width"], height=row["height"], full_ms=full,
            vertex_front_end_control_ms=front, incremental_raster_fragment_ms=full-front,
            vs_invocations=a[prefix+"_vs_invocations"], primitives=a[prefix+"_primitives"],
            ps_invocations=a[prefix+"_ps_invocations"], control_ps_invocations=0,
            full_samples=row["workload_samples"], control_samples=control["workload_samples"],
            method="identical shaders and geometry; empty scissor; VS/IA equality and zero PS verified"))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?", default=ROOT / ".local/forest-performance")
    parser.add_argument("--compare", type=Path, help="Directory containing a completed baseline summary.json")
    args = parser.parse_args()
    groups = defaultdict(list)
    for path in sorted(args.directory.glob("*.jsonl")):
        for line in path.read_text().splitlines():
            row = json.loads(line)
            key = case_key(row) + (json.dumps(row.get("executable_path")),)
            groups[key].append(row)
    lines = ["# Controlled forest performance audit", "",
             "Static cameras, shadow state shown per case, no readback in measured frames. DX12 shadow diagnostics submit shadow/color as separate renderer frames; use full Editor captures for production FPS. Warmup and measured-frame budgets are recorded per case (current diagnostic: 32 warmup frames, 36 measured or 120 in CPU-profile mode); repeated matching cases are pooled. GPU samples resolve asynchronously; only new valid samples count. Candidate triangles are measured before GPU LOD/culling and cannot quantify selected geometry.", "",
             "| Backend | View | Mode | Impostor m | Width | Shadows | FPS | Frame wall median ms | P95 ms | P99 ms | GPU mean ms | Samples |",
             "|---|---|---|---:|---:|---|---:|---:|---:|---:|---:|---:|"]
    summary = []
    for key, rows in groups.items():
        times = sorted(r["frame_ms"] for r in rows)
        gpu = [r["gpu_ms"] for r in rows if r["gpu_valid"]]
        item = {field: rows[0].get(field) for field in CASE_FIELDS}
        item["executable_path"] = rows[0].get("executable_path")
        item.update(frames=len(rows), frame_median_ms=statistics.median(times),
                    effective_fps=1000 / statistics.mean(times),
                    frame_p95_ms=times[math.ceil(.95*len(times))-1],
                    frame_p99_ms=times[math.ceil(.99*len(times))-1],
                    frames_over_10_ms=sum(value>10 for value in times),
                    phase_mean_ms={field: statistics.mean(row[field] for row in rows if field in row and (not field.startswith("gpu_") or row.get("gpu_valid")))
                                   for field in sorted(set().union(*(row.keys() for row in rows)))
                                   if field.endswith("_ms") and field not in ("frame_ms", "gpu_ms") and (not field.startswith("gpu_") or any(row.get("gpu_valid") for row in rows))},
                    gpu_mean_ms=statistics.mean(gpu) if gpu else None, gpu_samples=len(gpu))
        counts = [row for row in rows if row.get("gpu_valid") and row.get("gpu_workload_valid")]
        item["workload_samples"] = len(counts)
        item["workload_mean"] = {field: statistics.mean(row[field] for row in counts)
                                 for field in rows[0] if field.endswith(("_invocations", "_primitives"))} if counts else {}
        summary.append(item)
        gpu_text = f"{item['gpu_mean_ms']:.3f}" if gpu else "unavailable"
        lines.append("| " + " | ".join(map(str, (item[k] for k in ("backend", "view", "mode", "impostor_distance", "width", "shadows")))) + f" | {item['effective_fps']:.1f} | {item['frame_median_ms']:.3f} | {item['frame_p95_ms']:.3f} | {item['frame_p99_ms']:.3f} | {gpu_text} | {len(gpu)} |")
    lines += ["", "## Opaque categories and executed workload", "",
              "| Backend | View | Mode | Width | Bark ms | Leaves/cards ms | Other opaque ms | VS invocations (trees) | PS invocations (trees) | Workload samples |",
              "|---|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for item in summary:
        phases, counts = item["phase_mean_ms"], item["workload_mean"]
        timing = [f"{phases[key]:.3f}" if key in phases else "unavailable"
                  for key in ("gpu_tree_bark_ms", "gpu_tree_leaves_ms", "gpu_opaque_other_ms")]
        workload = [str(round(sum(counts.get(f"gpu_tree_{part}_{stage}_invocations", 0) for part in ("bark", "leaves"))))
                    if counts else "unavailable" for stage in ("vs", "ps")]
        lines.append("| " + " | ".join(map(str, [item["backend"], item["view"], item["mode"], item["width"],
                                                     *timing, *workload, item["workload_samples"]])) + " |")
    costs = isolate_costs(summary)
    if costs:
        load_manifest(args.directory)
        lines += ["", "## Direct geometry/front-end and fragment-work ablation", "",
                  "Same shader bytecode, draw workload and viewport; empty scissor removes all fragment invocations. VS and IA counts must match exactly. The control includes vertex fetch/shading, primitive/front-end work and query overhead; the residual includes rasterization, fragment shading and depth/color work. These are measured ablations, not additive hardware shader clocks. Negative residuals are retained as measurement noise/overlap, never clamped.", "",
                  "| Backend | Category | Width | Full ms | Vertex/front-end control ms | Incremental raster/fragment ms | Control PS invocations |",
                  "|---|---|---:|---:|---:|---:|---:|"]
        for item in costs:
            lines.append(f"| {item['backend']} | {item['category']} | {item['width']} | {item['full_ms']:.3f} | {item['vertex_front_end_control_ms']:.3f} | {item['incremental_raster_fragment_ms']:.3f} | 0 |")
    (args.directory / "cost-isolation.json").write_text(json.dumps(costs, indent=2)+"\n", encoding="utf-8")
    if args.compare:
        baseline = json.loads((args.compare / "summary.json").read_text())
        current_manifest = load_manifest(args.directory)
        baseline_manifest = load_manifest(args.compare)
        validate_comparison(current_manifest, baseline_manifest, summary + baseline)
        for manifest, captures in ((current_manifest, summary), (baseline_manifest, baseline)):
            for row in captures:
                if row.get("backend") != manifest.get("backend") or Path(row.get("executable_path", "")).resolve() != Path(manifest.get("executable_path", "")).resolve():
                    raise ValueError("Recording backend/executable does not match its capture manifest")
        previous = {case_key(row): row for row in baseline}
        lines += ["", f"Before executable SHA-256: `{baseline_manifest['executable_sha256']}`.",
                  f"After executable SHA-256: `{current_manifest['executable_sha256']}`."]
        lines += ["", "## Matched baseline comparison", "",
                  "Verified asset/shader content, environment, machine and per-case build configuration/route/settings. Executable hashes are recorded separately to identify the before/after builds; separate runs can vary with system load. GPU stage means and frame wall medians describe different quantities.", "",
                  "| Backend | View | Width | Before GPU ms | After GPU ms | Before frame ms | After frame ms |",
                  "|---|---|---:|---:|---:|---:|---:|"]
        matched = 0
        for row in summary:
            old = previous.get(case_key(row))
            if row["mode"] != "full" or row["impostor_distance"] != 90 or not old:
                continue
            gpu_old, gpu_new = old["gpu_mean_ms"], row["gpu_mean_ms"]
            if gpu_old is None or gpu_new is None:
                continue
            matched += 1
            lines.append(f"| {row['backend']} | {row['view']} | {row['width']} | {gpu_old:.3f} | {gpu_new:.3f} | {old['frame_median_ms']:.3f} | {row['frame_median_ms']:.3f} |")
        if not matched:
            raise ValueError("No fully matched full-scene cases with valid GPU samples")
    lines += ["", "Invocation counters describe executed shader workload, not elapsed vertex/fragment time. leaves-unlit retains alpha testing; leaves-solid uses cutoff zero and changes coverage/depth. Neither delta is a pure stage-time measurement. Historical captures without provenance are descriptive only."]
    (args.directory / "summary.json").write_text(json.dumps(summary, indent=2)+"\n", encoding="utf-8")
    (args.directory / "report.md").write_text("\n".join(lines)+"\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
