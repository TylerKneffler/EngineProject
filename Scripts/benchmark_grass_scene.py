"""Compare the grass test scene with its layer LODs enabled and disabled.

Runs the headless Server for geometry counts and optionally VideoExporter for
end-to-end DX11 PNG throughput. Times include scene loading and encoding, so
they are not GPU frame times.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
SCENE = ROOT / "Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene"
SERVER = ROOT / "build/Debug/Debug/Server.exe"
EXPORTER = ROOT / "build/Debug/Debug/VideoExporter.exe"
OUTPUT = ROOT / ".local/grass-benchmark"


def run(command: list[str]) -> tuple[float, str]:
    start = time.perf_counter()
    result = subprocess.run(command, cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    seconds = time.perf_counter() - start
    if result.returncode:
        raise RuntimeError(f"{' '.join(command)}\n{result.stderr[-4000:]}")
    return seconds, result.stdout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--render-frames", type=int, default=0,
                        help="Also export this many 960x540 PNG frames per case")
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    source = SCENE.read_text(encoding="utf-8")
    report = {"scene": str(SCENE.relative_to(ROOT)),
              "headlessFrames": args.frames,
              "renderFrames": args.render_frames,
              "cases": {}}
    for case in ("full", "layer_lod"):
        scene_text = source
        if case == "full":
            for field in ("lod1Distance", "lod2Distance"):
                pattern = rf'TerrainObjectLayer\.{field}="[0-9.]+"'
                scene_text, count = re.subn(
                    pattern, f'TerrainObjectLayer.{field}="18"', scene_text,
                    count=1)
                if count != 1:
                    raise RuntimeError(f"Missing {field} in scene")
        scene = OUTPUT / f"{case}.scene"
        scene.write_text(scene_text, encoding="utf-8")
        seconds, output = run([str(SERVER), "--scene", str(scene),
                               "--frames", str(args.frames), "--every",
                               str(args.frames)])
        (OUTPUT / f"{case}.jsonl").write_text(output, encoding="utf-8")
        states = [json.loads(line) for line in output.splitlines()
                  if line.startswith('{') and '"scene_state"' in line]
        tiles = {item["name"]: item for item in states[-1]["objects"]
                 if item["name"].startswith("Grass Tile (")}
        data = {"headlessTotalSeconds": round(seconds, 3),
                "tileCount": len(tiles),
                "tileVertices": sum(tile["mesh"]["vertexCount"]
                                    for tile in tiles.values())}
        if args.render_frames:
            end = 15 + args.render_frames
            render_dir = OUTPUT / f"{case}_render"
            render_seconds, _ = run([
                str(EXPORTER), "--scene", str(scene), "--output",
                str(render_dir), "--format", "png", "--width", "960",
                "--height", "540", "--fps", "30", "--duration", "1",
                "--frame-range", f"15:{end}", "--timeout", "180"])
            data["dx11PngTotalSeconds"] = round(render_seconds, 3)
            data["dx11PngSecondsPerSelectedFrame"] = round(
                render_seconds / args.render_frames, 3)
        report["cases"][case] = data
        print(case, json.dumps(data), flush=True)
        (OUTPUT / "report.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
