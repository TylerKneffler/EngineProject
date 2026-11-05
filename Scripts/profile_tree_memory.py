"""Capture matched editor Edit/Play/Stop phases and summarize measured frames."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess


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
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--stop-frames", type=int, default=30)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--summarize-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if not args.summarize_only:
        if not 30 <= args.frames <= 1200 or not 1 <= args.stop_frames <= 1200 or not 0 <= args.warmup <= 1200:
            parser.error("frames must be 30..1200, stop-frames 1..1200, warmup 0..1200")
        env = os.environ.copy()
        env.pop("ENGINE_EDITOR_AUTO_PROFILE_KEEP_OPEN", None)
        env.update({
            "ENGINE_EDITOR_AUTO_PROFILE_SCENE": str(ROOT / "Engine/Core/Assets/Scenes/terrain/oak_meadow.scene"),
            "ENGINE_EDITOR_AUTO_PROFILE_GAME_VIEW": "1",
            "ENGINE_EDITOR_AUTO_PROFILE_FRAMES": str(args.frames),
            "ENGINE_EDITOR_AUTO_PROFILE_WARMUP_FRAMES": str(args.warmup),
            "ENGINE_EDITOR_AUTO_PROFILE_STOP_FRAMES": str(args.stop_frames),
            "ENGINE_EDITOR_FRAME_PROFILE": str(output),
            "ENGINE_EDITOR_PROFILE_LAYOUT": str(output.with_suffix(".ini")),
        })
        # Seed identical layouts for comparable viewport dimensions.
        output.with_suffix(".ini").write_bytes((ROOT / "imgui.ini").read_bytes())
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        with output.with_suffix(".log").open("w") as log:
            subprocess.run([str(args.editor.resolve())], cwd=ROOT, env=env, stdout=log,
                           stderr=subprocess.STDOUT, timeout=args.timeout, check=True,
                           startupinfo=startup)
    result = summarize(output)
    if not args.summarize_only:
        if result["play"]["frames"] != args.frames or result["stopped"]["frames"] != args.stop_frames:
            raise RuntimeError("Capture did not contain the requested measured Play/Stop frames")
        sizes = {(row["game_view_width"]["median"], row["game_view_height"]["median"])
                 for row in result.values() if "game_view_width" in row}
        if sizes != {(1920, 1080)}:
            raise RuntimeError(f"Unexpected benchmark viewport sizes: {sizes}")
    output.with_suffix(".summary.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
