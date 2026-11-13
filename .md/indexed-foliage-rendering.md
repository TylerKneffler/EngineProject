# Indexed foliage milestone — 2026-11-12

Target remains 120 FPS at 1920x1080 (8.33 ms). This change reduces shared mesh
storage and vertex work without reducing foliage density, triangle count,
distance, resolution, material quality, or the existing LOD policy.

## Implementation

- Terrain prefab prototypes retain authored indices. Triangle-list assets are
  indexed once on a geometry worker using exact full-vertex equality. UV seams,
  normals, tangents, colors and auxiliary attributes remain distinct. Conversion
  is skipped if the combined vertex/index storage would not shrink.
- Geometry remains shared across tiles and streamed cells. Instance ranges use
  index counts for indexed sources and vertex counts otherwise. Existing native,
  indirect, shadow and selection submission paths already support both formats.
- Index hashing/copy preparation runs on the worker. Index resource publication
  stays on the owner thread, respects the shared upload allowance, and completes
  before a source becomes ready. Vertex uploads retain the existing worker path.
- Standalone Game uses `PrepareRenderFrame(false)`: simulation Update already
  publishes its scene snapshot, while rendering consumes a separate owned native
  packet. This avoids two additional full-scene snapshot scans during preparation.
- Periodic prefab checks reuse the worker-resolved source metadata instead of
  repeating the same filesystem lookup on the owner thread.
- The capture includes preparation stages, instance reuse, pending prefab layers,
  and unique shared-source vertex/index counts and logical bytes.

## Measurement

Standalone Game, Release, RTX 4090, DX12, 1920x1080, native render scale, uncapped,
expanded oak meadow, unchanged starting camera. Each run uses 1,200 warm-up frames
and 240 measured frames. All 240 frames are valid. Both runs retain 3,383 objects,
9 terrain components, 8 streamed prefab cells, 177 submitted draws and 79,400,310
CPU candidate triangles. Candidate triangles are not executed GPU triangles.

| Median | Before | After |
| --- | ---: | ---: |
| Frame interval | 27.86 ms | 16.74 ms |
| Scene preparation | 5.95 ms | 3.46 ms |
| Command recording | 7.95 ms | 6.69 ms |
| GPU opaque pass | 23.60 ms | 13.37 ms |
| GPU shadows | 2.74 ms | 1.56 ms |
| Frame interval p95 | 31.63 ms | 20.13 ms |

Frame time decreased about 40% (approximately 36 to 60 FPS). Shared source data
dropped from 739,788 vertices / 195,304,032 bytes to 247,742 vertices plus 736,728
indices / 68,350,800 bytes, about 65% less logical geometry storage. This does not
measure total VRAM: caches, mirrors and frame resources also consume memory.

The intermediate instrumented run found that the oak inputs were nonindexed;
merely retaining authored indices did not change their geometry. That run supplies
the before geometry counts. It also recorded 1.97 ms in preparation commits,
versus 0.004 ms after eliminating the extra snapshot scans. Final DX12 frames
reuse all 3,353 instanced render items and update none; draw/object/geometry counts
stay constant. Pending source refreshes can briefly report pending layers even
after all geometry is resident.

The final build enables additional benchmark profiling. The original baseline
did not have those detailed counters. These are observed whole-change timings,
not isolated per-optimization speedups; live wind phases and scheduling also vary.
CPU/GPU stages overlap and must not be summed.

DX11 and Vulkan each also pass 240/240 expanded-scene capture frames with matching
geometry/residency counts. DX11 median/p95 is 14.35/51.20 ms; Vulkan is 17.82/20.01
ms. DX11's high tail remains unresolved; these backend runs are compatibility
checks, not before/after comparisons. Captures preserve the forest and terrain;
grass wind phase differs between runs.

Evidence is in `.local/indexed-foliage-validation.json` and the
`.local/indexed-baseline`, `.local/indexed-after`, and `.local/indexed-final-*`
capture directories. Reproduction commands are in `Engine/Game/Benchmark/README.md`.

## Validation and remaining work

`IndexedInstanceGeometry`, `DrawBatchPreparation`, and `InstanceTilePreparation`
pass in Release. Indexed tests cover exact triangle reconstruction, UV seam
preservation, the no-savings fallback, shared index ownership, indexed instance
range validation, failed index publication, and nonindexed publication.

The Debug Editor builds successfully, and `IndexedInstanceGeometry` also passes
in Debug. A Release DX12 walking capture passes 300/300 frames and moves 40.44 m
across the terrain seam at z=180 (z=170 to z=210.32), retaining all eight streamed
neighbors. This functional run overlapped the Editor build, so its timings are
not used for performance comparisons. Evidence: `.local/indexed-seam`.

This is the indexed-instance and preparation milestone, not a Nanite system or a
triangle simplifier. The 120 FPS target remains open: the opaque pass alone is
still above budget. Next work should reduce canopy geometry and overdraw with
better middle/far LOD assets and distant representations, then measure whether
more granular GPU visibility selection is needed.
