# Engine TODO

## Debug editor performance

Target: responsive editing and playback in Debug with assertions enabled. Address repeated CPU work and shader compilation policy before reducing scene quality, draw distance, density, or resolution.

Audit source: `.local/performance-logs/performance-20261111-174034-921.jsonl` (2026-11-11, DX12, oak meadow).

Representative steady periods, median milliseconds:

| Measurement | Scene view, editing | Scene view, playing | Game view, playing |
| --- | ---: | ---: | ---: |
| Frame work (`frame_ms`) | 69.6 | 63.1 | 42.8 |
| Editor UI | 33.1 | 32.9 | 1.4 |
| Scene preparation | 12.3 | 10.6 | 10.6 |
| Instrumented GPU geometry passes | 25.9 | 24.1 | 27.0 |

Samples: frames 753–930, 940–1069, and 1100–1499 respectively. These scopes overlap; do not add their timings. Different views and camera positions prevent treating these observations as a controlled comparison of rendering cost.

### 1. Cache Scene toolbar LOD summaries

- [x] Instrument global toolbar sections and LOD summary construction. Frame records include total/section timings, draw counts, LOD summary timing/calls, and remaining unattributed UI time.
- [x] Capture an initial trace with the Scene View toolbar active. In 24 editing frames, median/p95 were 1.86/2.56 ms for editor UI, 0.22/0.36 ms for the toolbar, and 0.073/0.099 ms for the LOD summary (`.local/performance-logs/toolbar-audit-scene.jsonl`). This short startup capture was stopped after 40 seconds and is not a controlled comparison with the original log; the old scan's cost was not measured before removal.
- [x] Remove the placement scan from the toolbar's LOD summary. Placement-only assets contribute one LOD, so the maximum can be derived from current profiles and overrides; stop scanning scene components after reaching the three-level display limit. The bundled grass prefab has 324,147 placements referencing only two distinct assets.
- [x] Assess whether the remaining profile and scene/component scans warrant a cache. The initial Scene-focused capture measured 0.073 ms median and 0.099 ms p95 for the entire LOD summary, so retain the current direct summary until a representative workload shows a need for caching.
- [ ] Compare toolbar and total UI time with the same Debug scene and camera before and after the change.

Code: `Engine/Editor/Core/View/Templates/EditModes/EditToolsViewTemplate.cpp`, `Engine/Core/Compoonents/Terrain/TerrainObjectLayer.cpp`.

### 2. Avoid repeated prefab validation and preparation

- [x] Instrument `prepare_prefab_instances_ms` with registry collection, validation, LOD profiles, source-set construction/lookup, build work, and main-thread static-buffer creation. Validation contains its signature/profile/source subphases; build contains main-thread buffer creation. Asynchronous worker execution is not included in buffer creation time.
- [x] Capture frames with the new breakdown. In `.local/performance-logs/prefab-preparation-warm.jsonl`, the final 50 frames after a 240-frame warmup have a 0.96 ms median and 1.23 ms p95 for prefab preparation. Median validation is 0.95 ms, including 0.23 ms for LOD profiles, 0.42 ms for source lookup (including 0.27 ms for source-set construction), and 0.20 ms for source probes; registry collection is 0.007 ms. Build and main-thread buffer creation are zero throughout those final 50 frames. Earlier frames in the same capture still have build work, so this is a warmed result and does not establish the cause of the earlier log's 6.6–6.8 ms median. The automated process was stopped after 55 seconds, after all 120 measured frames had been recorded.
- [x] Cache distinct placement asset names by the existing placement signature and reuse them when constructing LOD definitions and source sets. Add profiler fields for placement-asset collection, LOD profile construction, and source-set construction. The next controlled trace must establish the performance effect.
- [x] Cache resolved LOD profiles and distinct source sets. Invalidate on placement-signature, owner, or component revision changes; refresh at least every 100 ms during preparation to catch direct field edits that bypass revision tracking. Keep source-file/relocation probes on their existing schedule.
- [x] Measure the cache using the same 240-frame warmup and 120-frame Debug meadow capture. Across the final 50 measured frames, prefab preparation median/p95 changed from 0.963/1.226 ms to 0.471/1.065 ms, and LOD profile construction median changed from 0.232 ms to 0.009 ms. Overall frame-time medians were 34.73 ms before and 36.08 ms after; do not infer an overall FPS gain from these runs. Logs: `.local/performance-logs/prefab-preparation-warm.jsonl` and `.local/performance-logs/prefab-cache-warm.jsonl`.
- [x] Check prefab profile edits, untracked direct edits, source relocation/deletion/reappearance, placement edits, and identical serialized restore with the current build. Updated `TerrainSharedPrefabs` for current mesh fields and asynchronous preparation; it passes in Debug. Fixed authored LOD-chain precedence over fallback placement profiles, source-cache refresh bookkeeping, and cleanup when a prefab source disappears.
- [x] Check a terrain cell unload/reload in the `TerrainSharedPrefabs` Debug regression. The test prepares an authored two-level prefab, unloads its cell, reloads it, and verifies the placement and both LOD bands are restored.
- [x] Run an editor play/stop smoke capture. `.local/performance-logs/prefab-play-stop.jsonl` completed successfully and recorded 30 stopped frames after restore.
- [x] Observe the restored editor scene through prefab preparation. In `.local/performance-logs/prefab-play-stop-long.jsonl`, a 900-frame post-stop capture completed cleanly. The first 50-frame stretch with zero prefab build time and a constant 614 candidate draws began at post-stop frame 339 (approximately 42.5 seconds of update intervals after the first stopped frame). The last 100 frames held at 614 candidate draws and 42,608 candidate instances. The capture demonstrates eventual settling; it does not establish an exact visual or object-state match with the pre-play scene, which was still building when playback started.
- [ ] Compare the fully settled restored scene with a fully settled pre-play baseline, including authored placements, LOD selection, and rendered output. Investigate the approximately 339-frame post-stop rebuild as a separate responsiveness issue.
- [x] Retain existing shared prefab prototypes and prepared buffers across trusted Play restore until restored layers finish preparation; normal scene clears release retention. In the matching 900-frame Debug capture (`.local/performance-logs/prefab-play-stop-retained.jsonl`), 49 prototype references were retained and subsequently released. The first stable 50-frame stretch began at post-stop frame 277 (39.22 seconds), versus frame 339 (42.53 seconds) in the earlier capture; both ended with 614 candidate draws and 42,608 candidate instances. The first 200 post-stop frames had a 54.39 ms median frame time versus 36.46 ms previously, so investigate the frame-time tradeoff before treating this as an unqualified performance improvement.
- [x] Allow prefab preparation to publish additional parts and groups while the shared 3 ms budget remains, while making progress if one part exceeds it and retaining the existing upload allowance. A budget-only capture settled at frame 339 (42.73 seconds), so it did not materially reduce the restore tail in this scene.
- [ ] Bound refresh work across frames. Include registry/validation overhead in the preparation budget and split work that can exceed the 3 ms budget inside one call. Report unavoidable upload overruns separately.
- [ ] Compare steady preparation time and check updates after edits, streaming, and play/stop. In the Debug DX12 Oak recording `.local/performance-logs/performance-20261112-115316-338.jsonl`, the last 50 Game-play frames still contain prefab build work in 45 frames and have 13 distinct camera matrices; they are not a settled fixed-camera comparison. Prefab preparation median/p95 is 10.55/18.23 ms and validation is 4.85/11.76 ms for that interval.

Code: `Engine/Core/Scene/Rendering/SceneRenderer/SceneFramePreparation.cpp`, `Engine/Core/Compoonents/Terrain/TerrainPrefabInstances.cpp`.

### 3. Optimize GPU shaders while retaining application Debug behavior

- [x] Decouple shader optimization from application `_DEBUG`. DX12 explicitly requests `D3DCOMPILE_SKIP_OPTIMIZATION` in Debug; DX11 has the same policy.
- [x] Compile optimized shaders with debug symbols by default, retaining application Debug symbols and assertions. Provide an explicit option for unoptimized shader stepping.
- [x] Record effective D3D shader optimization/debug settings in performance provenance. DX11/DX12 compile from source and have no persistent shader bytecode cache requiring a cache-key update.
- [ ] Compare GPU pass timings and visual correctness using the same Debug application configuration, scene, camera, and resolution. The existing log does not establish the gain.

Code: `Engine/Core/Renderers/DX12/DX12ShaderCompiler.cpp`, `Engine/Core/Renderers/DX11/DX11ShaderCompiler.cpp`.

### 4. Reduce render command recording cost

- [x] Profile unchanged-state work within command recording. In the stationary normal Oak standalone Game capture (Release DX12, RTX A2000 Laptop GPU, 1920x1080; 300 warmup and 180 measured frames), command recording median/p95 was 5.367/7.870 ms, including material 1.262/2.001 ms and batching 0.461/0.723 ms. Material pipeline resolution was 0.057/0.107 ms, geometry binding 0.028/0.052 ms, and other material work 1.173/1.852 ms; batch snapshot, build, and wait medians were 0.254, 0.151, and 0.187 ms. All measured frames reported a batch cache hit with 504 reused keys and a shader pipeline cache hit with zero misses. These stages are nested or may overlap; do not sum them. Evidence: `.local/performance-logs/oak-standalone/oak_meadow-game-dx12-20261112-111543-051/frames.jsonl`. The earlier 19.3/4.1/2.6 ms measurements were from a different workload and are not a matched comparison.
- [x] Reuse unchanged draw/material preparation and batch metadata. Exact unchanged batch inputs reuse grouped membership; Game batching skips editor-only selection lookup; view inverse, direction and view-projection are prepared once per view instead of per draw. A focused 504-input CPU test measured 0.015 ms reuse versus 0.045 ms forced regrouping. A normal Oak Release DX12 capture measured batch build median/p95 0.092/0.176 ms versus 0.151/0.243 ms in the earlier capture, but variable CPU/GPU timings and an intervening noisy run limit the comparison. Further lifetime and multi-view validation remains a separate item. Capture: `.local/performance-logs/oak-standalone/oak_meadow-game-dx12-20261112-113312-447/frames.jsonl`.
- [ ] Preserve command/resource lifetimes and invalidation for material edits, visibility, transforms, streaming, and multiple views. Batch preparation now retires absent slots and compatibility keys; focused tests cover key/texture/pipeline changes, visibility-style removal and re-entry, slot churn, alternating view inputs, packet resource ownership, transform changes, old tile-record owners, and separate shadow/color domains. Release DX12 clustered and unclustered Game checks pass, as does a 60-frame normal Oak moving-camera capture (5.59 units displacement; `.local/performance-logs/oak-lifetime/oak_meadow-game-dx12-20261112-114629-957/frames.jsonl`). Live scene material edits, actual streaming lifecycle, and simultaneous Editor/Game or portal view transitions still need end-to-end verification.
- [ ] Investigate view-switch recording spikes separately: some transitions take approximately 0.3–0.6 seconds. The Debug Oak recording `.local/performance-logs/performance-20261112-115316-338.jsonl` captures an Edit-mode Scene-to-Game transition: frames 405/406 take 368.42/285.50 ms, with color render-upload scopes of 148.35/154.71 ms; frame 405 also reports 132.62 ms shadow-draw time, 32 cold shadow batches and 96 shadow-instance allocations. These scopes must not be summed, and native call timing is unsupported; the underlying resource operations are not identified. `pipeline_record_ms` is zero because these frames are not using the Play recording pipeline. Editor traces now flag rendered-view changes and report per-view setup/shadow/color recording, native replay wall time, panel resize work, and material pipeline timings/cache counters; existing native-call timing remains unsupported on DX12. The opt-in `ENGINE_EDITOR_AUTO_PROFILE_SWITCH_VIEWS` reproducibly exits with 0xC0000409 in two Release DX12 normal Oak runs, while the no-switch control and a control with the switch delayed both complete. Both switch traces end before a view change is recorded, so the original spike remains unattributed. Investigate the automated failure and obtain a complete transition capture. Evidence: `.local/performance-logs/editor-view-switch-dx12.jsonl`, `.local/performance-logs/editor-view-switch-repeat-dx12.jsonl`, and `.local/performance-logs/editor-view-control-dx12.jsonl`.

Code: `Engine/Editor/EditorApplication.cpp`, `Engine/Core/Scene/Rendering/SceneRendering.cpp`, `Engine/Core/Scene/Rendering/SceneRenderer/SceneInstanceBatching.cpp`.

### 5. Measure foliage GPU workload before choosing geometry changes

- [ ] Capture a controlled Debug trace with `ENGINE_GPU_WORKLOAD_COUNTERS=1`. Every audited frame has `gpu_workload_valid=false`; leaf pass timing alone cannot distinguish geometry cost, overdraw, and shader complexity. The 2026-11-12 user recording also has zero valid workload samples across all 655 detail frames.
- [ ] Distinguish candidate instances/triangles from GPU-selected instances and pipeline invocation counts in profiler labels and logs. The approximately 29 million reported submitted triangles include pre-cull candidates on the indirect path; they are not a measured rasterized triangle count.
- [ ] Measure LOD transition duplication, shadow work, and foliage shader cost with a fixed camera. Instance counters aggregate draws and must not be interpreted as unique tree counts.
- [ ] Use those results to prioritize LOD transitions/assets, foliage shaders, shadow proxies, or meshlet/cluster culling. Establish the expected benefit before committing to a Nanite-like hierarchy.

Code: `Engine/Core/Scene/Rendering/SceneRendering.cpp`, `Engine/Core/Graphics/IGraphicsContext.h`, `Engine/Core/Renderers/DX12/DX12GraphicsContext.cpp`.

### Measurement and responsiveness follow-up

- [ ] Compare changes in Debug with fixed scene/camera/view layout and report median and tail frame times. Separate scene load, play start/stop, view switches, and steady frames.
- [ ] Keep `dt_ms` (update interval), `frame_ms` (current frame work), idle wait, and completed GPU sample scope explicit. The post-stop editor intentionally waits approximately 50 ms while idle; this is not rendering work. `prepare_ms` can include UI/render entry overhead; use `scene_prepare_ms` for scene preparation comparisons.
- [ ] Treat `GetThreadTimes` CPU measurements as coarse: this trace contains 15.625 ms increments. Zero CPU time and wall-minus-CPU do not establish precise per-frame stall time.
- [ ] Investigate load/restore responsiveness separately. Frame 490 spends approximately 14.27 seconds in pending load; stopping play includes approximately 2.75 seconds of restore work followed by another approximately 3.75-second frame. The following frame's `dt_ms` reflects the preceding stall.
- [ ] Set acceptance targets after the first controlled baseline. Release builds, reduced resolution, and shorter view distances do not substitute for fixing Debug overhead.
