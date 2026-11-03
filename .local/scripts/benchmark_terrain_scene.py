"""Benchmark the authored terrain performance scene on the DX11 render helper."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys

sys.dont_write_bytecode = True
from evaluate_performance import load


ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--config", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--sizes", nargs="+", default=("960x540", "1920x1080"))
    parser.add_argument("--shadows", choices=("on", "off"), default="on")
    args = parser.parse_args()
    executable = ROOT / "build" / args.config / args.config / "TerrainPerformanceSceneTests.exe"
    if not executable.is_file():
        parser.error(f"build TerrainPerformanceSceneTests first: {executable}")
    output = args.output_dir.resolve()
    if not output.is_relative_to(ROOT / ".local" / "performance-logs"):
        parser.error("output must be inside .local/performance-logs")
    sizes = []
    for size in args.sizes:
        try:
            width, height = (int(part) for part in size.lower().split("x"))
        except (ValueError, TypeError):
            parser.error(f"invalid size {size!r}; expected WIDTHxHEIGHT")
        if not 0 < width <= 8192 or not 0 < height <= 8192:
            parser.error(f"invalid size {size!r}; dimensions must be 1..8192")
        sizes.append((width, height))
    output.mkdir(parents=True, exist_ok=False)
    summary = {"scene": "Engine/Core/Assets/Scenes/terrain/terrain_performance.scene",
               "renderer": "DX11", "config": args.config,
               "scope": "update_and_render_no_editor", "shadows": args.shadows,
               "cases": {}}
    for width, height in sizes:
        name = f"{width}x{height}"
        trace = output / f"terrain-{name}.jsonl"
        environment = os.environ.copy()
        environment["TERRAIN_PERFORMANCE_BENCHMARK"] = "1"
        environment["TERRAIN_CURRENT_BENCHMARK"] = str(trace)
        environment["TERRAIN_PARALLEL_PREPARATION"] = "1"
        environment["TERRAIN_BENCHMARK_WIDTH"] = str(width)
        environment["TERRAIN_BENCHMARK_HEIGHT"] = str(height)
        if args.shadows == "off":
            environment["TERRAIN_BENCHMARK_NO_SHADOWS"] = "1"
        else:
            environment.pop("TERRAIN_BENCHMARK_NO_SHADOWS", None)
        with (output / f"terrain-{name}-stdout.txt").open("w", encoding="utf-8") as log:
            result = subprocess.run([str(executable)], cwd=ROOT, env=environment,
                                    stdout=log, stderr=subprocess.STDOUT, check=False)
        if result.returncode:
            raise RuntimeError(f"{name} failed; see {output / f'terrain-{name}-stdout.txt'}")
        evaluated = load(trace)
        frames = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()
                  if line.startswith("{")]
        frames = [row for row in frames if row.get("type") == "frame_details"
                  and row.get("measured")]
        case = {"frames": evaluated["frames"],
                "frame_p50_ms": statistics.median(row["frame_ms"] for row in frames),
                "frame_interval_ms": evaluated["interval_ms"],
                "gpu_stages_ms": evaluated["gpu_stages_ms"],
                "gpu_samples": evaluated["unique_gpu_samples"],
                "update_ms": evaluated["metrics"].get("update_ms"),
                "render_cpu_ms": evaluated["metrics"].get("render_cpu_total_ms"),
                "peak_loaded_cells": max(row.get("terrain_loaded_cells", 0) for row in frames),
                "peak_far_tiles": max(row.get("terrain_far_tiles", 0) for row in frames),
                "shadow_cascades": frames[0]["shadow_cascades"],
                "shadow_atlas_resolution": frames[0]["shadow_atlas_resolution"]}
        summary["cases"][name] = case
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                               encoding="utf-8")
        print(name, json.dumps(case), flush=True)


if __name__ == "__main__":
    main()
