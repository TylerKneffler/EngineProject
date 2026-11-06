"""Summarize controlled forest diagnostics; GPU samples are deduplicated by the recorder."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?", default=ROOT / ".local/forest-performance")
    parser.add_argument("--compare", type=Path, help="Directory containing a completed baseline summary.json")
    args = parser.parse_args()
    groups = defaultdict(list)
    for path in sorted(args.directory.glob("*.jsonl")):
        for line in path.read_text().splitlines():
            row = json.loads(line)
            row.setdefault("shadows", False)
            key = tuple(row[k] for k in ("backend", "view", "mode", "impostor_distance", "width", "shadows"))
            groups[key].append(row)
    lines = ["# Controlled forest performance audit", "",
             "Static cameras, shadow state shown per case, no readback in measured frames. DX12 shadow diagnostics submit shadow/color as separate renderer frames; use full Editor captures for production FPS. Each case warms for 12 frames after texture warmup, then measures 36 frames (120 in CPU-profile mode); repeated matching cases are pooled. GPU samples resolve asynchronously; only new valid samples count. Candidate triangles are measured before GPU LOD/culling and cannot quantify selected geometry.", "",
             "| Backend | View | Mode | Impostor m | Width | Shadows | FPS | Frame wall median ms | P95 ms | P99 ms | GPU mean ms | Samples |",
             "|---|---|---|---:|---:|---|---:|---:|---:|---:|---:|---:|"]
    summary = []
    for key, rows in groups.items():
        times = sorted(r["frame_ms"] for r in rows)
        gpu = [r["gpu_ms"] for r in rows if r["gpu_valid"]]
        item = dict(zip(("backend", "view", "mode", "impostor_distance", "width", "shadows"), key))
        item.update(frames=len(rows), frame_median_ms=statistics.median(times),
                    effective_fps=1000 / statistics.mean(times),
                    frame_p95_ms=times[math.ceil(.95*len(times))-1],
                    frame_p99_ms=times[math.ceil(.99*len(times))-1],
                    frames_over_10_ms=sum(value>10 for value in times),
                    phase_mean_ms={field: statistics.mean(row[field] for row in rows if field in row)
                                   for field in sorted(set().union(*(row.keys() for row in rows)))
                                   if field.endswith("_ms") and field not in ("frame_ms", "gpu_ms")},
                    gpu_mean_ms=statistics.mean(gpu) if gpu else None, gpu_samples=len(gpu))
        summary.append(item)
        gpu_text = f"{item['gpu_mean_ms']:.3f}" if gpu else "unavailable"
        lines.append("| " + " | ".join(map(str, key)) + f" | {item['effective_fps']:.1f} | {item['frame_median_ms']:.3f} | {item['frame_p95_ms']:.3f} | {item['frame_p99_ms']:.3f} | {gpu_text} | {len(gpu)} |")
    if args.compare:
        baseline = json.loads((args.compare / "summary.json").read_text())
        for row in baseline:
            row.setdefault("shadows", False)
        fields = ("backend", "view", "mode", "impostor_distance", "width", "shadows")
        previous = {tuple(row[k] for k in fields): row for row in baseline}
        lines += ["", "## Matched baseline comparison", "",
                  "Same diagnostic scene/settings, static views and warmup; separate runs can vary with system load. GPU stage means and frame wall medians describe different quantities.", "",
                  "| Backend | View | Width | Before GPU ms | After GPU ms | Before frame ms | After frame ms |",
                  "|---|---|---:|---:|---:|---:|---:|"]
        for row in summary:
            old = previous.get(tuple(row[k] for k in fields))
            if row["mode"] != "full" or row["impostor_distance"] != 90 or not old:
                continue
            gpu_old, gpu_new = old["gpu_mean_ms"], row["gpu_mean_ms"]
            if gpu_old is None or gpu_new is None:
                continue
            lines.append(f"| {row['backend']} | {row['view']} | {row['width']} | {gpu_old:.3f} | {gpu_new:.3f} | {old['frame_median_ms']:.3f} | {row['frame_median_ms']:.3f} |")
    (args.directory / "summary.json").write_text(json.dumps(summary, indent=2)+"\n", encoding="utf-8")
    (args.directory / "report.md").write_text("\n".join(lines)+"\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
