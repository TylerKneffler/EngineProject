"""Capture matched editor Edit/Play/Stop phases and summarize measured frames."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
from performance_inputs import compare_inputs, snapshot_inputs
from editor_performance_provenance import (build_identity, machine_identity, workload_identity,
                                         compare_captures, content_identity, identity_digest, sha256)


ROOT = Path(__file__).resolve().parents[1]


def summarize(path):
    phases = {name: [] for name in ("edit", "play_warmup", "play", "stopped")}
    seen_play = False
    complete = False
    for line in path.read_text().splitlines():
        row = json.loads(line)
        if row.get("type") == "capture_complete":
            complete = True
        if "frame" not in row or "playing" not in row:
            continue
        if row["playing"]:
            seen_play = True
            phase = "play" if row.get("measured") else "play_warmup"
        else:
            phase = "stopped" if seen_play else "edit"
        phases[phase].append(row)
    if not complete or any(not phases[name] for name in ("edit", "play", "stopped")):
        raise RuntimeError(f"Incomplete Edit/Play/Stop capture: {path}")
    result = {}
    for phase, rows in phases.items():
        entry = {"frames": len(rows)}
        for key in ("frame_ms", "editor_update_interval_ms", "prepare_ms",
                    "render_batching_ms", "physics_ms", "render_physical_draws",
                    "shadow_draws", "rendered_view_pixels", "game_view_width",
                    "game_view_height", "editor_working_set_bytes", "editor_private_bytes",
                    "editor_peak_working_set_bytes", "play_snapshot_capacity_bytes",
                    "scene_history_capacity_bytes"):
            values = [row[key] for row in rows if key in row]
            if values:
                entry[key] = {"median": statistics.median(values), "max": max(values),
                              "last": values[-1]}
        result[phase] = entry
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--editor", type=Path, default=ROOT / "build/Release/Release/Editor.exe")
    parser.add_argument("--renderer", choices=("dx11", "dx12", "vulkan"),
                        help="Explicit editor backend; otherwise use project settings")
    parser.add_argument("--scene", type=Path, default=ROOT / "Engine/Core/Assets/Scenes/terrain/oak_meadow.scene")
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--stop-frames", type=int, default=30)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--summarize-only", action="store_true")
    parser.add_argument("--layout-seed", type=Path,
                        help="Stable layout input for matched runs; defaults to the workspace imgui.ini")
    parser.add_argument("--input", type=Path, action="append", default=[],
                        help="Additional file to fingerprint, e.g. a route or settings file")
    parser.add_argument("--fixed-step", type=float, help="Deterministic simulation seconds per Play frame (e.g. 0.0166666667)")
    parser.add_argument("--compare", type=Path, help="Provenance-verified editor recording to compare")
    args = parser.parse_args()
    if args.fixed_step is not None and not 0 < args.fixed_step <= .1:
        parser.error("fixed-step must be greater than zero and at most 0.1 seconds")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    input_comparison = None
    if not args.summarize_only:
        if not 30 <= args.frames <= 1200 or not 1 <= args.stop_frames <= 1200 or not 0 <= args.warmup <= 1200:
            parser.error("frames must be 30..1200, stop-frames 1..1200, warmup 0..1200")
        if output.exists() or output.with_suffix(".inputs.json").exists():
            parser.error("Use a new recording path so capture identities cannot be mixed")
        env = os.environ.copy()
        env.pop("ENGINE_EDITOR_AUTO_PROFILE_STEP_SECONDS", None)
        if args.fixed_step is not None:
            env["ENGINE_EDITOR_AUTO_PROFILE_STEP_SECONDS"] = str(args.fixed_step)
        env.pop("ENGINE_EDITOR_AUTO_PROFILE_KEEP_OPEN", None)
        env.update({
            "ENGINE_EDITOR_AUTO_PROFILE_SCENE": str(args.scene.resolve()),
            "ENGINE_EDITOR_AUTO_PROFILE_GAME_VIEW": "1",
            "ENGINE_EDITOR_AUTO_PROFILE_FRAMES": str(args.frames),
            "ENGINE_EDITOR_AUTO_PROFILE_WARMUP_FRAMES": str(args.warmup),
            "ENGINE_EDITOR_AUTO_PROFILE_STOP_FRAMES": str(args.stop_frames),
            "ENGINE_EDITOR_FRAME_PROFILE": str(output),
            "ENGINE_EDITOR_PROFILE_LAYOUT": str(output.with_suffix(".ini")),
        })
        # Keep the exact initial layout: the Editor may rewrite its working copy.
        layout_seed = (args.layout_seed or ROOT / "imgui.ini").resolve()
        layout_working = output.with_suffix(".ini")
        layout_snapshot = output.with_suffix(".layout-seed.ini")
        if layout_seed in (layout_working.resolve(), layout_snapshot.resolve()):
            parser.error("layout seed must differ from the capture's working and snapshot layout paths")
        layout_bytes = layout_seed.read_bytes()
        layout_snapshot.write_bytes(layout_bytes)
        layout_working.write_bytes(layout_bytes)
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [str(args.editor.resolve())]
        if args.renderer:
            command += ["--editor-renderer", args.renderer]
        extra_inputs = [args.editor.resolve(), layout_seed,
                        *(path.resolve() for path in args.input)]
        manifest = dict(version=2, scope="route/scene/prefabs, all bundled assets/shaders, build/libraries, driver, layout, settings and trajectory",
                        command=command, requested_frames=args.frames, warmup=args.warmup,
                        explicit_inputs=[str(path.resolve()) for path in args.input],
                        before=snapshot_inputs(args.scene.resolve(), extra_inputs, ROOT))
        manifest['layout_seed'] = dict(source=str(layout_seed), snapshot=str(layout_snapshot),
                                     sha256=hashlib.sha256(layout_bytes).hexdigest())
        if manifest['before']['files'].get(str(layout_seed), {}).get('sha256') != manifest['layout_seed']['sha256']:
            raise RuntimeError("Layout seed changed while preparing capture")
        print("Fingerprinting route, assets, shaders, build and driver...", flush=True)
        assets = content_identity()
        build = build_identity(args.editor)
        excluded = {"ENGINE_EDITOR_FRAME_PROFILE", "ENGINE_EDITOR_PROFILE_LAYOUT"}
        comparison_environment = {key: value for key, value in env.items()
                                 if key.startswith(("ENGINE_", "VK_", "DX", "D3D")) and
                                 not key.startswith("ENGINE_EDITOR_AUTO_PROFILE_") and key not in excluded}
        manifest["provenance"] = dict(schema=1, build=build, platform=machine_identity(),
            assets_sha256=identity_digest(assets),
            explicit_inputs_sha256=identity_digest({str(path.resolve()): sha256(path.resolve()) for path in args.input}),
            route_path=str(args.scene.resolve()),
            route_sha256=sha256(args.scene.resolve()), environment=comparison_environment,
            assets_unchanged=False, build_unchanged=False)
        manifest_path = output.with_suffix(".inputs.json")
        manifest_path.write_text(json.dumps(manifest, indent=2))
        try:
            with output.with_suffix(".log").open("w") as log:
                subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                               stderr=subprocess.STDOUT, timeout=args.timeout, check=True,
                               startupinfo=startup)
            manifest["provenance"]["workload"] = workload_identity(output, args.fixed_step, args.renderer)
            manifest["provenance"]["recording_sha256"] = sha256(output)
            manifest["capture_succeeded"] = True
        finally:
            manifest.setdefault("capture_succeeded", False)
            manifest["after"] = snapshot_inputs(args.scene.resolve(), extra_inputs, ROOT)
            input_comparison = compare_inputs(manifest["before"], manifest["after"])
            manifest["comparison"] = input_comparison
            manifest["provenance"]["assets_unchanged"] = assets == content_identity()
            manifest["provenance"]["build_unchanged"] = build == build_identity(args.editor)
            manifest_path.write_text(json.dumps(manifest, indent=2))
    result = summarize(output)
    if not args.summarize_only:
        if result["play"]["frames"] != args.frames or result["stopped"]["frames"] != args.stop_frames:
            raise RuntimeError("Capture did not contain the requested measured Play/Stop frames")
        sizes = {(row["game_view_width"]["median"], row["game_view_height"]["median"])
                 for row in result.values() if "game_view_width" in row}
        if sizes != {(1920, 1080)}:
            raise RuntimeError(f"Unexpected benchmark viewport sizes: {sizes}")
    if args.compare:
        result["matched_comparison"] = compare_captures(output, args.compare)
    if not args.summarize_only and (not manifest["provenance"]["assets_unchanged"] or not manifest["provenance"]["build_unchanged"]):
        raise RuntimeError(f"Capture asset/shader/build inputs changed; see {manifest_path}")
    output.with_suffix(".summary.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    if input_comparison is not None and not input_comparison["unchanged"]:
        raise RuntimeError(f"Capture inputs changed or could not be fingerprinted; see {manifest_path}")


if __name__ == "__main__":
    main()
