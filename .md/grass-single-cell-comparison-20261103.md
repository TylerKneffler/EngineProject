# Grass terrain cell versus empty scene — 2026-11-03

Release, DX11, 1920×1080, 240 measured frames per case. Each run used a fresh
process. Frame intervals include submission and presentation. GPU stages are
asynchronously resolved and should not be added to frame intervals. The empty
default scene contains a camera and light but no mesh; its camera and light
settings are not identical to the grass test scene.

| Scene | Grass | Terrain surfaces | Mean frame ms | P95 ms | Mean terrain GPU ms | Mean render CPU ms |
|---|---:|---:|---:|---:|---:|---:|
| Empty default | Off | 0 | 4.19 | 5.02 | 0 | 0.36 |
| One detailed cell | Off | 1 | 28.87 | 30.24 | 24.57 | 0.53 |
| One detailed cell | On | 1 | 27.21 | 29.95 | 8.16 | 0.94 |
| One detailed cell + coarse distant ground | Off | 2 | 33.54 | 35.55 | 29.07 | 0.56 |
| One detailed cell + coarse distant ground | On | 2 | 31.17 | 33.94 | 12.13 | 0.98 |

The one-cell grass-free scene is 24.68 ms slower per frame than the empty scene
in these runs. Its terrain GPU stage accounts for 24.57 ms, while the recorded
render CPU work is 0.53 ms. Thus the large cost is in the terrain rendering
path for this view and hardware, and it occurs with only one detailed cell.
Adding the coarse distant ground increased the grass-free frame mean by 4.67 ms.

The grass-on case has a lower measured terrain stage and a slightly lower frame
interval than grass-off, despite adding grass and shadow work. That ordering
needs a GPU capture and controlled render-state investigation before treating
grass as an optimization; the stage labels and visibility/depth behavior can
change how the work is attributed. These measurements do not identify the
specific terrain shader, draw, material, or pixel cost.

The original `grass_flat_test.scene` render baseline did not start its
`TerrainManager`. It therefore rendered only the authored center cell plus
the coarse distant ground, although the scene describes a streamed grid.
The corrected streamed benchmark harness now starts the manager and records
resident terrain counts, but a completed measurement is pending: the local
build tree was empty when rerunning, and CMake could not fetch its dependencies
through the restricted network. The partial warmup trace is not a benchmark.

Scene fixtures:

- `Engine/Core/Assets/Scenes/terrain/grass_single_cell_test.scene`
- `Engine/Core/Assets/Scenes/terrain/grass_single_cell_distant_test.scene`
- `Engine/Core/Assets/Scenes/Basics/default.scene`

Raw traces and summaries are in `.local/performance-logs/grass-single-release-1920x1080-20261103/`,
`.local/performance-logs/grass-single-distant-release-1920x1080-20261103/`, and
`.local/performance-logs/grass-empty-default-release-1920x1080-20261103/`.
