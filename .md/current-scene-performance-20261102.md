# Current grass and terrain scene performance

Fresh DX11 Debug benchmarks used the current authored scenes. Each mode has 240 measured frames after warmup. "Serial" disables parallel-for loops; background jobs and owner-thread phases remain active. The grass helper measures spatial grass updates and rendering without editor UI or full physics. The terrain helper measures scene update and rendering without editor UI. Grass uses 1848 x 1069; terrain uses 960 x 540.

| Route | Order | Serial frame mean / P95 ms | Parallel frame mean / P95 ms | Serial render CPU ms | Parallel render CPU ms | Serial GPU grass/terrain ms | Parallel GPU grass/terrain ms |
|---|---|---:|---:|---:|---:|---:|---:|
| Grass resident | serial first | 106.51 / 238.31 | 110.42 / 216.40 | 22.43 | 19.09 | 72.67 / 15.38 | 76.38 / 15.79 |
| Grass resident | parallel first | 91.06 / 185.23 | 89.27 / 189.79 | 13.60 | 14.17 | 64.70 / 13.45 | 63.23 / 13.22 |
| Grass streaming | serial first | 204.90 / 395.74 | 82.66 / 119.92 | 43.22 | 15.43 | 79.61 / 23.58 | 54.56 / 15.13 |
| Grass streaming | parallel first | 96.04 / 177.46 | 85.23 / 134.67 | 17.45 | 15.61 | 62.46 / 17.42 | 56.44 / 15.60 |
| Terrain moving | serial first | 16.01 / 17.92 | 16.63 / 18.32 | 2.45 | 2.72 | n/a / 4.35 | n/a / 4.70 |
| Terrain moving | parallel first | 19.16 / 25.67 | 20.68 / 39.62 | 3.59 | 4.53 | n/a / 5.47 | n/a / 4.89 |

Grass resident has no repeatable frame gain. Grass streaming improves with parallel-for enabled in both orders, but the serial-first run had a much larger cold-state cost. Average visible grass instances differed: 2,582 vs 2,944 in the serial-first pair, and 2,890 vs 2,951 in the parallel-first pair. GPU stage work also changed substantially, so the frame deltas cannot be attributed solely to CPU parallelism.

The current terrain scene is slightly slower with parallel-for enabled in both orders (0.62 and 1.52 ms mean frame increases). Terrain worker-active time is below 0.3 ms per frame in both modes, leaving little useful work to parallelize. These Debug runs do not establish a release-build regression.

The old terrain performance regression expects a mandatory runtime bake, while the authored scene has an empty bake directory and requires no bake. Its CTest registration was removed. The new terrain benchmark enters immediately after loading the current scene, validates that configuration, waits for initial map data, and measures a fixed moving camera route.

Raw JSONL, stdout and per-run summaries are in .local/performance-logs/current-scene-20261102/ and .local/performance-logs/current-scene-20261102-reverse/. Scripts/benchmark_current_scenes.py recreates the clean comparison in a new output directory. GPU stages are asynchronous and overlap CPU work.
