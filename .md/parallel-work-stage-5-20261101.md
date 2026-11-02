# Parallel work: retained spatial hierarchy and dirty refits

The generic render/spatial-script index now retains its partition when item count
and bounded/unbounded classification remain stable. Changed bounds update only
affected leaves and ancestors instead of sorting and rebuilding every object.
Camera/shadow visibility and point-query results retain their conservative bounds
semantics across DX11, DX12, and Vulkan.

## Changes

- Track item-to-leaf membership, parent/depth links, and generation-marked dirty
  nodes. Repeated changes in one leaf or shared ancestors are processed once.
- Refit bottom-up: small levels stay serial; levels with at least 256 dirty nodes
  can use the engine scheduler. Jobs read owned bounds and update disjoint nodes;
  each level joins before parents are processed and owner queries resume.
- Unchanged inputs retain the tree and cached views without scheduling jobs.
  Unbounded records remain visible and do not require leaf refitting.
- Rebuild on item-count or bounded-classification changes. A tree area cost
  exceeding twice the last build baseline triggers a quality rebuild immediately,
  including a single teleport followed by unchanged frames.
- Invalidate visibility caches when bounds change. Interrupted updates remain
  unpublished; conservative queries and a full rebuild recover the index.
- Keep index implementation out of scene headers, limiting worker-template code
  expansion in large scene translation units.
- Record compare/refit/rebuild wall times, changed/reused bound records, dirty
  leaves, refitted nodes, rebuilds and quality rebuilds. The Performance view and
  evaluator show these counters and explain their nested timing scopes.

## CPU benchmark

`RenderSpatialIndexTests --benchmark` runs eight warmup and 60 measured updates
per case in one Debug process. Input copies occur before the timed update.
The forced-rebuild reference uses the same current node format and retained
allocations, rather than claiming to reproduce an older executable exactly.
Static inputs skip rebuilding in every mode. These are isolated CPU index costs,
not engine FPS.

| Items | Update | Full rebuild ms | Serial refit ms | Adaptive refit ms |
|---:|---|---:|---:|---:|
| 1,024 | static | 0.019 | 0.019 | 0.019 |
| 1,024 | sparse | 1.675 | 0.021 | 0.021 |
| 1,024 | dense | 1.772 | 0.095 | 0.096 |
| 16,384 | static | 0.331 | 0.333 | 0.333 |
| 16,384 | sparse | 34.048 | 0.333 | 0.331 |
| 16,384 | dense | 55.892 | 3.284 | 2.587 |

Sparse means one translated bound; dense means translating every bound. The
large gains come from avoiding repartitioning. Worker refits provide an additional
benefit for large dirty levels; small refits intentionally remain serial.

## Validation

- Deterministic sparse/dense tests compare serial and parallel updates with fresh
  rebuilds, independent corner/plane frustum tests, and brute-force XZ candidates.
- Cover twelve cached views, unchanged inputs, inserts/removals, bounded-state
  changes, quality rebuilds, empty and unbounded-only indexes, and clip boundaries.
- Nine focused CTests pass: RenderSpatialIndex, GrassPerformance, HumanoidRigMapping,
  TerrainColliderStreaming, ParticleEmitterRegression, AssetPreviewAsync, and GPU
  particle regressions across DX11, DX12, and Vulkan. Thirteen evaluator tests pass.
- Debug Editor, ProjectScripts and Game are rebuilt.

## Full-scene recordings

The 441-cell grass diagnostic completed 240 measured frames per route:

| Route | Index average ms | Refitted nodes, total | Full rebuilds | Quality rebuilds |
|---|---:|---:|---:|---:|
| resident | 0.202 | 124 | 44 | 0 |
| streaming | 0.847 | 1463 | 193 | 0 |

Captures and evaluator reports are beside `.local/performance-logs/parallel-stage5-{resident,streaming}.jsonl`. These runs confirm refits are used, while changing
draw-list membership still dominates index cost during streaming. Persistent
draw-slot spatial entries are now tracked as the next separate TODO. These
diagnostics do not establish the index as the sole cause of the reported FPS.

The full-editor capture `.local/performance-logs/parallel-stage5-editor.jsonl`
contains 120 measured Play frames after 120 warmup frames, with the new spatial
fields. All 276 accepted records were written with zero drops/failures and a
`capture_complete` marker. Spatial regressions also pass with
`ENGINE_WORKER_COUNT=0`. Grass compaction, LOD/fades and shadow-receiver pixel
checks match CPU references across DX11/DX12/Vulkan; ordinary automatic mesh
instancing/edit-isolation image checks also pass on DX11.

## Remaining scope

Full topology rebuilding still runs on its owner for membership/quality changes.
Static render-item caching, visibility/sorting preparation, full scene snapshots,
and simulation/render thread ownership remain separate TODOs.
