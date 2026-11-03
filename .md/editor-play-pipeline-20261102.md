# Editor Play simulation/submission overlap

Editor Play now records owned commands for the runtime Scene and Game views,
then advances one simulation tick on a persistent owner worker while the window
and graphics owner replays those commands. The live scene returns to the editor
before the next UI interaction. This is a generic editor path, not grass logic.

## Ownership and lifecycle

- UI editing and camera interaction finish before packet capture. Replay does
  not inspect live scene objects. Independent asset/document views retain the
  serial editing path.
- Exactly one simulation tick can be outstanding. Native input and embedded
  Game-view pointer coordinates travel with that tick. Unfocused input clears
  hover/click state; skipped render surfaces still allow simulation updates.
- Completion returns scene ownership even when an update throws. Destructive
  lifecycle GPU waits marshal back to the graphics owner after frame submission.
- Stop, scene replacement, hot reload and shutdown drain the handoff and its
  retired resources before replacing scene/module ownership. Reentrant window
  resize/focus events defer until the owner returns.
- Editor DX11 event queries, DX12 fences and Vulkan submission serials retain
  packets and replaced buffers/textures/pipelines through GPU completion.
  Outstanding GPU frames are bounded to three.
- Viewport texture resizing happens before UI draw-data construction. UI panels
  release their registrations before UI-backend shutdown, with the graphics
  device still alive. The prior Vulkan editor shutdown failed in both serial
  and parallel controls because its UI backend outlived that device; corrected
  Vulkan and DX12 lifecycle smoke runs exit successfully.

## Removed repeated work

Completed packets recycle their command arrays and byte storage only after GPU
completion and release of external readers. Reset drops resource owners and
replay results; tests cover stale commands and empty compute constants.

Replay preparation still commits queued edits but defers snapshot publication
to the simulation commit. This removes the second full publication scan without
delaying the rendered edit or leaving the publication suppression flag set.

## Diagnostics

Performance view, JSONL captures and evaluation include:

- `editor_play_pipeline`: active frame path.
- `simulation_ms`, `simulation_cpu_ms`: owner-worker wall/CPU time.
- `simulation_wait_ms`: completion wait after submission.
- `simulation_overlap_ms`: worker wall time overlapping submission, not a claim
  that both threads were continuously CPU-busy.
- `simulation_gpu_wait_ms`: destructive-lifecycle GPU wait.
- `pipeline_record_ms`: CPU command-packet preparation.

In this mode main/shadow submission timers measure replay. Recording and
simulation overlap/nest in existing scopes and must not be added to frame time.
`ENGINE_EDITOR_PLAY_PIPELINE=0` retains the serial path for comparison.

## Validation

- Debug Editor, focused pipeline tests, Play-session integration and grass
  render tests build successfully.
- Six focused tests pass: owner handoff/overlap/lifecycle/retirement/input;
  repeated Play/Stop identity, selection, dirty state and working undo/redo;
  grass scheduling/packet-reset regressions; sparse shadow rendering on DX11,
  DX12 and Vulkan. Pipeline and session tests also pass after the cleanup fix.
- Live editor smoke captures exercise DX11, DX12 and Vulkan with active
  pipelining; Vulkan and DX12 corrected cleanup paths exit with code 0.
- A live DX11 run completed three actual window resizes during Play and exited
  with code 0; the resized viewport continued rendering through packet reuse.
- All 20 performance-evaluator tests pass, including overlapping scope reporting.

## Measurement scope and remaining work

Grass comparison runs use Debug DX11, only the Game view, the same saved panel
layout, a stationary camera, 60 warmup frames and 180 measured frames. The
measured viewport is 534 x 420 with 3,422 resident grass instances in both paths.
These captures are not comparisons against earlier recordings at other sizes,
nor proof of equivalent gains on moving/streaming routes.

The first pipeline run was effectively tied with serial: recording allocations
consumed the overlap saving. Recycling packet allocations improved the next run
from 28.26 to 30.71 FPS. The final run includes command-record storage reuse and
removal of duplicate snapshot publication; its results are recorded below.

| Measurement | Serial baseline | Final pipeline |
|---|---:|---:|
| FPS from frame intervals | 28.26 | 32.85 |
| Mean frame interval | 35.38 ms | 30.44 ms |
| P95 interval | 43.26 ms | 35.47 ms |
| P99 interval | 46.48 ms | 42.80 ms |
| CPU processing | 34.91 ms | 30.21 ms |
| Scene preparation | 7.85 ms | 6.24 ms |
| Simulation owner work | 8.71 ms serial update | 7.96 ms worker |
| Simulation/submission overlap | none | 3.64 ms |
| Completion wait | none | 4.36 ms |
| Packet recording | included in direct rendering | 13.28 ms |

This run improved average FPS by 16.2% and P95 interval by 18.0%. Candidate
draws (157), physical camera draws (13), shadow draws (24), candidate triangles
(745,540), automatic instances (3,983) and grass instances (3,422) match. GPU
timings also vary between runs; this is one matched-layout stationary comparison,
not an isolated attribution of every saved millisecond to threading.

Captures: `.local/editor-grass-pipeline-0.jsonl` and
`.local/editor-grass-pipeline-final.jsonl`. Full evaluation and comparison:
`.local/editor-grass-pipeline-final-evaluation.md` and the adjacent JSON report.

Render-item, material and command preparation still contain substantial owner
work. The change overlaps simulation with submission; it does not complete
parallel preparation or increase grass shader throughput. Runtime-copy startup
and bounded destruction remain separate open tasks in todo.md.
