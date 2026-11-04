# Terrain and foliage isolation benchmark - 2026-11-03

**Foliage causes the expensive CPU render-item preparation.** Terrain-only averages about 0.144 ms in that scope, while foliage-only averages 9.869 ms. The combined scene averages 9.963 ms.

Debug DX11, fixed 1920 x 1080 render targets, stationary camera, original grass stress scene. Each case runs in a fresh process with at least 60 warmup frames and 240 measured frames. Three repetitions per case (720 measured frames each), rotated case order. All nine runs passed.

| Case | Loop wall ms | Scene preparation ms | Render-item preparation ms | Measured GPU ms (with shadows) | Average FPS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Terrain only | 4.199 | 0.461 | 0.144 | 0.535 | 238.1 |
| Foliage only | 13.610 | 10.030 | 9.869 | 3.623 | 73.5 |
| Terrain + foliage | 13.978 | 10.130 | 9.963 | 4.310 | 71.5 |

Render-item preparation is nested inside scene preparation. CPU loop and GPU times overlap; do not add them. The loop includes the direct renderer, grass updates, presentation and shared bookkeeping, but no editor UI, full gameplay loop or physics. These FPS values are not directly comparable to the earlier interactive editor capture.

Foliage-only and combined cases keep exactly 3,586 grass instances, with 88 visible grass tile meshes. Terrain-only keeps 10 visible terrain meshes and zero visible foliage; foliage-only keeps zero visible terrain meshes. Terrain data remains loaded for placement and streaming metadata in every case, so this isolates rendering cost rather than terrain memory. All measured frames have zero tile builds and zero grass placement upload bytes: the expensive preparation remains even with resident placements.

The mean render-item preparation time per repeat is:

| Case | Repeat 1 ms | Repeat 2 ms | Repeat 3 ms |
| --- | ---: | ---: | ---: |
| Terrain only | 0.140 | 0.146 | 0.146 |
| Foliage only | 9.947 | 9.890 | 9.771 |
| Terrain + foliage | 9.911 | 9.996 | 9.981 |

Foliage update itself averages about 0.240 ms in the isolated case. The main cost is preparing its render items each frame. The static-item cache excludes instanced meshes, consistent with this result; finer profiling inside item construction is still needed to distinguish material/shader preparation from mesh state and metadata work.

This benchmark removes terrain draw surfaces from the foliage-only case, so occlusion, shadow receivers, and GPU visibility differ. The isolated times are not additive. Shadow quality, camera and render resolution are held constant.

Added reusable `--modes foliage-only` support and explicit scene/item preparation and visible-mesh counters to the local benchmark helper.

Example: `python .local/scripts/benchmark_foliage_baseline.py .local/performance-logs/my-isolation --config Debug --route fixed --modes off foliage-only on`.

Capture folders: `.local/performance-logs/grass-isolation-1080p-20261103-{1,2,3}`. Aggregate data: `.local/performance-logs/grass-isolation-1080p-20261103-summary.json`.
