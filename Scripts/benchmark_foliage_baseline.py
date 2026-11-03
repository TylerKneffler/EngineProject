"""Capture matched DX11 landscape render baselines with foliage off and on.

The helper includes grass updates and rendering, but no editor UI or physics.
Each case runs in a fresh process and writes its full per-frame JSONL trace.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from evaluate_performance import load


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--config", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--route", choices=("moving", "fixed"), default="moving")
    parser.add_argument("--scene", choices=("stress", "single", "single-distant", "default"),
                        default="stress")
    parser.add_argument("--modes", nargs="+", choices=("off", "shadows-off", "on"),
                        default=("off", "shadows-off", "on"))
    parser.add_argument("--reuse", action="store_true",
                        help="Summarize existing traces without running the renderer")
    args = parser.parse_args()
    if args.width <= 0 or args.height <= 0:
        parser.error("width and height must be positive")
    if args.scene == "default" and (list(args.modes) != ["off"] or args.route != "moving"):
        parser.error("the default scene supports --modes off and the default route")
    executable = ROOT / "build" / args.config / args.config / "GrassPerformanceTests.exe"
    if not executable.is_file():
        parser.error(f"build GrassPerformanceTests first: {executable}")
    output = args.output_dir.resolve()
    if not output.is_relative_to(ROOT / ".local" / "performance-logs"):
        parser.error("output must be inside .local/performance-logs")
    output.mkdir(parents=True, exist_ok=args.reuse)
    scenes = {"stress": "grass_flat_test.scene",
              "single": "grass_single_cell_test.scene",
              "single-distant": "grass_single_cell_distant_test.scene"}
    scene = ("Engine/Core/Assets/Scenes/Basics/default.scene" if args.scene == "default"
             else f"Engine/Core/Assets/Scenes/terrain/{scenes[args.scene]}")
    summary = {"config": args.config, "width": args.width, "height": args.height,
               "scene": scene,
               "renderer": "DX11", "route": args.route, "cases": {}}
    for mode in args.modes:
        trace = output / f"foliage-{mode}.jsonl"
        if not args.reuse:
            command = ([str(executable), "--empty-default-baseline", str(trace),
                        str(args.width), str(args.height)] if args.scene == "default" else
                       [str(executable), "--foliage-baseline-fixed" if args.route == "fixed"
                        else "--foliage-baseline", mode, str(trace),
                        str(args.width), str(args.height), scene])
            with (output / f"foliage-{mode}-stdout.txt").open("w", encoding="utf-8") as log:
                result = subprocess.run(command, cwd=ROOT, stdout=log,
                                        stderr=subprocess.STDOUT, check=False)
            if result.returncode:
                raise RuntimeError(f"{mode} failed; see {output / f'foliage-{mode}-stdout.txt'}")
        evaluated = load(trace)
        frames = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()
                  if line.startswith("{")]
        frames = [row for row in frames if row.get("type") == "frame_details"
                  and row.get("measured")]
        if len(frames) != 240:
            raise RuntimeError(f"{mode} has {len(frames)} measured frames, expected 240: {trace}")
        summary["cases"][mode] = {
            "frames": evaluated["frames"],
            "frame_interval_ms": evaluated["interval_ms"],
            "gpu_stages_ms": evaluated["gpu_stages_ms"],
            "gpu_samples": evaluated["unique_gpu_samples"],
            "grass_update_ms": evaluated["metrics"].get("grass_ms"),
            "render_cpu_ms": evaluated["metrics"].get("render_cpu_total_ms"),
            "peak_grass_instances": max((row.get("grass_instances", 0) for row in frames), default=0),
            "tiles_built": sum(row.get("grass_built", 0) for row in frames),
            "peak_placement_upload_bytes": max((row.get("grass_instance_upload_bytes", 0)
                                                 for row in frames), default=0),
            "peak_instance_resident_bytes": max((row.get("instance_resident_bytes", 0)
                                                 for row in frames), default=0),
            "peak_terrain_components": max((row.get("terrain_component_count", 0)
                                            for row in frames), default=0),
            "peak_render_draws": max((row.get("render_draws", 0)
                                     for row in frames), default=0),
        }
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                               encoding="utf-8")
        print(mode, json.dumps(summary["cases"][mode]), flush=True)


if __name__ == "__main__":
    main()
