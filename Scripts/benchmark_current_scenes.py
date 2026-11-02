"""Run clean DX11 grass and terrain scene comparisons on the current build."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXECUTABLES = ROOT / "build" / "Debug" / "Debug"


def evaluate(path):
    spec = importlib.util.spec_from_file_location(
        "evaluate_performance", ROOT / "Scripts" / "evaluate_performance.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    result = module.load(path)
    metrics = result["metrics"]
    gpu = result["gpu_stages_ms"]
    def mean(mapping, key):
        value = mapping.get(key)
        return round(value["mean"], 3) if value else None
    return {
        "frames": result["frames"],
        "frame_mean_ms": round(result["interval_ms"]["mean"], 3),
        "frame_p95_ms": round(result["interval_ms"]["p95"], 3),
        "update_mean_ms": mean(metrics, "update_ms"),
        "render_cpu_mean_ms": mean(metrics, "render_cpu_total_ms"),
        "worker_active_mean_ms": mean(metrics, "render_worker_active_ms"),
        "worker_wait_mean_ms": mean(metrics, "render_worker_wait_ms"),
        "gpu_grass_mean_ms": mean(gpu, "gpu_grass_ms"),
        "gpu_terrain_mean_ms": mean(gpu, "gpu_terrain_ms"),
        "gpu_shadow_mean_ms": mean(gpu, "gpu_shadow_ms"),
        "gpu_samples": result["unique_gpu_samples"],
        "native_timed_frames": result["native_timed_frames"],
        "native_calls": result["native_calls"],
        "native_stall_frames_5ms": result["native_stall_frames_5ms"],
    }


def run(command, output, env):
    with output.open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        raise RuntimeError(f"{Path(command[0]).name} failed; see {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--reverse", action="store_true",
                        help="Run parallel before serial to check order effects")
    parser.add_argument("--native-call-timings", action="store_true",
                        help="Record DX11 native call counts, wall time, and maxima")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if not output.is_relative_to(ROOT / ".local" / "performance-logs"):
        parser.error("Output must be inside .local/performance-logs")
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.pop("ENGINE_WORKER_COUNT", None)
    if args.native_call_timings:
        env["ENGINE_NATIVE_CALL_TIMINGS"] = "1"
    grass = EXECUTABLES / "GrassPerformanceTests.exe"
    terrain = EXECUTABLES / "TerrainPerformanceSceneTests.exe"
    for executable in (grass, terrain):
        if not executable.is_file():
            raise FileNotFoundError(executable)

    grass_mode = ("--current-scene-comparison-reverse" if args.reverse
                  else "--current-scene-comparison")
    run([str(grass), grass_mode, str(output / "grass-")],
        output / "grass-stdout.txt", env)
    results = {}
    for route in ("resident", "streaming"):
        for mode in ("serial", "parallel"):
            name = f"grass-{route}-{mode}"
            results[name] = evaluate(output / f"{name}.jsonl")

    for mode in (("parallel", "serial") if args.reverse else ("serial", "parallel")):
        current_env = env.copy()
        current_env["TERRAIN_PERFORMANCE_BENCHMARK"] = "1"
        current_env["TERRAIN_CURRENT_BENCHMARK"] = str(output / f"terrain-{mode}.jsonl")
        if mode == "parallel":
            current_env["TERRAIN_PARALLEL_PREPARATION"] = "1"
        else:
            current_env.pop("TERRAIN_PARALLEL_PREPARATION", None)
        run([str(terrain)], output / f"terrain-{mode}-stdout.txt", current_env)
        results[f"terrain-{mode}"] = evaluate(output / f"terrain-{mode}.jsonl")

    (output / "summary.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    rows = ["# Current scene performance comparison", "",
            "DX11 Debug. The serial label means parallel-for loops are disabled; "
            "background jobs and owner-thread phases remain active in both modes. "
            "Modes share the current scene files and view sizes.",
            "Grass covers update and rendering without editor UI or physics. "
            "Terrain covers scene update and rendering without editor UI.",
            "",
            "| Route | Mode | Mean frame ms | P95 frame ms | Render CPU ms | Grass GPU ms | Terrain GPU ms |",
            "|---|---|---:|---:|---:|---:|---:|"]
    for name, value in results.items():
        route, mode = name.rsplit("-", 1)
        def show(key):
            number = value[key]
            return f"{number:.2f}" if number is not None else "n/a"
        rows.append(f"| {route} | {mode} | {show('frame_mean_ms')} | "
                    f"{show('frame_p95_ms')} | {show('render_cpu_mean_ms')} | "
                    f"{show('gpu_grass_mean_ms')} | {show('gpu_terrain_mean_ms')} |")
    rows += ["", "GPU stages resolve asynchronously and can overlap CPU work. "
             "Single-run mode differences are indicative; compare GPU stage changes "
             "before attributing a frame change to parallel work."]
    if any(result.get("native_calls") for result in results.values()):
        rows += ["", "## Native call maxima", "",
                 "Wall time includes timing overhead. Maxima are individual calls across measured frames.", "",
                 "| Route | Operation | Maximum call ms | Frames with call ≥5 ms | Mean calls/frame |",
                 "|---|---|---:|---:|---:|"]
        for route, result in results.items():
            for operation, value in sorted(result.get("native_calls", {}).items()):
                if value["calls"]["max"]:
                    rows.append(f"| {route} | {operation} | "
                                f"{value['maximum_call_ms']['max']:.3f} | "
                                f"{result['native_stall_frames_5ms'].get(operation, 0)} | "
                                f"{value['calls']['mean']:.1f} |")
    (output / "summary.md").write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(output / "summary.md")


if __name__ == "__main__":
    main()
