# Parallel work: copied draw-batch preparation and compatibility reuse

Local-stage report preserved under a distinct name during the 2026-11-02 merge because upstream also used the stage-7 filename. The combined code integrates this cache with the newer native render-packet and parallel-shadow paths. Measurements below describe the pre-merge local stage.

Compatible automatic draw groups now prepare from owned identity snapshots,
rather than traversing live render components on a worker. Scene rendering uses
separate camera and shadow caches. This applies to eligible ordinary meshes,
prefab copies and instanced grass, across the graphics backends.

## Changes

- Copy the existing full compatibility key on the owner: geometry/index/pipeline
  identities, all 30 texture identities, stride/counts, light count, render and
  shadow flags, LOD, species ranges, indirect LOD meshes and isolation owner.
  Existing exclusions for blended, selected, preview, custom-shader, deformed,
  portal and other incompatible draws stay in the owner eligibility pass.
- Cache each persistent object's compatibility assignment independently of the
  active view set. Reordering, temporary absence and reactivation preserve that
  assignment. New or changed keys use the lookup; unchanged keys reuse a shared
  identity. Reference counts retire old key variants when their last slot changes.
- Preserve the prior first-seen group order and object-slot-sorted group members
  when mapping results to current prepared-draw indices. Frame-list positions
  are never retained as valid output across views.
- At least 256 eligible draws can prepare on one scheduler worker while the
  owner computes draw statistics. Smaller sets run directly. Worker-count zero
  and disabled parallel loops use the owner path. A future fence joins on normal
  completion and exception unwinding before cache/scene lifetime can end.
- Workers touch copied keys and private CPU cache state only. They do not call
  mesh, material, scene, graphics-resource or submission APIs. The owner assigns
  result groups and continues submission after the result is joined.
- Add snapshot, grouping, synchronization, eligible-draw, cache-hit and worker-job
  measurements to render metrics, recordings, Performance view and evaluation.
  Reused-key counts show partial reuse during streaming; cache hits count views
  whose active keys all reused their assignments, not individual groups.

## CPU experiment

`DrawBatchPreparationTests --benchmark` compares a serial map-based grouping
reference with cached owner and cached worker execution in one Debug process.
Inputs rotate source order every frame while keeping slot compatibility fixed.
There are 21 compatibility keys, eight warmup updates and 64 measured updates.
Input construction and copying are outside the timer. Grouping, result remapping
and (for the worker case) dispatch/wait are included. This is a CPU grouping
experiment, not an engine FPS benchmark or an exact old-executable comparison.

| Eligible draws | Serial map ms | Cached owner ms | Cached worker ms |
|---:|---:|---:|---:|
| 512 | 1.496 | 0.449 | 0.489 |
| 4,096 | 10.722 | 3.242 | 3.344 |

Every measured cached update reused assignments. The final cache retains
assignments per slot: a whole-view cache proved ineffective for streaming
membership changes. Active groups sort only small member indices. Worker
dispatch carries overhead: it is useful for moving work off
the owner and overlapping independent owner work, not inherently faster than
running the cached algorithm directly.

Artifact: `.local/performance-logs/parallel-stage7-batch-benchmark.jsonl`.

## Validation

- Debug Editor, Game, ProjectScripts and focused helpers build.
- Ten focused CTests pass: HumanoidRigMapping, ParticleEmitterRegression,
  GPU particle regressions on DX11/DX12/Vulkan, GrassPerformance,
  TerrainColliderStreaming, AssetPreviewAsync, RenderSpatialIndex and
  DrawBatchPreparation. Fourteen evaluator tests pass.
- Independent serial grouping reference checks group order and slot-sorted
  members. Tests cover draw reordering with gaps in source indices, every key
  field including all texture slots and LOD mesh slots, slot identity changes,
  removal/addition, separate owners, empty views, worker-owned snapshots and
  reuse. The regression also passes with `ENGINE_WORKER_COUNT=0`.
- Final DX11/DX12/Vulkan grass image checks match CPU references with zero
  differing channels for instancing, compaction, LOD fades and shadow receivers,
  including gaps in active persistent slots. Ordinary automatic instancing,
  visibility/LOD, dirty uploads and edit isolation pass on DX11.

## Full grass-scene capture

The final 441-cell Debug capture finished loading and ran 240 measured moving
frames per route. Recording/evaluation artifacts are
`.local/performance-logs/parallel-stage7-{resident,streaming}*.{jsonl,json,md}`.

| Route | Eligible keys/frame | Reused keys/frame | Key reuse | Worker frames | Snapshot ms | Grouping ms | Wait ms | Total batching ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| resident | 173.7 | 173.2 | 99.7% | 0 / 240 | 0.794 | 0.360 | 0.000 | 1.227 |
| streaming | 229.4 | 226.5 | 98.7% | 69 / 240 | 1.000 | 0.467 | 0.153 | 1.587 |

Resident views had fewer than 256 eligible draws and intentionally stayed on the
owner. Streaming views peaked at 272 eligible draws and used workers above the
threshold. Grouping time overlaps the owner's synchronization waits when jobs
run; do not add these columns. Total batching includes draw-statistics work
performed while grouping can run, plus applying the result.

The diagnostic reported CPU-frame averages of 22.15 ms resident and 34.67 ms
streaming. This capture does not establish an overall FPS improvement. Separate
prior captures ran substantially faster across other stages as well, including
snapshot encoding (about 54 ms previously versus 139?149 ms here), so they do not
isolate this change. The controlled grouping experiment above supports the
compatibility-cache benefit; it does not include owner-side key gathering.

Owner key gathering remains more costly than grouping in these captures. Static
render-item and compatibility-snapshot caching are the next performance targets.
The full capture now measures the owner, grouping, wait and reused-key costs
separately so this limit is visible instead of hidden inside a batching total.

## Remaining scope

Owner-side eligibility checks and compatibility snapshots still read live
components and resources. Render-item/material snapshots, general component
parallelism, simulation/render-thread ownership and immutable scene handoff
remain separate TODOs. Cache storage retains the most recent compatibility assignment for each seen
persistent slot in the camera/shadow category. This is bounded by the scene's
allocated slot high-water mark. Old key variants are reference-counted and reused
when no slot retains them. Inactive slots keep only CPU identities, not graphics
resource ownership. Membership changes rebuild only active member lists.
This stage moves one CPU preparation phase and avoids repeated compatibility
lookup; it does not move graphics calls or the whole scene loop to workers.
