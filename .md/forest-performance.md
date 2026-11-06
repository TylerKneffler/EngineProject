# Forest performance investigation

Target: **100+ FPS at 1920x1080**, a **10 ms frame budget**, on the current machine with an optimized build. The latest shadow-enabled moving run averages 100.26 FPS, but P95 is 12.42 ms; sustained 100+ FPS remains open.

## DX12 command, binding and shader correctness

Investigating the recurring shadow-toggle device removal with API/GPU validation revealed five invalid paths:

- Indirect and indexed draws could execute before a primitive topology was set. Both now bind triangle-list topology, matching the ordinary draw path.
- Shadow textures started in DEPTH_WRITE, while their first depth-pass barrier expected PIXEL_SHADER_RESOURCE. Creation now matches the pass contract, including both depth/stencil planes.
- The standalone Game renderer assumed an old interop overlay would transition the swap-chain buffer to PRESENT. It now records that barrier itself after composition/readback.
- Flattened inactive vertex-instancing branches accessed past a one-record (48-byte) placeholder SRV. Explicit control-flow guards avoid reads from instance records absent in ordinary draws.
- `SetTexture(nullptr)` left root arguments untouched, yielding stale or uninitialized shadow-map bindings when shadows were disabled. Each DX12 texture factory now owns a typed null SRV in its own texture heap, retained by its contexts; empty texture bindings explicitly select it without switching heaps. A disabled-shadow filtering guard alone was insufficient, so valid null bindings remain required.

The errors are captured in `.local/forest-dx12-validation.log`, `.local/forest-dx12-gpu-validation.log` and `.local/forest-dx12-shadow-guard-validation.log`. After the corrections, the complete forest repro passes with GPU-based validation (`.local/forest-dx12-null-binding-validation.log`), three consecutive ordinary runs pass (`.local/forest-dx12-correctness-repeat.log`), and the final suite passes **all 13 tests** in 203.97 s (`.local/forest-dx12-correctness-final-tests.log`). The new `OakBackend_dx12_gpu_validation` CTest case checks both CPU and asynchronously reported GPU errors through the final idle wait. It returns skip code 77 when the optional Windows DX12 debug layer/GPU-validation interface is unavailable; it passed on this machine. Normal rendering enables no debug layer.

The Release Editor and test targets build successfully (`.local/forest-dx12-correctness-final-rebuild.log`, plus the final capture-harness rebuild). The final 1920x1080 shadow-enabled Editor route exits normally with **1,200 measured frames**, **100.26 FPS**, **9.97 ms average interval**, **12.42 ms P95**, **17.48 ms P99**, and **1,200 unique resolved GPU samples**. Streaming GPU drains and pipeline queue waits remain zero. GPU means: opaque **6.78 ms**, shadows **2.16 ms**, grass **0.52 ms**, terrain **0.13 ms**; instrumented regions total **9.60 ms**, overlapping CPU work and excluding uninstrumented work.

The average is lower than the preceding 105.62 FPS capture. The rail is fixed-frame/variable-time, and short static results also varied, so this is not a matched attribution of the difference to the correctness fixes. Sustained 100+ FPS still needs work. The opaque pass remains the largest measured GPU cost. Recording: `.local/performance-logs/forest-dx12-correctness-route.jsonl`; report: `.local/forest-dx12-correctness-route-result.md`.

## Opaque isolation and full-coverage fade fast path

Controlled DX12 Release shadows-off isolation identifies leaves as the main forest color cost. At the entrance, GPU means are **5.21 ms leaves**, **1.12 ms bark**, and **5.89 ms trees together** at 1920x1080. At 960x540, leaves still cost **3.40 ms**, bark **1.01 ms**, and trees **3.94 ms**. These visibility experiments are not additive because hiding geometry changes occlusion. Resolution changes mip/depth behavior as well as pixel count; these measurements do not directly separate vertex from fragment execution. Reports and captures: `.local/forest-opaque-baseline` and `.local/forest-full-coverage-dither`.

`ApplyGeometryFade` now returns before calculating its world-space dither hash when its interval covers [0,1]. Since the hash is in [0,1), every fragment in that interval already survives. Partial LOD/caster-range fades retain the existing hash and clips. No authored asset, material, LOD distance or shadow setting changes. The isolated 1080p tree, bark, leaf and terrain screenshots are **byte-for-byte identical** before/after. Full-scene images differ between runs, so no full-scene pixel-equivalence claim is made. Comparison artifact: `.local/forest-full-coverage-dither/image-comparison.json`.

Short static GPU measurements are mixed: full-scene entrance 7.01?6.70 ms, mid 7.59?6.98 ms, overlook 9.10?9.10 ms; some isolated cases regress or vary. Do not infer a uniform gain or attribute the whole route difference to skipping the hash.

The complete shadow-enabled DX12 Release Editor route at 1920x1080 records **1,200 recurring measured frames**, **105.62 FPS**, **9.47 ms average interval**, **11.55 ms P95**, **17.41 ms P99**. All 1,200 measured GPU samples resolve: opaque **6.46 ms**, shadows **1.98 ms**, grass **0.49 ms**, terrain **0.13 ms**, summing to **9.06 ms** of instrumented GPU regions. CPU/GPU scopes overlap; this sum excludes uninstrumented work. Streaming GPU drains and pipeline queue waits remain zero. Average 100+ FPS is achieved here; sustained 100+ FPS remains open. Fixed-frame, variable-time route coverage differs between runs and prevents an exact matched speedup claim.

Recording: `.local/performance-logs/forest-full-coverage-dither-route.jsonl`; report: `.local/forest-full-coverage-dither-route-result.md`. Release build succeeds. Regression results: **11/12 pass** (`.local/forest-full-coverage-dither-tests.log`), including all three LOD tests, DX11/Vulkan forest captures, radial coverage and lifetime/buffer checks. The existing DX12 forest diagnostic again loses the device during overlook shadows-on after the shadows-off capture. It was unresolved at that stage; the subsequent DX12 correctness fixes above pass the repro and GPU validation. The complete Editor route exits normally.

## Radial directional-shadow coverage and resolved GPU timings

Directional cascades now cover concentric receiver spheres around the camera rather than a fitted viewing frustum. The receiver shader chooses bands by radial distance rather than camera-forward depth. Camera rotation and FOV changes therefore leave light projections, receiver band selection and caster coverage unchanged. The light-space texel grid snaps all three axes, with a common depth scale and a small coverage margin. Full orthographic light bounds retain off-screen casters; camera-frustum receiver silhouette culling is bypassed for these radial regions.

Staggered updates are permitted only while every retained tile still covers its whole current radial receiver band. Walking still moves coverage, and distance-band transitions/caster-range fades still depend on position; this is not a promise of identical shadow pixels through every translation or across the range boundary. Fixed LOD1 and the existing visible tree LOD distances are retained.

New tests sample receiver coverage in every direction, test upstream casters and translation on the world texel grid, and compare a fixed color receiver image while rotating the shadow camera sideways/backwards. The final fixed-receiver comparisons (threshold >5 RGB levels) changed 1/14 pixels for side/back turns on DX11, 0/3 on DX12 and 0/2 on Vulkan, out of 518,400 pixels. All **12 Release checks pass**, including real forest captures on all three backends, sphere coverage/upstream casters/texel phase, LODs, retirement and upload/buffer regressions. Log: `.local/forest-stable-shadow-final-tests.log`. This validates rotation; translated receiver images across distance-band transitions still need temporal comparison. Lens-change expectations now require unchanged shadow coverage, since radial bands do not depend on lens/aspect.

The DX12 Editor was missing the context factory's `FinalizeFrame()` call, so timestamp queries were never resolved. It now binds its actual submission command list and resolves queries before closing it; the existing frame-slot fence protects readback. The new complete DX12 Release Editor route capture at 1920x1080 contains **1,200 measured frames / 1,200 unique resolved GPU samples**:

| Measurement | Average | P95 | P99 |
|---|---:|---:|---:|
| Frame interval (**98.44 FPS**) | 10.16 ms | 13.23 ms | 18.29 ms |
| GPU opaque | 6.95 ms | 8.62 ms | 9.46 ms |
| GPU shadow | 2.13 ms | 3.13 ms | 3.53 ms |
| GPU grass | 0.53 ms | 0.87 ms | 1.09 ms |
| GPU terrain | 0.14 ms | 0.32 ms | 0.56 ms |
| Sum of measured GPU regions | 9.75 ms | 11.99 ms | 12.97 ms |

Simulation GPU drains and pipeline queue waits remain zero. Command recording averages 2.56 ms; physics averages 0.022 ms. GPU regions overlap CPU work, and their sum excludes uninstrumented GPU work. Opaque work accounts for ~71% of measured GPU time, shadows ~22%; the next experiment should isolate tree geometry and leaf overdraw within opaque rendering. Sustained 100+ FPS remains unachieved. Route coverage is variable-time/fixed-frame, so this is not an exact speedup comparison with previous runs.

Recording: `.local/performance-logs/forest-stable-shadow-route-complete.jsonl`; report: `.local/forest-stable-shadow-route-complete-result.md`. The earlier `forest-stable-shadow-route.jsonl` was interrupted when the user accidentally closed the Editor; it is incomplete and excluded from the final benchmark. The complete rerun exited normally. At that stage the earlier intermittent DX12 diagnostic device-removal issue remained tracked; the later correctness section above records the validation errors and fixes.

## Earlier streamed render-object retirement baseline

Streamed grass tile removal previously drained the graphics device through `Scene::RemoveObject`. Runtime leaves containing only the exact built-in Mesh/Material types now detach from the scene immediately, retaining the whole object (including inactive LOD geometry and material textures) in the existing fence-tagged graphics retirement queue. Scripted/custom components, physics, authored objects and subtrees retain their synchronous destruction path. Pending resources also drain safely during Clear/shutdown before handoff.

The new 1,200-frame DX12 Release Editor moving run at 1920x1080 averages **96.32 FPS / 10.38 ms**, with **12.46 ms P95** and **17.35 ms P99**. All 1,200 frames have **zero simulation GPU waits and zero pipeline queue waits**; retained pipeline frames never exceed two, and grass tile rebuilding remains active. The prior fixed-proxy run had 178 simulation GPU waits. Sustained 100+ FPS remains open. The fixed-frame, variable-time rail does not guarantee matched route coverage, so these results do not establish an exact speedup percentage.

Recording: `.local/performance-logs/forest-stream-retirement-route.jsonl`; report: `.local/forest-stream-retirement-route-result.md`. Startup (~1.81 s) and Stop (~1.65 s) remain separate nonrecurring costs. `StreamedRenderRetirementTests` covers ownership through fence handoff, inactive LODs, registry/parent detachment, synchronous lifecycle fallbacks, Clear and shutdown.

Validation limitation: the Release build and new lifecycle check pass, and 10/11 checks pass in the regression set. `OakBackend_dx12` intermittently fails with DXGI_ERROR_DEVICE_REMOVED (0x887A0005) when toggling diagnostic shadows. Two isolated repeats passed, but the subsequent full run reproduced it; neither serialized screenshot frames nor delayed image publication eliminated the issue. The harness has deferred scene retirement disabled, so this failure does not execute the new removal path, but its cause remains unresolved. Logs: `.local/forest-stream-retirement-tests.log`, `.local/forest-stream-capture-final.log`, `.local/forest-stream-retirement-final-tests.log`. No claim of complete cross-backend validation is made for this stage.

## User capture at 20:30 and shadow stability

`performance-20261105-203051-791.jsonl` records 860 Play frames at 2560x1369: **33.25 FPS**, **30.07 ms** average interval, **56.91 ms P95**, **100.62 ms P99**. Shadow packet replay averages 11.13 ms (P95 34.49), physics 9.33 ms (P95 25.50), and recording 9.38 ms (P95 11.71). These scopes overlap. Simulation GPU drains and pipeline queue waits remain zero. GPU timings are invalid on all frames; build/backend identity is not established by this capture. This is not a matched comparison with the 1080p Release rail. Detailed report: `.local/performance-logs/performance-20261105-203051-791-analysis.md`.

The user's report of shadows changing with player position/look remains an open visual correctness issue. Source inspection confirms camera-frustum-fitted cascade centers, view-depth receiver selection, and staggered cascade updates; fixed LOD1 does not remove these dependencies. They are investigation targets, not a confirmed diagnosis of the reported artifact. Validate fixed world-space receiver patches under a fixed sun and disabled wind, separating rotation from translation, then audit caster membership (including off-screen trees), cascade transitions, filter/bias scale and retained tile coverage. Existing tests establish drawing/cut/zoom behavior, not temporal pixel stability. Full atlas freezing is not a valid fix when receivers, casters or lighting leave cached coverage.

## User capture at 19:34

`.local/performance-logs/performance-20261105-193441-179.jsonl` contains 461 Play frames at **12.81 FPS**, **78.08 ms average**, **170.96 ms P95**, **370.17 ms P99**. This is substantially worse than the controlled Release results. Game/Scene targets are 2560x1369 (about 1.69 times 1080p pixels per target); 218 frames use Game only, 242 use Scene only, and one uses both. The Game-only subset still averages 11.35 FPS, so simultaneous views alone do not explain the result.

Shadow packet replay averages 36.42 ms, view recording 22.70 ms, and physics 20.47 ms with a median four substeps per frame. These overlap and must not be added. Upload record packing remains fast (0.076 ms), and tile rebuilds have a median of zero, so the previously fixed CPU upload-read/cache problems are not the dominant recurrence. GPU timestamps are unavailable throughout this recording.

A Debug build is suspected, not confirmed: VS Code previously exposed only a Debug executable launch, and the Debug binary was rebuilt at 19:32 shortly before recording. This log has no executable/build identity. Added **Release x64 (performance)** launch and its explicit Release build task, using `build/Debug/Release/Editor.exe`. Record executable/configuration/renderer provenance in future logs before attributing differences between captures to rendering changes. Full report: `.local/performance-logs/performance-20261105-193441-179-analysis.md`.

## Current fixed-proxy tree shadows

The forest now uses **one fixed shadow LOD: LOD1**, independent of camera distance and visible LOD selection. Select **Shadow geometry > Fixed LOD 1** on the terrain object layer. The authored scene and generator both set `shadowLod=1`; other layers default to distance-based shadow LODs (`-1`). Shorter prefab chains clamp to their last level, including a single-level prefab. The setting is serialized, clamped, copied through prepared-layer publication, and propagated into generated meshes.

Non-selected tree bands are excluded before shadow material preparation and submission. Selected proxies bypass GPU LOD rejection and geometry-LOD dithering. They retain leaf alpha testing but disable camera-distance texture/alpha simplification, so crossing a distance threshold cannot reshape their leaf cutouts. Fully covered proxies also skip the unnecessary distance-fade dither hash. The existing 90 m caster range retains its fade at the far boundary.

Visible tree geometry/materials, 2,048 placements, and 28/90 m visible LOD transitions are unchanged. Sun shadows retain the full 2048 atlas and four cascades. CSM tiles still refresh when coverage changes as the camera moves; a fixed proxy stabilizes the caster shape rather than freezing the atlas. Removing unnecessary atlas redraws requires reliable static-caster/light/coverage invalidation and remains future work.

| Latest DX12 Release Editor at 1920x1080 | Frames | FPS | Average interval | P95 | P99 |
|---|---:|---:|---:|---:|---:|
| Fixed proxy, entrance | 600 | 98.83 | 10.12 ms | 11.61 ms | 22.30 ms |
| Fixed proxy, moving rail | 1,200 | 90.28 | 11.08 ms | 25.87 ms | 28.86 ms |

Per refreshed cascade, tree draws dropped from 85 to 34. Stationary performance is close to 100 FPS, but **sustained 100+ FPS remains unachieved**, particularly during movement. These are recorded frame-interval measurements, not smoothed UI FPS; startup, warmup and Stop are excluded. The moving rail is fixed-frame/variable-time and samples different route portions as FPS changes, so route runs are not matched full-loop speedup comparisons. Editor GPU timestamp samples remain unavailable.

Final validation: **all 10 Release checks pass** (`.local/forest-fixed-shadow-final-tests.log`), including DX11/DX12/Vulkan forest captures, fixed-proxy selection, serialization and one/two-level fallback, camera movement/cuts/zoom, GPU LOD reuse and buffer regressions. A fresh DX12 entrance capture was inspected with visible shadows on the ground.

Recordings: `.local/performance-logs/forest-fixed-shadow-final-editor.jsonl` and `.local/performance-logs/forest-fixed-shadow-final-route.jsonl`. Reports: `.local/forest-fixed-shadow-final-editor-result.md` and `.local/forest-fixed-shadow-final-route-result.md`.

## Earlier shadow optimization stage


Full-resolution sun shadows remain enabled: 2048 atlas, four cascades, existing filtering and 90 m tree caster range. All 2,048 placements and visible tree geometry/materials remain intact; visible LOD transitions stay at 28/90 m.

| DX12 Release Editor at 1920x1080 | Frames | FPS | Average interval | P95 | P99 |
|---|---:|---:|---:|---:|---:|
| Restored shadows, before these optimizations, entrance | 600 | 83.60 | 11.96 ms | 13.55 ms | 33.70 ms |
| Optimized shadows, entrance | 600 | 94.12 | 10.62 ms | 12.53 ms | 27.33 ms |
| Optimized shadows, moving rail | 1,200 | 82.00 | 12.20 ms | 27.14 ms | 31.92 ms |

The earlier ~145 FPS results below excluded sun shadows and do not establish the shadow-enabled target. The latest measurements still do **not** meet sustained 100+ FPS. Startup, warmup and Stop are excluded from this table.

The moving rail averaged 55.44 FPS before staggered cascade updates and 82.00 FPS afterward. These are fixed-frame, variable-time runs: they cover different portions of the 18-second loop, so this is indicative rather than a matched full-route speedup. Normal moving-camera shadow draws dropped from roughly 340 to 170 per frame. The latest run still records 209 simulation GPU-wait samples out of 1,200 frames, alongside substantial tail latency. Editor GPU timestamp samples were unavailable; these FPS values come from recorded frame intervals.

Implemented shadow changes:

- Apply shadow LODs to independent prefab meshes, which previously bypassed the shadow policy. Shadow geometry switches at 8.4/54 m for this forest, with matching GPU culling and pixel-dither thresholds. The visible trees retain 28/90 m transitions.
- Use atlas-clamped texture gathers for the existing bilinear and radius-one PCF filter footprints. Skip shadow filtering when a surface has neither front-facing direct light nor subsurface scattering.
- Cull static shadow placements against transformed bounds rather than oversized spheres. Keep the original color-pass spherical envelope for wind-deformed foliage.
- Keep independent tile-cache entries for shadow and color passes. Stable entrance-frame tile rebuilds fell from about 176 to 0.11 per frame. Capture regressions reject widespread static tile rebuilds.
- During small camera movements and turns, update a dirty near cascade and rotate farther updates with at most two tiles per frame. Camera cuts, large turns, cascade-layout changes and changed projection footprints refresh all affected tiles. Preserve unmodified atlas tiles and their sampling matrices.
- Fix standalone DX12 capture timing so readback targets the color pass; reject blank diagnostic captures. Earlier shadow audit images taken between passes were blank and cannot substantiate image-equivalence claims. Earlier three-view shadow audit timings remain diagnostic only: DX12 submits shadow/color as separate renderer frames, so use full Editor captures for production FPS.

A smaller 1536 atlas and reduced filtering for distant surfaces were trialed and rejected; the final scene retains its full atlas and filter settings.

Validation: all **10 Release regression checks pass** (`.local/forest-shadow-final-tests.log`), including forest captures and LOD tests on DX11/DX12/Vulkan, static-buffer readback and upload-cache checks. Forest regressions verify bounded refresh during translation/turns, no far-cascade starvation, full refresh after a cut or zoom, visible shadow darkening, and stable tile-cache reuse. The corrected 1080p DX12 audit produced three nonblank color captures in `.local/forest-shadow-verified`; the entrance image was visually inspected.

Current recordings:

- `.local/performance-logs/forest-shadow-stagger-editor.jsonl` and `.local/forest-shadow-stagger-editor-result.md`.
- `.local/performance-logs/forest-shadow-stagger-route.jsonl` and `.local/forest-shadow-stagger-route-result.md`.
- Original restored-shadow baseline: `.local/performance-logs/forest-release-shadows-isolated.jsonl`.

## Earlier results with sun shadows disabled

DX12 Release Editor, authored forest with 2,048 trees and unchanged tree geometry/materials/LOD distances:

| Capture | Measured frames | Effective FPS | Average interval | P95 | P99 |
|---|---:|---:|---:|---:|---:|
| Full Editor Play, entrance camera | 600 | 144.70 | 6.91 ms | 8.17 ms | 13.92 ms |
| Moving camera rail with grass streaming | 1,200 | 144.54 | 6.92 ms | 15.06 ms | 17.82 ms |

These use frame intervals, not smoothed UI FPS. Startup, warmup and Stop are excluded from recurring measurements. The static capture retains the authored player controller and physics. The route is an ignored scene copy using the existing CameraTrack/SplinePath components; the test camera replaces player control and its rigid body. The route copies the authored scene; the earlier captures in this section used a disabled sun shadow setting. The moving capture samples roughly eight seconds of the continuous 18-second rail, not its entire loop.

The controlled static diagnostic uses 120 measured frames per view at 1920x1080, with shadows disabled and no readback or image creation inside measurements:

| View | Original CPU-profile FPS | After CPU mirror | After coarse groups | Final P95 |
|---|---:|---:|---:|---:|
| Entrance | 37.7 | 135.7 | 151.4 | 7.44 ms |
| Mid-forest | 35.2 | 115.7 | 139.9 | 9.19 ms |
| Elevated | 37.1 | 117.7 | 122.7 | 9.21 ms |

Separate runs can vary with system load. GPU times resolve asynchronously and overlap CPU scopes; do not add them to CPU frame times. Nested and summed worker timings also overlap. Candidate triangles count geometry before GPU visibility/LOD selection, not actual selected throughput.

## Implemented fixes

1. Prefab refresh resolves each distinct asset spelling once per check, instead of repeating path resolution for thousands of placements. The previously unmeasured asset publication, prefab preparation, commits and graphics preparation now have timers.
2. DX12 and Vulkan static vertex buffers now upload once into GPU-local memory. Previously both inherited the upload-memory fallback; DX11 already had a static-buffer implementation. Immutable geometry is fenced during initialization; editable meshes retain their writable-buffer fallback.
3. DX12 Release shaders now compile with optimization level 3. Previously shader compilation unconditionally disabled optimization in every configuration. Debug shaders retain their debugging flags.
4. Object and shadow record change detection now reads a CPU mirror instead of mapped GPU upload memory. Mirrors rotate with their owning frame/pass buffers, reset on buffer replacement, and invalidate after external writes. This avoids slow CPU reads from write-combined upload allocations. Combined record packing fell from roughly 28-33 ms to below 0.4 ms before the grouping change.
5. CPU prefab groups span 256 m instead of 64 m. The forest now generates **340 CPU mesh components**, retaining **85 shared source meshes and all 2,048 placements**. GPU culling, LOD selection and fades still operate per placement. The authored 90 m impostor transition remains in place.

The final static Editor capture averages 0.60 ms scene preparation, 0.37 ms item preparation, 1.64 ms scene rendering/recording and 0.36 ms simulation. These scopes can overlap and are not a frame-time sum.

## Validation

The Release Editor builds at `build/Debug/Release/Editor.exe` (the multi-configuration build tree is named Debug; this executable is Release).

All 10 Release CTest checks pass: UploadRecordCache, shared-prefab geometry/placement/collider editing, real oak captures/shadow comparisons on DX11/DX12/Vulkan, LOD rendering on all three backends, and DX12/Vulkan static-buffer residency/readback/editability checks. Upload-cache coverage includes initially zero records, no mapped-memory access on an unchanged record, separate buffer validity, external overwrites, buffer replacement, stride changes and concurrent disjoint records. Real oak captures verify the coarser groups still render and cast shadows.

No tree asset or material defaults were changed. Large-oak LOD1 remains 7,790-15,139 triangles; far impostors remain 28 triangles. A 55 m transition visibly flattened the elevated canopy and was rejected. `Scripts/Assets/audit_oak_lods.py` inventories all 68 oak prefabs and their materials.

## Historical stalls before streaming retirement

The earlier shadows-off moving capture has 112/1,200 frame intervals above 10 ms, and 107 simulation GPU-wait samples. Several waits exceed 10 ms while scene preparation and recording remain short. `EditorPlayPipeline::Finish` services these requests using `WaitIdle`; streamed object destruction/resource replacement is the next lifecycle path to trace. Preserve GPU references, destruction callbacks and ownership before replacing drains with retirement; do not simply suppress the waits.

Initial preparation and Stop remain separate, nonrecurring costs. The current static Release recording contains approximately 1.92 s startup and 1.65 s Stop/restore blocking work. Earlier 109.8 s Stop measurements were from Debug and are not a matched speedup comparison.

## Reproduce

Build and validate:

```powershell
cmake --build build/Debug --config Release --target Editor ForestPerformanceAudit OakBackendCapture TreeLodBackendTests UploadRecordCacheTests StreamedRenderRetirementTests StableShadowCascadeTests -- /m
ctest --test-dir build/Debug -C Release -R '^(StableShadowCascade|StreamedRenderRetirement|UploadRecordCache|TreeLod_.*|DX12StaticGeometry|VulkanStaticGeometry|TerrainSharedPrefabs|OakBackend_.*)$' --output-on-failure
```

Detailed static CPU diagnostic:

```powershell
$env:ENGINE_FOREST_AUDIT_WIDTH='1920'
$env:ENGINE_FOREST_AUDIT_CPU_PROFILE='1'
# Set to include the authored sun shadows; omit for historical shadows-off runs.
$env:ENGINE_FOREST_AUDIT_SHADOWS='1'
$env:ENGINE_FOREST_AUDIT_OUTPUT='C:\repos\EngineProject\.local\forest-cpu-coarse'
& .\build\Debug\Release\ForestPerformanceAudit.exe dx12
python Scripts/summarize_forest_performance.py .local/forest-cpu-coarse --compare .local/forest-cpu-before
```

The diagnostic defaults to 36 measured frames per case; CPU-profile mode uses 120 frames for each of the three authored camera comparisons. It keeps images in memory until all measurements finish, avoiding shader-watcher invalidation caused by artifact creation between cases. No authored scene settings are edited.

Editor captures:

```powershell
python Scripts/profile_tree_memory.py .local/performance-logs/forest-release-coarse-editor.jsonl --editor build/Debug/Release/Editor.exe --frames 600 --warmup 90 --stop-frames 3
python Scripts/Assets/build_forest_benchmark_route.py
python Scripts/profile_tree_memory.py .local/performance-logs/forest-release-route-editor.jsonl --editor build/Debug/Release/Editor.exe --scene .local/forest-benchmark-route.scene --frames 1200 --warmup 90 --stop-frames 3
```

Detailed Editor reports are beside those recordings with `-analysis.md` suffixes. CPU stage data and reference images are in `.local/forest-cpu-before`, `.local/forest-cpu-after` and `.local/forest-cpu-coarse`. Prior static geometry residency measurements are in `.local/forest-performance-before` and `.local/forest-performance`; those older Debug results do not establish optimized target acceptance.
