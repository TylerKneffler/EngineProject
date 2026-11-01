# Landscape performance implementation and evaluation

Implemented the landscape performance feature list in `.md/todo.md`. Grass camera and shadow draws now merge compatible tiles, with tile transforms, materials, and fades preserved. Audio, lighting, camera warp checks, and spatial sampling use component registries. Large immutable preparation workloads can use persistent workers, and instance staging/buffers and visibility results are reused.

Cascade receiver silhouettes conservatively reject irrelevant shadow casters, including support for off-screen casters and cascade transitions. Shadow quality presets include a grass caster distance control in project preferences. The default project distance remains unchanged until explicitly edited or a preset is applied.

## Controlled workload comparison

Debug DX11 helper, 1848 x 1069, the same 441-cell stress landscape, camera route, lighting, four cascades, and 2048 shadow atlas. Every mode warmed the moving route for at least 300 frames before recording 240 frames. No mode built new grass tiles during measurement. The empty/default baseline hides mesh geometry in the same scene.

| Mode | Helper frame average ms | CPU shadow pass ms | GPU measured stages ms | Main mesh draws | Shadow draws |
|---|---:|---:|---:|---:|---:|
| default | 3.825 | 0.383 | 0.000 | 0.0 | 0.0 |
| terrain-only | 13.912 | 0.976 | 2.206 | 1.0 | 0.0 |
| grass-no-shadows | 20.610 | 1.347 | 3.721 | 7.0 | 0.0 |
| full-grass | 22.662 | 3.226 | 12.937 | 9.0 | 14.6 |

This helper includes grass updates and render preparation/submission. It excludes editor UI, audio coordination, and full gameplay/physics updates, so these numbers are not editor FPS. CPU wall time and asynchronous GPU timings overlap and must not be added. GPU samples are deduplicated by sample ID.

The warmed full-grass run averaged about 9 main mesh draws and 15 shadow draws. Historical helpers submitted about 72 and 111 respectively, but their warmup and route phase differed; those captures do not establish a controlled before/after FPS percentage. The first short-warmup matrix is retained in files ending `-initial.jsonl`; its mode timings were inconsistent and it is not used for feature-cost estimates.

The warmed matrix adds approximately 6.70 ms from terrain-only to grass without shadows, then 2.05 ms with grass shadows enabled. These are observed differences for this run, not universal feature costs. Full grass still spends substantial time in material/visibility preparation, terrain occlusion preparation, grass management, and GPU shadow rendering. A fresh editor recording is needed to measure the complete application after this update.

## Diagnostics and validation

Final recordings expose shadow submission wall time, active thread cycles, and slowest batch time. The evaluator flags long submissions with unusually low cycle density as potential scheduling/wait candidates. CPU frequency can also change density. Per-pass `GetThreadTimes` was rejected after observing 15.625 ms quantization; the matrix captures from before that diagnostic correction retain those coarse experimental fields, which must not be interpreted as precise per-pass CPU or wait durations.

Editor and regression helpers build successfully. Regression checks cover nested audio/light/warp registry add, ancestor-disable, and removal behavior; conservative off-screen shadow retention; worker execution exactly once; terrain collider streaming; recording evaluation; and existing animation/particle behavior. Pixel comparisons cover per-tile fades/transforms/species and ordinary direct/GPU instancing on DX11, DX12, and Vulkan.

Raw recordings and per-mode evaluations are in `.local/performance-logs/landscape-matrix-*`; the combined evaluation is `.local/performance-logs/landscape-matrix-comparison.json`.
