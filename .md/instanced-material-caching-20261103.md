# Instanced render records and material caching - 2026-11-03

Implemented the next foliage performance TODOs and rebuilt Editor.

## Changes

- Rigid instanced meshes reuse prepared render records instead of rebuilding world matrices and bounds each frame. Geometry, placement revisions, source geometry and transform changes invalidate the record. Current material flags, fades, selection and GPU LOD parameters are refreshed on reuse. CPU LOD transitions, deformation, previews and portal/warp paths retain full preparation.
- Material validation caches its normalized scalar/vector/UV inputs. Public script edits are detected even without configuration revision notifications. Shader blend validation and alpha/specular normalization remain supported.
- Texture preparation reuses resident texture/provider state and avoids repeated shared-owner copies. Pending async loads, invalidation, texture replacement/removal and provider changes still enter preparation.
- Prepared transform revision reads avoid walking parent chains during the stable render phase. Outside that phase, lazy validation remains available.
- Added instanced record reuse/rebuild counters to performance view, recordings, benchmark traces and evaluator input.

## Final editor capture

Debug DX11, fixed 1920 x 1080 Game view, Play pipeline enabled, stationary camera, 60 warmup + 180 measured frames.

| Metric | Final result |
| --- | ---: |
| Average FPS | 162.01 |
| Mean frame interval | 6.172 ms |
| Frame interval P50 | 6.091 ms |
| Frame interval P95 | 7.117 ms |
| Frame interval P99 | 8.018 ms |
| Render-item preparation | 0.681 ms |
| Material validation | 0.043 ms |
| Reused / rebuilt foliage records per measured frame | 84 / 0 |

All measured editor frames remain below 10 ms. Grass instance count is 3,422; candidate draws 45, physical draws 14, candidate triangles 816,434, automatic instances 4,611 and shadow draws 24 match the prior quality/count baseline. No lost/failed profiler records.

The previously documented shader-only capture was 156.6 FPS with 0.74 ms item preparation; this stage is an incremental improvement, much smaller than the initial shader-source fix. The intermediate cache build captured 163.7 FPS / 0.66 ms. Those small differences are within cross-run variability and do not establish an isolated gain from the prepared-revision accessor. Earlier .local baselines were deleted during user cleanup, so this is not a paired A/B measurement of each cache individually.

## Isolated and moving checks

Two fixed repeats each for foliage-only and combined rendering, plus one moving combined run, are retained in the capture directory. Fixed runs verify 88 reused foliage records, zero rebuilt records, zero tile builds and zero placement uploads on every measured frame. Foliage-only has no visible terrain meshes; combined has 10. These direct-renderer runs were variable and do not demonstrate a reliable additional FPS gain from this stage. They omit editor UI and the full gameplay/physics loop and should not be compared directly with editor FPS. Moving/streaming and shadow/submission spikes remain open.

## Verification

- Built Editor and GrassPerformanceTests.
- Focused instanced regression: source/placement edits, transforms, visibility fades, material edits, resident GPU LOD settings, CPU LOD transition fallback, disable/re-enable and removal.
- Focused material regression: direct field edits, dependent limits, shader-only edits, asynchronous texture publication, resident reuse, invalidation, removal and provider switching.
- Prepared transform revisions match lazy validation, including non-batched fallback; existing hierarchy edit/reparent tests pass.
- GrassPerformance and DX11, DX12 and Vulkan shadow/render pixel regressions all pass (4 CTests, 66.20 seconds). New regressions run in the default GrassPerformance suite.

Current editor layout and asset index were held in memory and restored after final verification. Following user cleanup of .local, captures were moved under `build/Debug/foliage-cache-verification-20261103/` to retain them.

## Remaining TODOs

GPU-side update auditing, remaining parallel record preparation and broader Debug/Release stationary/moving/streaming validation remain in `.md/todo.md`. The new instanced cache covers rigid single-record instances; complex/deformed/multi-record paths continue through full preparation.

Detailed capture and evaluator: `build/Debug/foliage-cache-verification-20261103/editor-prepared-revision-1080p.jsonl` and `editor-evaluation.md`. Final tests: `ctest-final.log` in the same directory.
