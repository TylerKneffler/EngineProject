# Shared shader preparation optimization - 2026-11-03

Implemented generic shader-source preparation sharing and detailed render-item diagnostics. **Stationary Debug DX11 editor grass improved from 64.6 FPS to 156.6 FPS at 1920 x 1080.**

## Cause and change

The instrumented foliage-only baseline spent 9.422 ms of its 9.968 ms render-item scope resolving shader paths and checking shader files. Every tile has its own material component, so sharing by material pointer would not remove the duplication. Preparation now checks each authored material location + shader path + terrain/mesh variant once per preparation frame. The per-frame set is cleared before the next scene preparation, preserving shader hot reload and direct script changes without requiring revision notifications. This applies to all meshes/materials using this path, including instanced foliage.

Added material validation, texture, shader, mesh state, bounds/publication, LOD, and construction/remainder timers to the performance view, JSONL recording, evaluator reports, and local benchmark helper. Detail timings partition the item scope; source request and unique-check counters show sharing effectiveness.

## Editor result

Same stationary grass scene, Debug DX11, 1920 x 1080, 60 warmup and 180 measured frames; pipeline enabled in both captures.

| Metric | Before | After |
| --- | ---: | ---: |
| Mean frame interval | 15.474 ms | 6.386 ms |
| P95 frame interval | 16.462 ms | 7.369 ms |
| P99 frame interval | 16.742 ms | 7.910 ms |
| Render-item preparation | 9.522 ms | 0.742 ms |

All measured editor frames stay below 10 ms. The after capture resolves one shared shader source for 84 requests each frame. Both captures have exactly 3,422 grass instances and matching view dimensions, candidate/physical draw counts, candidate triangles, automatic instance counts and shadow draw counts. No visual quality settings were reduced. After-capture writer dropped/failed records: zero.

## Isolated checks

Two repeats per case, fresh processes, fixed camera, 60 warmup + 240 measured frames per run. Averages across both runs:

| Case | Loop wall ms | Render-item preparation ms |
| --- | ---: | ---: | ---: |
| Terrain only | 4.308 | 0.161 |
| Foliage only | 6.488 | 1.151 |
| Combined | 7.568 | 1.244 |

Foliage and combined cases keep 3,586 grass instances and 88 grass tile meshes; terrain-only keeps 10 terrain meshes. All fixed measured frames have zero tile builds and zero placement upload bytes. Foliage source checks fall from 88 requests to one lookup each frame. The helper excludes editor UI and full gameplay/physics, so its loop times should not be treated as editor frame times.

Moving combined helper: 7.262 ms average, P95 15.803 ms, P99 36.953 ms, with some tile residency changes. Fixed helper runs also have occasional shadow/submission spikes. Sustained moving/streaming 100+ FPS and Release validation remain open; the stationary editor result alone does not establish those outcomes. CPU/GPU and worker scopes overlap and cannot be summed.

## Validation and remaining work

Built Editor and GrassPerformanceTests successfully. Shared shader regression passes source sharing, refreshing each frame, direct shader path edits, disabled shader entries, object disabling, and separate terrain/mesh variants. It also checks that item detail timings partition their parent. GrassPerformance and DX11/DX12/Vulkan shadow/render pixel checks all passed (4 CTests). User layout and asset index restored to their saved bytes.

Updated `.md/todo.md`: instrumentation, shared source preparation, and the stationary Debug frame-budget milestone are complete. Persistent instanced render-record caching, material/texture revision caching, remaining parallel preparation, and broader Release/moving/streaming validation remain open. The measured cause was fixed before attempting broader caching or worker changes.

Captures:
.local/performance-logs/foliage-detail-before-20261103/foliage-foliage-only.jsonl
.local/performance-logs/foliage-shader-cache-20261103-{1,2}/
.local/performance-logs/foliage-shader-cache-moving-20261103/
.local/performance-logs/editor-grass-shader-cache-1080p-20261103.jsonl

Editor evaluation: `.local/performance-logs/editor-grass-shader-cache-1080p-20261103-evaluation.md`. Tests: `.local/foliage-shader-cache-ctest.log`.
