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

Terrain cell totals include native terrain cells and streamed prefab cells.
`terrain_prefab_cells_loaded` and `terrain_prefab_cells_pooled` report the prefab
subset separately; an authored center tile appears in `terrain_components`.

Captures enable preparation profiling and include `prepare_*` stage timings and
static instance reuse counters. These are nested in `scene_prepare_ms`; do not
add them to the frame interval. `prefab_layers_pending` includes source refresh
work as well as initial prefab loading. Check that object, draw and geometry
counts have settled instead of interpreting a transient source refresh as a
missing forest.

`shared_instance_vertices`, `shared_instance_indices`, and
`shared_instance_geometry_bytes` count each resident instance source once, across
all cells and LODs. Bytes describe logical vertex/index data, not total GPU/CPU
memory (buffer caches and upload mirrors can retain additional copies).

`foliage_gpu_selected_lod0/1/2` and `foliage_gpu_transition_lod0/1/2` report
completed camera-pass culling readbacks. Check `foliage_gpu_readback_samples`
before interpreting zero counts. These are asynchronous mesh-part instances,
not unique trees or rasterized triangles; a tree can have bark and leaf parts
and can contribute to two LODs during a transition.

CPU recording diagnostics separate the last camera render (`render_cpu_ms`)
from the shadow render (`shadow_cpu_ms`). Camera fields include startup,
culling, sorting, occlusion, material assembly, batching, upload, submission,
and remaining work. `render_instance_tile_prepare_ms` is part of remaining
work; record packing runs on workers and can overlap batching. Do not add
these nested/overlapping fields to obtain frame time. Shadow preparation,
cascade culling, record upload, and submission are reported separately.
`command_record_ms` measures the enclosing command-recording operation.

Opaque GPU diagnostics also expose bark, leaves, and foliage coverage-depth
timings separately. Set `ENGINE_GPU_WORKLOAD_COUNTERS=1` to collect vertex,
primitive, and pixel-shader invocation counts; check `gpu_workload_valid` first.
`gpu_opaque_ms` includes the coverage-depth pass, so moving work into that pass
cannot appear as a false opaque-pass improvement. `foliage_depth_draws` counts
its extra draws; submitted instances/triangles include both passes, while the
GPU visibility and LOD readbacks still describe one selection per view.
The foliage depth path is enabled by default for supported instanced masked
materials. Set `ENGINE_FOLIAGE_DEPTH_PREPASS=0` for a same-executable control
capture; setting it to `1` enables it explicitly. Low-overdraw scenes can pay
extra geometry/submission cost, so compare total frame and GPU time, not only
the lighting-stage time.

For a Release large-map measurement using the existing multi-configuration build:

```powershell
cmake --build build/Debug --config Release --target GameBackendCapture --parallel 4
.\build\Debug\Release\GameBackendCapture.exe --backend dx12 --scene Engine/Core/Assets/Scenes/terrain/oak_meadow_expanded.scene --width 1920 --height 1080 --warmup 1200 --frames 240 --max-fps 0 --require-terrain --require-streamed-cells --wait-for-residency 120 --out .local/oak-release
```

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
