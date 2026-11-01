# Grass performance evaluation ? 23:03 capture

Capture: `.local/performance-logs/performance-20261031-230324-943.jsonl`. Analysis uses 1,306 playing frames over approximately 22.84 seconds. One Game view at **1753 x 982**; no Scene view. Shadow settings remain four cascades in a 2048 atlas. Hardware CPU figures below use the 44 samples within the playing-frame timestamp range.

## Result

**57.18 FPS** from frame intervals: **17.49 ms average**, 24.81 ms P95, 27.44 ms P99. 52.83% of frames exceed the 60 FPS budget. The capture remains primarily CPU limited.

| Measurement | Mean |
|---|---:|
| CPU frame work | 16.843 ms |
| Main-thread CPU, one logical core | 97.648 % |
| Process CPU core equivalents | 1.113  |
| Scene update | 4.875 ms |
| Object updates | 2.478 ms |
| Grass management | 0.610 ms |
| Physics | 1.890 ms |
| Main render preparation/submission | 5.376 ms |
| Main upload phase | 0.883 ms |
| Shadow CPU preparation/submission | 2.032 ms |
| Editor UI | 1.804 ms |
| Summed worker execution wall time | 0.545 ms |
| Worker synchronization wait | 0.006 ms |
| Record publication wait | 0.003 ms |
| Resolved GPU stages | 7.031 ms |
| GPU shadows | 3.710 ms |

Nested CPU phases overlap, and GPU timestamps resolve asynchronously. These rows must not be summed into frame time. Worker execution is summed wall time rather than a hardware CPU utilization percentage.

## Remaining bottlenecks

1. **View/streaming changes still cause placement uploads.** 579 of 1306 playing frames (44.3%) uploaded merged placement input. 389 frames uploaded placements despite building no new tiles. Uploading frames averaged 1.89 ms in the main upload phase; its maximum was 5.39 ms. The capture averaged 46.2 KiB/frame of placement uploads.

This exposes a remaining limitation of the current cache: it preserves buffers when the member list is unchanged, but visibility membership, submission-order cache slots, and prepared object-record indices can change. The implementation can then reread/merge unchanged transforms and upload them. The log proves the upload behavior; it does not separately count which invalidation reason caused each miss. The next generic fix is persistent instance storage independent of view visibility, compact visible-ID buffers, persistent scene object IDs, and cache entries keyed by semantic batch identity.

2. **Scene changes cause update/preparation spikes.** The scene built 222 tiles and indexed 9,072 placements during the capture. Frames with newly built tiles averaged 7.35 ms in scene update. The worst measured CPU frame was 29.77 ms; another frame reached 12.53 ms in scene update. Adding runtime objects invalidates the global structure revision, which can rebuild unrelated typed registries and render lookups. Incremental component registration and render-item updates should replace these broad invalidations.

3. **Physics overhead is mostly bookkeeping.** Setup averaged 1.083 ms, Bullet simulation 0.074 ms, and post-step work 0.729 ms. There are 441 registered terrain collider cells but only 1?4 active cells; activation is working. Maintain generic active-body lists so suspended terrain bodies avoid repeated setup/synchronization scans.

Worker waits are approximately 0.006 ms and record publication waits 0.003 ms. Synchronization between preparation workers is not the current frame-time problem. GPU shadows remain the largest measured GPU stage, but average GPU stage time is about 7.03 ms against 16.84 ms of CPU frame work, so lowering visual quality is not the first fix for this capture.

## What is working

- The spatial scheduler processes about 44.4 candidate fields and skips 396.6 dormant fields.
- Main rendering issues about 11.8 GPU grass batches and shadows about 17.9. GPU LOD classification is active; CPU LOD transition tiles remain zero while shader fades are used.
- Collider activation stays within the requested 1?4 cells.
- The quieter final 200 frames average 77.16 FPS (12.96 ms intervals), with zero placement uploads and zero new tiles. This is a quieter segment of the same capture, not a controlled camera-motion experiment.

## Comparison limits

The 21:30 capture averaged 36.64 FPS; this capture averages 57.18 FPS. The new viewport contains 12.9% fewer pixels and the route/workload is uncontrolled, so a precise optimization speedup cannot be attributed from that comparison. The prior automated 83.62 FPS audit used a smaller 1208 x 900 viewport, a stationary camera, and zero steady placement uploads. Its FPS is not directly comparable to this larger, changing workload.

Generated metric reports: `.local/performance-logs/evaluation-20261031-230324.json` and `.md`. No engine code was changed during this evaluation.
