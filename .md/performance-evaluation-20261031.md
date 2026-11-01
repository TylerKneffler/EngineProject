# Grass scene performance evaluation: 21:05 capture

## Comparison with the blank default scene

Default source: `.local/performance-logs/performance-20261031-210741-146.jsonl`.
Both captures are playing with one 1674 x 850 game view. The default capture
contains 1,015 frames over about 2.6 seconds; the grass capture spans 15.8 seconds.

| Measurement | Blank default | Grass landscape | Added cost |
|---|---:|---:|---:|
| Throughput | 384.1 FPS | 39.1 FPS | Frame interval is about 9.8 times longer |
| CPU frame wall time | 2.31 ms | 25.04 ms | 22.73 ms |
| Scene updates | 0.05 ms | 6.22 ms | 6.18 ms |
| Main view pass | 0.26 ms | 9.46 ms | 9.20 ms |
| Shadow pass, CPU | 0.18 ms | 4.79 ms | 4.61 ms |
| Scene preparation | 0.02 ms | 2.09 ms | 2.08 ms |
| Editor UI | 1.55 ms | 1.77 ms | 0.22 ms |

The large difference is landscape workload. Editor UI cost barely changes.
Grass CPU updates account for 1.55 ms of the added work; they do not explain
the 22.73 ms total increase. Terrain/object preparation, main rendering, shadows,
physics, and the old combined pose/audio stage account for the remaining cost.
GPU shadows add 10.62 ms to measured GPU work. That overlaps CPU execution and
must not be added to the CPU rows. The default recording has zero ordinary mesh
draws and effectively zero measured geometry GPU stages, so it establishes editor
baseline rather than a comparable geometry workload. These two archives predate
the newly added startup/terrain/occlusion, audio, and process/thread measurements.

## Grass capture details

Source: `.local/performance-logs/performance-20261031-210559-971.jsonl`.
619 playing frames, about 15.8 seconds, one 1674 x 850 game view.

Average throughput is **39.08 FPS**, with a 25.59 ms mean frame interval,
31.38 ms P95, and 33.91 ms P99. CPU frame wall time averages 25.04 ms.
All sampled intervals exceed the 16.67 ms budget for 60 FPS.

| CPU measurement | Average ms | Interpretation |
|---|---:|---|
| Rendering | 18.81 | Largest portion of frame wall time |
| Main view pass | 9.46 | Includes setup, culling, preparation, uploads, and submission |
| Shadow pass | 4.79 | About 3.04 ms of shadow scene preparation |
| Scene preparation | 2.09 | Includes spatial work missing from the old preparation sub-timers |
| Editor UI | 1.77 | Editor overhead |
| Scene update | 6.22 | Includes grass, pose/audio, physics, and deferred changes |
| Grass | 1.55 | Only 0.009 ms instance preparation and 0.016 ms placement uploads |
| Pose and audio combined | 1.68 | Old label cannot identify which system costs this time |
| Physics | 1.68 | 0.82 ms setup, 0.08 ms simulation, 0.78 ms post processing |

Rows are nested measurements and must not be added together.
Terrain collision remains limited to one or two cells in every hardware sample.
Grass prepares 128 instance tiles while moving; placement indexing totals 6,480.
Generation and uploads are no longer the main grass CPU cost.

GPU timestamps resolve asynchronously. All 619 valid sample IDs are unique.
Measured GPU stages average **14.20 ms**, including **10.62 ms shadows**,
1.26 ms terrain, and 2.33 ms opaque geometry. Shadows account for about **75%**
of measured GPU time. CPU wall time and GPU time overlap; adding them would be
incorrect. The measured GPU stages exclude untimed work and gaps.

The main view averages one automatic GPU batch with 143 instance candidates.
Its existing detailed CPU timers account for about 5.76 ms of the 9.46 ms main
pass, leaving **3.70 ms unclassified**. The next capture separates camera/lighting
setup, terrain occluder preparation, occlusion bookkeeping, and remaining work.
Do not attribute the entire main-pass timer to driver draw calls.

The earlier 18:56 recording averaged 22.87 FPS, but it rendered 1848 x 1069:
the latest view has 28% fewer pixels. The improvement is real for these captures,
but this is not a controlled measurement of a single optimization.

Next priorities are GPU shadow cost and the remaining CPU rendering cost.
Evaluate shadow resolution/cascades, coverage, and grass caster distance against
visual quality. Use the new timing breakdown before choosing the next CPU change.
Process and main-thread CPU counters are needed to distinguish active CPU work
from waits; the existing system CPU and GPU-engine percentages cannot do that.

## Updated diagnostics

The Performance view now shows rolling interval statistics, batching/physical draw
counts, changed upload bytes, grass instance/LOD/fade workload, per-type shadow
draw counts, separate audio/constraint timing, and the complete main-pass breakdown.
Render sub-timers aggregate all rendered main views, including prefab views.
Shadow eligible-caster counts remain from the shadow pass and count unique objects.
Hardware recording identifies system-wide CPU/GPU scope and adds editor-process
CPU usage, logical-core equivalents, main-thread CPU usage, shadow atlas size,
allocation generation, and cascade splits. Frame records use schema version 2.

Reproduce the evaluation:

```powershell
python Scripts/evaluate_performance.py .local/performance-logs/performance-20261031-210559-971.jsonl --compare .local/performance-logs/performance-20261031-185609-942.jsonl --output .local/performance-logs/evaluation-20261031-210559
```

The evaluator writes JSON and Markdown, uses frame intervals rather than smoothed
UI FPS, ignores invalid GPU samples, deduplicates resolved GPU sample IDs, and
reports missing instrumentation and workload differences. Existing captures
cannot supply measurements that were not recorded; the added fields appear in
new recordings from the rebuilt editor.

Validation: editor build succeeded; both recording/grass regression targets and
three evaluator tests passed. The isolated 441-field diagnostic audit completed
all 240 moving frames and verified that shadow draw-type totals agree with actual
submissions and that main rendering preserves shadow caster counts. Its output
is `.local/performance-logs/grass-diagnostics-audit.jsonl`. The audit averaged
19.02 ms per helper frame; it excludes full gameplay updates and editor UI.
