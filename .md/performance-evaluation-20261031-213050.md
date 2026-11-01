# Grass capture at 21:30: main-thread saturation

Source: `.local/performance-logs/performance-20261031-213050-154.jsonl`.
670 playing frames, one 1848 x 1069 game view, schema version 2.

Average throughput is **36.64 FPS**: 27.29 ms mean frame interval and 26.71 ms
CPU frame wall time. P95 interval is about 33.7 ms; worst interval is 54.84 ms.
The main thread averages **94.62% of one logical core**. Editor process CPU usage
averages **1.016 logical-core equivalents**, or **3.175% of the 32-thread system**.
Low whole-machine CPU percentage therefore hides a saturated main thread.

| CPU measurement | Average ms |
|---|---:|
| Rendering | 20.25 |
| Main render pass | 9.27 |
| Shadow pass | 6.21 |
| Scene preparation | 2.23 |
| Editor UI | 1.83 |
| Scene updates | 6.46 |
| Grass updates, included above | 1.58 |
| Audio, included above | 1.50 |
| Physics, included above | 1.64 |
| Constraints, included above | 0.24 |

These are nested CPU wall-time measurements. Pacing averages 0.003 ms and Present
0.100 ms, so neither is imposing the observed frame rate. Actual Bullet simulation
averages only 0.084 ms; setup and post processing account for most physics time.
Only one or two terrain colliders are active.

The main pass now has a full breakdown: startup/camera/lighting 1.96 ms, terrain
occluder preparation 0.44 ms, view culling 1.41 ms, material preparation 0.90 ms,
batching 0.21 ms, uploads 0.07 ms, draw submission 2.82 ms, and other/overlays
1.37 ms. It submits about 71 physical draws. Terrain uses one automatic GPU batch
with about 137 instance candidates; grass retains separate explicit tile batches.

Shadow preparation averages 3.08 ms. Shadow geometry submission averages 2.16 ms,
but reaches 22.96 ms in the worst sample. Draw-call wall time alone cannot separate
driver CPU work from synchronization. All recorded main shadow draws are explicit
instance draws, about 112 per frame, rather than automatic or ordinary draws.
In this scene those are the grass instance batches. Four cascades use a 2048 atlas
with splits around 13.4, 28.2, 53.2, and 150 world units.

Measured GPU stages total **12.96 ms**, including **9.44 ms shadows** (72.9%),
1.14 ms terrain, and 2.37 ms opaque geometry. GPU samples resolve asynchronously
and overlap CPU work. System GPU-engine usage averages 30.4%; this is not an
editor-only or selected-adapter utilization measurement.

The next CPU targets are repeated scene/render preparation, global batching of
grass across tiles, grass shadow submission/cascade coverage, and audio update
overhead. Grass instance generation averages only 0.012 ms and uploads 0.022 ms;
moving more generation work to the GPU would not address the current bottleneck.
The main thread needs less work or safe parallel preparation, rather than simply
requesting higher overall processor utilization.
