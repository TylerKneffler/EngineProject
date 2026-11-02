# Parallel work: DX11 resource lifecycle and submission diagnostics

This stage fixes frame-resource bookkeeping and reduces compute constant-buffer
renaming before attempting a dedicated render thread. Graphics calls retain their
current owner. The remaining parallel update/render and asset-processing work is
still tracked in `todo.md`.

## Changes

- DX11 editor rendering advances the prepared-frame serial and recycles frame
  resources for every rendered frame, independently of GPU query collection.
  Previously `PrepareFrame` only ran with GPU timing enabled. Persistent instance
  storage uses that serial for frame bookkeeping and retirement; closing the
  Performance view must not freeze it.
- Compute constants append 256-byte records to the existing DX11 constant arena,
  using `WRITE_NO_OVERWRITE` after the first map. Unsupported devices and failed
  arena maps retain the separate dynamic-buffer fallback. Shared arena generations
  invalidate cached graphics offsets across contexts after a discard or frame reset.
- Opt-in native DX11 call measurements separate camera, shadow, and other work.
  Groups cover maps, upload/unmap, selected bindings, dispatch, indirect-header
  copy dispatch, draws, buffer/compute resource creation, and buffer readback maps.
  Each group records count, total wall time, maximum call wall time, active thread
  cycles, and successful cycle samples. These are selected calls, not exhaustive
  driver coverage: texture/pipeline creation, shader compilation, Present, and
  query resolution are outside these groups. Existing Present/query timers remain.
- The Performance view exposes the toggle and compute arena/fallback counters.
  Recordings preserve these fields through immutable capture and background
  formatting, including exact 64-bit cycle values. The evaluator reports native
  groups using only instrumented frames and warns about diagnostic overhead.

Enable **Native graphics call timing** in the Performance view, or set
`ENGINE_NATIVE_CALL_TIMINGS=1` before launching. It defaults off and disabled
scopes do not query clocks/cycles. `ENGINE_DX11_COMPUTE_ARENA=0` selects the old
compute upload fallback for comparisons. DX12/Vulkan report native timing as
unsupported; their existing GPU and CPU measurements remain available.

## Validation

- Debug Editor, ProjectScripts, GrassPerformanceTests, PerformanceRecordingTests,
  MaterialShaderTests, and GpuParticleBackendsTests rebuilt.
- Focused WARP/device tests verify compute record contents/ranges, preservation of
  earlier records, fallback uploads, cross-context arena-wrap invalidation,
  native counter reset, and editor frame advancement with GPU queries disabled
  and across enable/disable changes.
- Recording tests verify native fields survive background formatting without
  losing 64-bit precision. Eleven Python evaluator regressions pass, including
  mixed enabled/disabled native diagnostics.

- The nine-test focused CTest suite passed. Shader and all three particle backend
  checks passed again after relinking against the current engine library.
- DX11 grass pixel checks passed with both arena uploads and the forced fallback:
  GPU compaction, cross-tile placement, five LOD/fade distances, and shadow
  receivers match their CPU references with zero differing channels.
- `.local/performance-logs/native-submission-stage2.jsonl` contains 240 warmed
  moving grass frames. The new counters show 57.0 compute arena writes per frame
  on average, zero fallback compute discard maps, and populated camera/shadow
  call groups. This is diagnostic verification, not a controlled FPS comparison.
- `.local/performance-logs/native-stage2-editor.jsonl` contains 120 measured Play
  frames following 120 warmup Play frames, with monotonically advancing frame
  serials across 270 editor frames. All 276 accepted writer records were written;
  no records were dropped or failed. Both traces have generated evaluator reports
  beside them; the editor trace ends with `capture_complete`.

The broader buffer-reuse stall TODO remains open. Fewer discard maps do not prove
that the previously observed long submissions were GPU synchronization; native
call timings and thread cycles narrow the next investigation. No FPS improvement
is claimed from an instrumented diagnostic capture.
