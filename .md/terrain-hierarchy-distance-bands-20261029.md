# Terrain hierarchy and radius comparison — 2026-10-29

Release DX11 game benchmark, 1920×1080, ridge viewpoint, 360° heading sweep,
walking and running phases, followed by a moving cell-boundary crossing with
turning. The camera clip distances match within each pair. The hierarchy path
uses a one-cell interactive radius. The radius path uses the listed far radius
and disables the far height grid. These are game test results, not editor play
view FPS. Frame times are measured by the benchmark without an editor layout.

| Camera clip | Path | Resident cells peak | Far tiles visible at end | Draws p95 | Frame p50 / p95 | GPU p95 | Visible terrain reach |
|---:|---|---:|---:|---:|---:|---:|---:|
| 3 km | Hierarchy | 9 | 1 | 2 | 2.72 / 3.41 ms | 1.84 ms | 2.93 km |
| 3 km | Radius 5 | 100 | 0 | 5 | 3.02 / 3.70 ms | 2.23 ms | 0.91 km |
| 9.5 km | Hierarchy | 9 | 8 | 9 | 3.10 / 4.04 ms | 2.23 ms | 9.48 km |
| 9.5 km | Radius 14 | 361 | 0 | 17 | 9.09 / 10.65 ms | 9.62 ms | 2.53 km |
| 16 km | Hierarchy | 9 | 8 | 10 | 3.21 / 3.98 ms | 2.30 ms | 15.98 km |
| 16 km | Radius 24 | 841 | 0 | 49 | 13.40 / 22.23 ms | 12.00 ms | 4.34 km |

Visible terrain reach uses projected far-tile vertices on the hierarchy path
and projected resident-cell centers on the radius path. The two measures have
different sample positions, but the large reach difference is clear. Equal
camera clip planes did not give equal visible terrain coverage.

## Hierarchy resident memory by band

Peak far tile mesh memory for each stride, excluding near cells and shared
textures. Counts and memory peaks may occur on different frames.

| Clip | Stride | Peak resident tiles | Peak visible tiles | CPU MiB | GPU MiB |
|---:|---:|---:|---:|---:|---:|
| 3 km | 1 | 1 | 1 | 0.695 | 0.481 |
| 3 km | 2 | 0 | 0 | 0 | 0 |
| 3 km | 4 | 0 | 0 | 0 | 0 |
| 9.5 km | 1 | 6 | 6 | 2.681 | 2.321 |
| 9.5 km | 2 | 3 | 3 | 0.965 | 0.779 |
| 9.5 km | 4 | 2 | 2 | 0.316 | 0.208 |
| 16 km | 1 | 7 | 7 | 3.078 | 2.689 |
| 16 km | 2 | 7 | 5 | 1.763 | 1.450 |
| 16 km | 4 | 2 | 2 | 0.316 | 0.208 |

The authored 129-cell-wide test scene needs strides 1–4. The scheduling test
also constructs a 385-cell-wide grid and confirms that stride 8 is selected
without creating 100 visible tile objects. It checks the independent far
distance cap.

The rendered benchmark recorded zero near/far coverage gaps in every sampled
look, walk, run, and moving/turning crossing frame at each hierarchy distance.
This is evidence for these tested routes and view distances; it is not a proof
for every possible camera path. After the comparison, the `TerrainManager`
code default was reduced from 2 to 1 cell. The saved performance scene was
already at 1 cell and was not changed.

The JSON files with frame-time percentiles, tile counts, draw counts, visible
reach, per-level memory, and coverage metrics are in `.local/performance-logs` under
`terrain-1080p-far-{3000,9500,16000}-radius-*-{distant,cells-only}-ridge-near-1-legacy-sweep-cold.json`.
