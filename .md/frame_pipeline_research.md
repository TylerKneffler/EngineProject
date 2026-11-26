# Frame time after the 80 fps work: upscaling, frame generation and the serial main thread

Research and measurements of 2026-11-26, after the landscape demo reached its 80 fps target on the trail walks
(todo.md, "Now: 80 fps while moving"). Question (user): what other options would have a large effect, for
example an FSR implementation or setting? Vendor facts come from web search (press coverage and vendor blog
posts, listed at the end) and are not verified here beyond what the sources say. Engine numbers are measured on
the RTX 4090 at 1920x1080, overlay on, interleaved runs (`.local/routes/perf_runner.py`, `vista_ab.py`).

## 1. Where the frame goes now

Main thread (scene owner), median per frame at 6 m/s:

| Stage | Meadow | Forest |
|---|---|---|
| Simulation update | 1.8 | 1.8 |
| Scene prepare | 1.0-1.1 | 1.0 |
| Camera view record | 3.9-4.1 | 3.8-4.0 |
| Shadow view record | 1.8-1.9 | 1.8-1.9 |
| Retained UI | 0.4 | 0.4 |
| Frame | 10.3-10.6 | 11.9 |

The render thread replays in ~2.2 ms and waits on the main thread (`prepare_handoff_wait_ms` 7.4-7.8). At 23 m/s
the update rises to ~3 ms and prepare to 1.5-1.7 (meadow 12.5, forest 13.2 ms).

Camera view, by phase (`ENGINE_RENDER_PHASE_LOG`): candidates 0.2, per-draw preparation ("draws") 0.8-1.0,
batching 0.4, tile preparation 0.4-0.6, submission 0.9, material records 1.0. Shadow view: setup 0.3,
batching and tiles ~0.6, submission 0.45-0.7.

## 2. Render scale: what the existing upscaler buys

The engine already has temporal upscaling (DX12 game renderer, 2026-11-23): the scene's `renderScale` (Properties)
or `ENGINE_RENDER_SCALE`, resolved by `TemporalResolvePS` / `TemporalUpscale` in PostProcess.hlsl.

| Run | 1.0 | 0.67 | 0.5 |
|---|---|---|---|
| Vista (GPU-bound) | 12.09 | 11.66 | 10.99 |
| Forest walk 6 m/s | 11.73 / 11.78 | 10.34 / 10.27 | - |
| Meadow walk 6 m/s | 10.44 / 10.43 | 9.78 / 9.81 | - |

At Vista, 0.67 scale (44 % fewer pixels) cuts opaque GPU 6.65 -> 5.94 ms only. Clouds halve (0.58 -> 0.28), but the
instance cull (1.2), shadow (1.3), terrain (2.5 -> 2.2) and foliage depth (2.3 -> 2.0) barely move: the GPU is
bound by vertices, instances and overlap, not by pixels. On the walks the main thread is the limit.
So any upscaler, ours or a vendor's, gains ~0.5-1.5 ms here, at 1080p, on this GPU.

## 3. Vendor upscalers and frame generation

AMD FSR (FidelityFX / "FSR SDK"):
- The SDK is open source (MIT) and DX12; FSR 3.1 split the upscaler from frame generation (SDK 1.1).
- SDK 2.2 (March 2026) added FSR 4.1 (ML upscaling), Frame Generation 4.0, Ray Regeneration and Radiance Caching;
  SDK 2.3 brought FSR 4.1 to RDNA 3. A 2.4 release (October 2026) reportedly adds older hardware and Vulkan
  (headline only, unverified).
- The ML upscaler targets Radeon hardware; on the RTX 4090 the analytic FSR 3.1 path is what runs.

NVIDIA DLSS (Streamline SDK):
- DLSS super resolution runs on every RTX GPU; frame generation on RTX 40 and 50; multi-frame generation (and
  the DLSS 4.5 5x/6x modes) on RTX 50 only. On the RTX 4090: upscaling plus single frame generation.
- Integration goes through Streamline (DX12); frame generation also wants Reflex and a single viewport.
- One review measured RTX 40 cards losing up to ~15 % render rate moving from DLSS 4 to 4.5 upscaling presets.

What every one of these needs from the engine (and what it lacks):
1. Per-pixel motion vectors. Our TAA reprojects with depth and the camera only (PostProcess.hlsl), so anything
   that moves on its own (wind on grass, leaves and trees, animated characters, particles) has no motion. A
   vendor upscaler or interpolator fed camera-only motion ghosts exactly the foliage this demo is made of.
2. UI after the upscale. The UI draws into the scene pass (todo: "UI draws into the scene pass"), so it would be
   upscaled and, worse, interpolated by frame generation.
3. Jitter and exposure inputs, reactive/transparency masks for particles, water and alpha-blended foliage.

Expected gains here:
- FSR or DLSS upscaling: the same ~0.5-1.5 ms as our own upscaler (the cost is not pixels), with better edges
  than our 3x3 Gaussian resolve. Worth it for image quality once motion vectors exist, not for speed.
- Frame generation: the one option that raises displayed frames while the CPU is the limit, roughly 95 -> ~170
  displayed on the walks, at the cost of about a frame of latency. Needs items 1 and 2 first.

## 4. Structural options for the serial main thread

Ranked by expected gain on the walks (where the main thread is the limit):

1. **Parallel view recording** (~2-3 ms). Camera and shadow views are recorded one after the other on the scene
   thread (GameApplication `SimulationLoop`: shadow `Render`, then camera `Render`), into separate command
   packets. Blockers: `SceneRenderer::Render` writes ~70 renderer members and shared caches (pipelines,
   material records, the placement pool, upload caches, timings), and the camera reads the shadow view's
   cascade selection. Steps: per-draw work in parallel inside a view first (phase 5 is one ~800-line loop over
   render objects with a handful of shared side effects: pipeline and material-record caches, the terrain
   material state, detail-cost groups, timings), then shadow and camera on separate threads with the cascade
   selection hoisted ahead of both.
2. **Update N+1 overlapping record N** (~1.5-3 ms; more at 23 m/s). Needs the renderer to read a snapshot, not
   the live scene: render items hold live mesh and material pointers that the update mutates (volume chunk
   swaps, layer rebuilds). This is the "Retained render scene" item (engine_notes.md "Next" 3).
3. **GPU-driven submission** (~2 ms of camera record): per-draw preparation, batching and tile preparation
   moved to compute and ExecuteIndirect. Long term.
4. **GPU side**: async compute for the instance cull (1.2-1.5 ms) and clouds beside the shadow pass;
   two-phase HZB occlusion for the foliage depth pre-pass; screen-space-error terrain levels (terrain 2.4 ms).

## 4b. Forest walk GPU (measured after the 80 fps work)

The forest walk is GPU bound (main thread ~8.8 of 11.9 ms). Per stage, with workload counters:

| Stage | ms | Vertices | Primitives | Pixels shaded |
|---|---|---|---|---|
| Terrain | 2.31 | 0.38 M | 0.36 M | 1.02 M |
| Leaves | 1.66 | 4.14 M | 3.32 M | 0.78 M |
| Foliage depth pre-pass | 1.29 | 4.34 M | 3.45 M | 38.5 M |
| Instance cull (compute) | 1.72 | - | - | - |
| Grass | 1.57 | 1.36 M | 0.69 M | 6.47 M |
| Shadow | 0.99 | 0.88 M | 0.73 M | - |
| Bark | 0.75 | 1.00 M | 1.08 M | 0.28 M |

- The depth-pyramid occlusion in the instance cull already saves ~4 ms; switching it off raises the frame
  from 13.1 to 17.1 ms (counters on).
- Terrain is pixel-shader bound: switching off hex tiling (-0.14 ms) or relief (~-0.1 ms) barely helps, and
  PCF is already 3x3. The layered material is the cost. Engines with many terrain layers composite them once
  into a runtime virtual texture (a clipmap of albedo, normal and roughness around the camera, updated as it
  moves) and sample that per pixel.
- Leaves are triangle heavy (~4 triangles per shaded pixel) within the 60/220 m bands; that is a content
  and distance choice rather than a missing feature.

## 5. Recommendation and order

1. Parallel per-draw preparation inside a view, then parallel shadow and camera recording.
2. Retained render scene, then update overlapping record.
3. Motion vectors (object, terrain, grass, foliage with previous-frame wind) and UI after the upscale. They also
   improve our own TAA.
4. Then an upscaler setting (built-in / FSR / DLSS) and optional frame generation, as project graphics
   settings (editor-first rule).

Skip a vendor upscaler as a performance measure until 3 is done; at 1080p on this GPU it would not move the frame.

## 6. References

- AMD FSR SDK 2.3: https://gpuopen.com/learn/amd-fsr-sdk-2-3-blog/
- AMD FSR SDK 2.2: https://gpuopen.com/learn/amd-fsr-sdk-2-2-now-available/
- FSR SDK 2.2 coverage: https://www.tweaktown.com/news/110624/amd-releases-fsr-sdk-v2-2-for-developers-with-fsr-4-1-and-ray-regeneration-1-1/index.html
- FSR SDK 2.4 headline: https://www.gamingonlinux.com/2026/10/amd-fsr-sdk-2-4-is-out-with-fsr-upscaling-4-1-2-for-older-hardware-and-vulkan-support/
- FSR 3.1 in FidelityFX SDK 1.1: https://www.tweaktown.com/news/99267/amd-fsr-3-1-now-available-to-all-game-developers-with-the-release-of-fidelityfx-sdk-v1/index.html
- DLSS 4 with Streamline: https://developer.nvidia.com/blog/how-to-integrate-nvidia-dlss-4-into-your-game-with-nvidia-streamline
- Streamline SDK: https://developer.nvidia.com/rtx/streamline/get-started
- DLSS 4.5 SDK: https://wccftech.com/nvidia-dlss-4-5-sdk-now-available-enabling-devs-to-integrate-dynamic-frame-gen-in-games/
- DLSS 4.5 SDK (6x mode): https://hothardware.com/news/nvidia-dlss-45-sdk-releases-with-dynamic-multi-frame-generation-and-6x-mode-tools
