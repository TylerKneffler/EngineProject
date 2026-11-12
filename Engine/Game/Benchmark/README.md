# Standalone Game capture

Build the standalone C++ capture target from the engine checkout:

```powershell
cmake --build build --config Debug --target GameBackendCapture -- /m:1
.\build\Debug\GameBackendCapture.exe --backend dx11 --width 1920 --height 1080 --warmup 120 --frames 300
```

After building, `ctest --test-dir build -C Debug -R "^GameStandalone_" --output-on-failure`
runs the available backend regressions with clustered lighting on and off. They
require a graphics device and verify geometry, image capture, and camera movement.

The default scene is `Engine/Core/Assets/Scenes/terrain/oak_meadow.scene`; use
`--scene` to select another scene. Use `--project path\to\project.proj` to load a specific project's Game settings,
and `--device "adapter name"` to override its adapter. The target uses the
assets and scripts selected at CMake configuration time, just like Game. Run
`GameBackendCapture --help` for all options. Game's normal frame-rate limit,
real-time simulation, scripts, input, renderer, and worker handoffs apply.

Each run creates a directory under `--out` (default `.local/performance-logs`)
containing `frames.jsonl` and `capture.ppm`. The image is captured in an extra
frame after measurement. The session includes time through scene startup, build, backend,
adapter, resolution, frame limit, render scale, and shader settings. The frame
records include simulation, recording, replay and frame interval timings, scene
and terrain residency, camera draw/triangle/instance counts, foliage LOD data,
and available GPU pass timings. Material and batch timings are nested within
recording; they should not be added to it. `metrics_collection_ms` reports the
scene inspection cost, and JSON writing also adds benchmark overhead.

The summary is valid only when all requested sample frames have a camera,
nonzero CPU-submitted draws and triangles, the requested resolution, and a
successful image. `--require-terrain` and `--require-streamed-cells` add scene
checks. `--wait-for-residency <seconds>` waits for those checks before starting
the warm-up, returning an invalid result if the timeout expires. These checks do
not prove that streaming has settled; choose a warm-up suited to the scene and
inspect residency throughout the recording. CPU-submitted triangle counts can
include indirect candidates, so they are not GPU-rasterized triangle counts.
GPU timing values are null when unavailable, and sample IDs identify repeated
or delayed GPU timing results. Exit codes are 0 for a valid capture, 2 for an
invalid or incomplete capture, and 1 for a setup or runtime error.

For a scene with a `FirstPersonController`, `--drive-forward <frames>` sends a
synthetic focus/click and W hold through the Game input snapshot after warm-up.
Allow at least eight additional measured frames for setup. Camera positions are
recorded per frame. This does not exercise Win32 window focus or physical input
devices. `--require-movement` invalidates the capture if the camera moves less
than 0.05 world units. `--max-fps <fps>` overrides the project cap (zero disables
it), and `--disable-clustered-lighting` selects the non-clustered path for a
comparison; both effective settings are recorded.

DX12 and Vulkan Debug captures have been exercised with rendered geometry and
synthetic first-person input on the NVIDIA RTX A2000 Laptop GPU. Physical input
and focus handling still require an interactive desktop check.
