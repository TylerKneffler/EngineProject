# Foliage landscape render baseline — 2026-11-03

`Scripts/benchmark_foliage_baseline.py` runs `GrassPerformanceTests` in a fresh
process per case. The test loads `grass_flat_test.scene`, warms until grass
generation settles, then measures 240 frames on a fixed or small sinusoidal
moving route.
These original runs did not start `TerrainManager`, so their apparent streamed
landscape measurements contain only the authored center terrain cell and coarse
distant ground. See `grass-single-cell-comparison-20261103.md` for the isolated
cell and empty-scene comparison. Do not use these results to infer the cost of
the full resident grid.
The fixed scene, DX11 backend, scene shadow settings, and resolution match within
each group. This helper covers grass updates and rendering, without editor UI,
audio, physics, or full gameplay. Frame intervals are wall time around frame
submission and presentation; GPU stage timestamps resolve asynchronously.

| Build / view | Foliage | Mean ms | P95 ms | P99 ms | Grass GPU ms | Shadow GPU ms |
|---|---|---:|---:|---:|---:|---:|
| Debug 790×589 | Off | 10.26 | 13.68 | 18.53 | 0 | 0 |
| Debug 790×589 | On | 17.33 | 22.67 | 24.58 | 4.42 | 8.75 |
| Release 790×589 | Off | 9.08 | 10.28 | 11.10 | 0 | 0 |
| Release 790×589 | On, grass shadows off | 9.99 | 12.66 | 13.20 | 3.95 | 0 |
| Release 790×589 | On | 19.37 | 25.83 | 35.37 | 4.51 | 9.26 |
| Release 1920×1080 | Off | 33.21 | 34.29 | 34.46 | 0 | 0 |
| Release 1920×1080 | On, grass shadows off | 24.15 | 26.80 | 28.39 | 7.88 | 0 |
| Release 1920×1080 | On | 31.17 | 33.90 | 34.98 | 7.87 | 7.44 |
| Release 790×589, shadow-policy check | On, grass shadows off | 9.04 | 10.27 | 10.98 | 3.19 | 0 |
| Release 790×589, shadow-policy check | On | 15.29 | 17.84 | 19.65 | 3.30 | 7.80 |
| Release 790×589, fixed | Off | 9.55 | 10.65 | 11.83 | 0 | 0 |
| Release 790×589, fixed | On, grass shadows off | 9.27 | 11.53 | 12.56 | 4.16 | 0 |
| Release 790×589, fixed | On | 12.51 | 13.36 | 14.73 | 3.51 | 5.05 |

All GPU averages have 240 distinct resolved samples. Foliage cases reached 648
grass instances and a 65,536-byte persistent instance pool. There were no tile
builds or placement uploads during the measured route; streaming and teleport
spikes are not represented here. The 1920×1080 off case repeated at 33.69 ms
mean and 35.57 ms P95 after the other cases, so its unexpectedly high terrain
cost is reproducible in this helper. The high-resolution ordering differs from
the low-resolution ordering; visual/depth and GPU captures are needed before
claiming an optimization from that comparison. GPU stage times overlap CPU work
and must not be added to frame intervals.

At 790×589 Release, disabling grass shadows reduced the full-foliage mean
interval by 9.38 ms in the first pair and 6.25 ms in a second pair after fixing
matrix shadow-state restoration. These are separate runs, so the spread is a
measure of run-to-run variability, not a code improvement. Shadow policy is the
first candidate for a quality-preserving optimization, subject to image
comparison. At 1920×1080 even the foliage-off case exceeds the plan's 10 ms
frame target, so that target needs a defined hardware and quality tier.

Raw JSONL traces and summaries are under `.local/performance-logs/`:

- `foliage-baseline-debug-790x589-20261103/`
- `foliage-baseline-release-790x589-20261103/`
- `foliage-baseline-release-1920x1080-20261103/`
- `foliage-baseline-release-1920x1080-off-repeat-20261103/`
- `foliage-baseline-release-790x589-shadow-policy-20261103/`
- `foliage-baseline-release-fixed-790x589-20261103/`

The remaining plan gates still require a camera turn, sprint and
teleport, other backends, GPU overdraw capture, and visual comparisons. Those
must be captured before changing authored foliage quality or claiming the
100 FPS objective.
