# Serial and parallel validation, 2026-11-03

Debug build on a 16-logical-CPU Windows host. `Scripts/benchmark_current_scenes.py` captured 240 measured frames per mode after warmup, with serial then parallel order. Serial disables the engine's parallel-for work; background jobs remain active. Grass uses the authored 441-cell scene at 1848 x 1069 without editor UI or physics. Terrain uses the authored moving route at 960 x 540 without editor UI. The terrain pair was recaptured after adding route-version and shadow-setting fields to its JSONL records.

The serial and parallel files in each pair agree on renderer, benchmark scope, route, route version, view size, cascade count, and shadow atlas resolution. The 240 recorded grass camera positions match exactly in each pair; the terrain fixture uses the same frame-indexed camera formula in both modes. Grass uses four cascades and a 2048 atlas; terrain uses four cascades and a 1024 atlas. All six captures contain 240 measured frames and zero dropped records. Frame intervals below are wall time, not the sum of overlapping CPU and GPU stages.

| Route | Mode | Mean ms | P95 ms | P99 ms | Mean GPU grass ms | Mean GPU shadow ms |
|---|---|---:|---:|---:|---:|---:|
| Grass resident | Serial | 91.46 | 177.86 | 220.20 | 64.46 | 10.21 |
| Grass resident | Parallel | 88.34 | 146.88 | 183.96 | 62.89 | 9.76 |
| Grass streaming | Serial | 84.45 | 112.33 | 136.67 | 54.22 | 10.49 |
| Grass streaming | Parallel | 85.43 | 119.43 | 157.17 | 55.64 | 10.57 |
| Terrain moving | Serial | 15.19 | 17.91 | 117.58 | n/a | 6.96 |
| Terrain moving | Parallel | 14.44 | 21.73 | 119.85 | n/a | 6.42 |

Resident grass averaged 3,373.6 and 3,373.8 visible instances. Streaming grass averaged 2,890.2 and 2,906.6 instances and built 743 and 747 tiles, respectively. Terrain averaged 9.01 loaded cells and 4.39 far tiles in both modes. GPU stages and, for streaming grass, instance counts still differ. These Debug captures show no reliable end-to-end speedup: the resident mean and P95 improved in this order, streaming regressed slightly, and terrain's mean improved while P95 increased. The [previous two capture orders](current-scene-performance-20261102.md) also varied. GPU stage times resolve asynchronously and overlap CPU work.

## Worker scaling

`GrassPerformanceTests --transform-benchmark` measured 60 post-warmup iterations per case with 4,095 moving transforms. The parallel path can use the calling thread in addition to the configured workers. Values are mean preparation wall milliseconds; this is a focused CPU workload, not a rendered frame capture.

| Workers | Serial roots | Parallel roots | Serial hierarchy | Parallel hierarchy |
|---:|---:|---:|---:|---:|
| 1 | 39.76 | 23.36 | 40.11 | 25.69 |
| 2 | 41.50 | 16.57 | 40.12 | 19.52 |
| 4 | 40.19 | 11.56 | 42.89 | 13.92 |
| 8 | 43.94 | 9.21 | 44.77 | 13.62 |
| 14 | 41.70 | 7.53 | 42.86 | 13.58 |

The root workload continues to improve through 14 workers. The hierarchy workload levels off near eight workers. This demonstrates worker scaling for transform preparation; it does not establish the best worker count for GPU-heavy scene frames.

## Regression checks

| Area | Check | Result |
|---|---|---|
| Scene loading and Play/Stop | `GrassPerformance`, `EditorPlayPipeline`, `EditorPlaySession`, and `EditorPlaySessionTests --grass` | Passed. The full grass scene loaded and completed a retained-scene Play/Stop cycle. Its runtime copy took 116.7 seconds and Stop cleanup took 2.56 seconds; correctness passed, but these are not interactive latency targets. |
| Streaming | `TerrainManagerRegression`, `TerrainCellWorkerRegression`, `TerrainFarTileScheduling`, `TerrainColliderStreaming`, and the grass streaming capture | Passed after repairing the test fixture described below. |
| Transforms and spatial preparation | `GrassPerformance`, `RenderSpatialIndex`, and the worker-scaling benchmark | Passed, including serial/parallel hierarchy and sparse/dense spatial-index checks. |
| Shadows and all renderers | `ShadowSparseSlotsDX11`, `ShadowSparseSlotsDX12`, `ShadowSparseSlotsVulkan`, plus automatic rendering pixel checks on DX11, DX12, and Vulkan | Passed. Each automatic-render backend reported zero differing reference channels. |

The first `TerrainManagerRegression` run crashed in its final runtime-terrain check: `Scene::Start()` unloaded the cell, and the test dereferenced `GetLoadedCell(0, 0)` without reloading it. The fixture now calls `SyncLoadedCells(0, 0)` and asserts the cell exists before testing runtime edit restrictions. The manager and cell-worker tests passed on rerun. This was a test setup error rather than evidence of a terrain worker failure.

Raw captures, summaries, regression output, scaling samples, and backend logs are in `.local/performance-logs/validation-20261103/`. The full editor, physics, and release-build frame paths were outside these captures. Renderer checks cover the automatic draw and shadow paths, not every material or scene feature.
