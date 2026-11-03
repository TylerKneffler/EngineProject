# Editor performance and Stop freeze evaluation

Source: `.local/performance-logs/performance-20261102-214438-51.jsonl`.
Full evaluator report: the adjacent `performance-20261102-214438-51-evaluation.md`.
The recording has 908 detailed frames, of which 743 are marked playing. No
records were dropped or failed (1,860 accepted and written).

## Normal frame work

Excluding the single blocking Stop frame for this normal-work breakdown, Play
processing averages 23.30 ms: update 7.93 ms and rendering 15.36 ms. Recorded Play
intervals average 23.55 ms (42.46 FPS), with P95 36.13 ms and P99 51.73 ms.
Render preparation is 3.31 ms, including item preparation 1.62 ms. Main view
submission averages 6.36 ms and shadow submission 3.55 ms. These are nested CPU
scopes, not additional independent frame costs.

GPU stages total 11.57 ms: shadows 7.75 ms (67%), grass 3.16 ms and terrain 0.66 ms.
GPU timings resolve asynchronously and overlap CPU work. They must not be added
to CPU timings to estimate frame time, and low whole-device utilization does not
show which serialized CPU or GPU stage limits the frame.

Thirty workers are configured, but render worker execution averages only 0.63 ms
in its current measurement scope. Eligible batch grouping averages 166 draws and
stays below its dispatch threshold on measured Play frames. Compatibility key
reuse is already about 99.8%; the spatial index averages 0.038 ms. More workers
on these small operations cannot remove the remaining serialized work.

Code audit: `EditorApplication` performs scene updates, render preparation, view
rendering, UI and post-frame scene operations in sequence on its editor loop.
The dedicated simulation/render owners in `GameApplication` belong to the
standalone native game path. Editor Play does not gain that inter-frame owner
overlap. This recording alone is not a controlled serial/parallel FPS comparison.

## Observed Stop freeze

Frame 810 records 55,227.6 ms processing, of which 55,212.7 ms is in
`pending_load_ms`. That timer includes `finishPlayStop` / `RestorePlayModeScene`
as well as pending scene loads. Console output spans 55.213 seconds between
`[Play] Stopped` and `[Play] Restored pre-play scene state`.

The next frame is marked stopped and carries the 55,229.4 ms interval. Filtering
intervals to Play frames therefore omits the pause even though the preceding
Play processing frame contains it. The evaluator now retains a full-recording
interval summary and explicit blocking-frame/transition table before filtering.

## Isolated restore audit

`GrassPerformanceTests --stop-restore-audit` loads the actual 441-cell scene,
captures the binary pre-play snapshot, starts/updates it, captures the rollback
snapshot, fences, clears, deserializes and publishes again on its owner.
The test uses a separate hidden window and leaves the running editor open.

| Operation | Measured ms |
|---|---:|
| Initial load | 81,361.7 |
| Pre-play snapshot encoding (191,774 bytes) | 55.0 |
| Start/update | 173.0 |
| Stop rollback snapshot encoding | 55.4 |
| Background-job fence | 0.069 |
| Clear scene, including idle/destruction callback | 1,365.0 |
| Restore deserialization | 42,120.6 |
| First restored scene snapshot publication | 12,422.9 |

These phases explain the observed order of magnitude without attributing the
freeze to the scheduler or GPU fence. This is one Debug helper run, not exact
instrumentation of each phase in the user's Editor Stop event. Snapshot rollback
encoding is comparatively small; the heavy work is reconstruction and first
configuration publication.

The authored scene references `GrassStressCell.prefab` 440 times; that file is
429,427 bytes. `InstantiatePrefab` reads/parses it for every instance, reconstructs
components, then serializes each normalized instance's prefab baseline. Scene
snapshot publication serializes configuration for all recreated components again.
The audit restores 885 authored objects after 969 objects during play.

## Changes and next work

- Added the full-scene stop/restore diagnostic helper and report.
- Extended the evaluator to preserve transition stalls and full-recording FPS
  independently of the Play-only selection; a regression covers a Stop freeze
  whose long interval is carried by the first stopped frame.
- Updated TODOs for editor owner overlap, retained/incremental Play restoration,
  versioned prefab-document reuse, restored-configuration preparation and explicit
  Stop phase timing.

The 55-second UI freeze is diagnosed, not fixed by these instrumentation changes.
Avoiding synchronous reconstruction and first publication is the next engine
change; simply dispatching more small render loops does not address it.
