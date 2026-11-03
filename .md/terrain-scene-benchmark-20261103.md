# Terrain performance scene benchmark — 2026-11-03

`Scripts/benchmark_terrain_scene.py` runs the authored
`Engine/Core/Assets/Scenes/terrain/terrain_performance.scene` in the Release
DX11 render helper. Each case uses a fresh process, waits at least 180 frames
and for initial map data, then measures 240 frames on the same moving route.
Parallel preparation is enabled. The frame interval includes scene update,
rendering, and presentation; it excludes editor UI. All GPU averages below
use 240 distinct resolved timestamp samples. The scene uses four cascades and
a 1024-pixel shadow atlas when shadows are enabled.

| View | Shadows | Frame P50 ms | Mean ms | P95 ms | P99 ms | Terrain GPU mean ms | Shadow GPU mean ms | Update mean ms | Render CPU mean ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 960 x 540 | On | 15.68 | 15.84 | 20.11 | 28.87 | 8.09 | 6.31 | 0.97 | 0.50 |
| 1920 x 1080 | On | 37.64 | 37.21 | 49.09 | 75.93 | 27.03 | 6.39 | 1.12 | 0.79 |
| 1920 x 1080 | Off | 29.18 | 29.52 | 43.07 | 61.97 | 24.92 | 0 | 1.05 | 0.89 |

The measured route peaked at 10 loaded detailed cells and 5 built far tiles.
The 1080p shadow-off case still takes 29.52 ms per frame and records 24.92 ms
in the terrain GPU stage. This directly supports terrain rendering as the
largest cost in this scene at 1080p. Disabling shadows changed the 1080p mean
from 37.21 to 29.52 ms in adjacent runs. The terrain GPU stage rose from 8.09
to 27.03 ms when view pixels quadrupled, consistent with a substantial
resolution-dependent cost. A GPU capture is still needed to distinguish
terrain shader work, texture sampling, and fill/overdraw within that stage.

These numbers are for the terrain performance scene, not the grass stress
scene. They should not be subtracted from that scene's foliage results. CPU
wall time and asynchronously resolved GPU stages overlap, so their means must
not be added to reconstruct a frame interval.

Raw traces and machine-readable summaries:

- `.local/performance-logs/terrain-scene-release-20261103/`
- `.local/performance-logs/terrain-scene-release-no-shadows-20261103/`
