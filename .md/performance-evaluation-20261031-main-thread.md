# Main-thread performance implementation and editor evaluation

Implemented the main-thread feature list in `.md/todo.md`: centralized spatial grass scheduling, persistent/revisioned placement batches, worker visibility and record preparation, buffered record publication, GPU grass culling/LOD/indirect draws, and expanded timing/usage diagnostics. Graphics calls and live scene mutation retain their owning thread. Immutable record packing overlaps graphics setup and GPU culling; this does not introduce concurrent access to the live scene.

## Full-editor capture

Capture: `.local/performance-logs/main-thread-editor-after.jsonl`. DX11 Debug, the existing 441-cell grass scene and project quality, a stationary starting camera, one Game view at **1208 x 900**. The audit waited for actual rendered Play frames, warmed for 300 frames, and recorded 600 measured frames. It completed normally and left the editor open. Its docking layout is isolated in `.local/main-thread-profile-layout.ini`.

- Effective FPS: **83.62**, from frame intervals.
- Frame interval: **11.96 ms mean**, 15.69 ms P95, 19.80 ms P99.
- Frames exceeding the 60 FPS budget: 3.17%.

| Measurement | Mean |
|---|---:|
| Main-thread CPU, one logical core | 98.88% |
| Process CPU core equivalents | 1.17 |
| Scene update | 3.124 ms |
| Grass management | 0.460 ms |
| Physics | 1.620 ms |
| Audio coordination | 0.003 ms |
| Main view CPU submission/preparation | 3.871 ms |
| Shadow CPU submission/preparation | 1.018 ms |
| Editor UI | 1.336 ms |
| Retained in-scene UI | 0.006 ms |
| Portal/overlay work | 0.530 ms |
| Remaining main-render overhead | 0.126 ms |
| Summed worker execution wall time | 0.514 ms |
| Worker synchronization wait | 0.0076 ms |
| Record publication wait | 0.0027 ms |
| Resolved GPU stages | 2.902 ms |
| GPU shadow stage | 0.620 ms |

The scheduler selected 49 candidate fields and skipped 392 dormant fields. Fields still perform an exact local range check, so candidate count is conservative. Main rendering issued 12 GPU grass batches and shadows issued 15; indirect draw counts include calls whose GPU-selected instance count can be zero. **Grass batch placement upload bytes averaged 0 after warmup.**

The main thread remains almost fully occupied because playback is uncapped and substantial physics, editor UI, material preparation, graphics submission, and frame coordination remain serial. More FPS can keep its utilization near 100%; the goal is a shorter frame critical path rather than artificially raising total CPU utilization. Worker execution and GPU timings overlap main-thread work and must not be summed into frame time.

The earlier 21:30 capture used 1848 x 1069, a different viewport and uncontrolled camera route. This capture has about 45% fewer pixels. **Do not treat the observed FPS difference as a controlled before/after speedup.** It verifies the full-editor workload, new worker stages, GPU grass execution, small synchronization waits, and zero steady placement uploads. The opt-in trace does not contain all quality metadata found in normal Performance recordings, so no quality-normalized FPS claim is made.

## Validation and limitations

Editor and regression helpers build successfully. Five targeted CTest checks pass, covering recordings, grass/scheduling/worker behavior, streamed terrain colliders, particles, and rig mapping. Four additional custom-material/GPU-particle regression tests pass. Seven evaluator unit checks pass. DX11, DX12, and Vulkan image comparisons have zero differing channels for explicit grass instances, independent tile fades, GPU compaction, five LOD distances, and shadow receivers; existing ordinary direct/GPU batching and multiview checks also pass.

Vulkan shadow testing exposed and corrected attachment-format/render-pass compatibility problems and now preserves depth across compute dispatches. Validation still reports pre-existing optional-wireframe-feature and swapchain semaphore findings; it no longer reports the shadow pass failures. Those separate backend findings do not establish a clean Vulkan validation run.

Per-pass thread CPU estimates are excluded because Windows accounting is too coarse. Process/main-thread CPU counters sample intervals of at least 500 ms. Worker execution is summed wall time, not an OS CPU-utilization percentage. The evaluator accepts both typed Performance recordings and schema-versioned opt-in editor traces, filters warmup frames and hardware samples, and deduplicates GPU timestamps.

Generated evaluation: `.local/performance-logs/main-thread-editor-evaluation.json` and `.md`. Raw capture and editor stdout/stderr are retained under `.local`. No authored scene, quality preset, or user docking layout was changed for the audit.
