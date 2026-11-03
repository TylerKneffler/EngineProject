# Parallel work: persistent spatial draw slots

Historical local-stage report preserved during the 2026-11-02 merge. The combined scene path retains the newer upstream slot-bound aggregation, including conservative union for shared chart slots; the local persistent bounds utility, tests and measurements are preserved. Measurements below describe the pre-merge local stage.

Scene camera and shadow culling now address the spatial hierarchy by persistent
render-object slots instead of transient positions in the active draw list.
Temporary visibility changes and LOD fades can remove, reorder and restore draws
without changing spatial membership. This is generic scene rendering behavior;
it applies to ordinary meshes as well as grass and terrain.

## Implementation

- `RenderBoundsCache::PreparePersistent` writes active results into caller-owned
  slot storage, reusing the existing copied-input worker bounds path. Inactive
  slots retain their last bounds; active unbounded items explicitly replace any
  earlier bounded result. Invalid slot ranges are rejected before storage changes.
- Scene storage uses the same persistent slots already assigned to GPU object
  data, including separate LOD, previous-LOD and portal-chart keys. The existing
  owner/chart cleanup and free-slot reuse govern lifetime. Recycled active slots
  receive their current bounds before the tree is published.
- Camera classification and all directional-shadow cascade masks look up the
  active item's object slot. No draw submission iterates retained bounds, so
  inactive bounds cannot create phantom draws. Existing portal/2D fallbacks stay
  conservative; the generic spatial-script index keeps its packed API.
- Record `prepare_spatial_active_slots` and `prepare_spatial_retained_slots` in
  the frame recording, Performance view, native scene diagnostic and evaluator.
  The evaluator explains that retained capacity includes inactive/free slots.

## Controlled CPU experiment

`RenderSpatialIndexTests --slot-benchmark` warms all 4,096 static slots, then
rotates/reorders an active subset of 3,072?3,328 items with varying membership.
Eight warmup updates and 120 measured updates run in each mode in one Debug
process. Timings include bounds-cache preparation, the bounds copy into the
index, and index update. Input construction and visibility queries are excluded.
Both modes use the current bounds cache and current incremental tree.

| Layout | Bounds preparation + index ms | Index ms | Full rebuilds |
|---|---:|---:|---:|
| packed active list | 5.600 | 5.334 | 120 |
| persistent slots | 0.344 | 0.076 | 0 |

This demonstrates avoiding membership rebuilds, not a full-engine FPS change.
Artifact: `.local/performance-logs/parallel-stage6-slot-benchmark.jsonl`.

## Validation

- Debug Editor, Game, ProjectScripts and the focused regression helpers build.
- Nine focused CTests pass: HumanoidRigMapping, ParticleEmitterRegression,
  GpuParticleDX11Regression, GpuParticleDX12Regression,
  GpuParticleVulkanRegression, GrassPerformance, TerrainColliderStreaming,
  AssetPreviewAsync and RenderSpatialIndex. Thirteen evaluator tests pass.
- Persistent-slot regressions compare each active item's visibility with an
  independently calculated packed reference across twelve camera/shadow matrices.
  Cover reorder, inactive/empty frames, reactivation, slot recycling, transforms,
  bounded/unbounded transitions, holes, growth, storage reset and invalid ranges.
  The new tests pass with default workers and `ENGINE_WORKER_COUNT=0`.
- DX11/DX12/Vulkan grass image checks pass with zero differing channels against
  CPU references, including LOD fades, compaction and shadow receivers. These
  checks now reserve an earlier off-camera draw slot, hide it, and restore it for
  the shadow comparison, exercising visibility masks across gaps in active draws.
- Ordinary automatic instancing, GPU LOD/visibility, dirty uploads and edit
  isolation image checks pass on DX11 with zero differing channels.
- An initial unrestricted CTest invocation included unrelated optional targets
  absent from this build and was stopped during TerrainCapsuleTraversalRegression.
  Only the nine explicitly selected, rebuilt tests above are claimed as passing;
  the full repository suite was not completed.

## Grass-scene measurements

The 441-cell Debug diagnostic finished loading and completed both routes, each
with 240 measured moving frames after warmup. The captures and evaluator reports
are `.local/performance-logs/parallel-stage6-{resident,streaming}*.{jsonl,json,md}`.
Only records marked `measured` contribute to these values.

| Route | Index ms | Bounds + index ms | Rebuilds | Refitted nodes | Active slots avg | Retained slots avg |
|---|---:|---:|---:|---:|---:|---:|
| resident | 0.039 | 0.164 | 10 | 0 | 524.0 | 537.9 |
| streaming | 0.045 | 0.195 | 8 | 3,645 | 520.2 | 554.5 |

No quality rebuilds occurred. Retained storage peaked at 541 and 556 slots,
respectively; current active draws peaked at 529. The retained count averaged
2.6% and 6.6% above active draws. The earlier stage-5 captures had index averages
of 0.202/0.847 ms and 44/193 rebuilds for these routes. Those are separate runs,
not an isolated before/after FPS experiment; the controlled CPU experiment above
establishes the specific membership benefit. Current diagnostic CPU-frame times
were 9.34 ms resident and 15.08 ms streaming, including their own timing scope;
they do not represent end-user editor FPS or prove a full-engine FPS gain.

## Remaining scope and tradeoffs

Retained bounds and visibility buffers use the scene's maximum allocated draw-slot
count. Removed objects release slots for reuse, but storage does not shrink every
frame. Inactive historical bounds can reduce hierarchy quality or add query work;
active/retained counters expose this cost. Slot growth, bounded-state changes and
quality degradation can still rebuild the tree on the owner thread. There is no
new job per object and no GPU readback. Existing worker refits remain available
for large updates; avoiding rebuilds removes work rather than migrating it.

Static render-item caching, sorting preparation, immutable scene snapshots and
simulation/render thread ownership remain separate TODOs.
