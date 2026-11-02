# Parallel work: hierarchy-aware transform preparation

Before-render transform preparation now uses copied inputs and hierarchy-level jobs.
Wide sets of dirty transforms run on workers; narrow levels stay on their owner.
Completed float/double matrices and revisions are committed on the owner before
render-item and bone-palette preparation reads them. The helper is generic and
has no grass/terrain-specific behavior.

## Changes

- Cache hierarchy order and detect public parent-pointer edits independently of
  scene notifications. Preserve direct public TRS edits, explicit MarkDirty calls,
  lazy getters, external parents, double-precision positions, and matrix layers.
- Reuse static caches without scheduling jobs. In dirty batches, copy matrices
  only for changed nodes and the unchanged parents their computation requires.
- Retain dictionary allocations and use flat child/sibling adjacency arrays,
  avoiding per-object child-vector allocation during topology changes.
- Reject duplicate/cyclic batch topology before publishing transform caches.
- Keep the existing lazy owner path on scene structure-change/streaming-commit
  frames. Stable structure resumes the batch path. This avoids spending a whole
  hierarchy-index rebuild on a handful of newly streamed grass leaves.
- Use explicitly prepared reads during stable render-item and bone-palette
  preparation to avoid walking every ancestor chain again. Root/non-batched
  transforms retain cheap lazy validation. Prepared reads require a stable owner
  phase without intervening transform/hierarchy writes.
- Add capture/compute/commit wall times, local/world update and reuse counts,
  hierarchy levels, and the structure-change fallback flag to recordings and the
  Performance view. Evaluation reports these and explains nested timing scopes.
  On fallback frames, transform validation is inside item/bone preparation,
  rather than the dedicated batch counters.

## CPU benchmark

Command: `GrassPerformanceTests --transform-benchmark`. Each case uses 4,095
objects, eight warmup frames and 60 measured frames, with equivalent poses and
world-matrix reads. These are Debug CPU preparation timings, not engine FPS.
Cases run in one process; workload order, OS scheduling and machine conditions
can affect results.

| Topology | Motion | Lazy owner ms | Batch serial ms | Batch parallel ms |
|---|---|---:|---:|---:|
| roots | static | 0.160 | 0.416 | 0.407 |
| roots | moving | 27.013 | 32.282 | 4.791 |
| tree | static | 5.098 | 1.703 | 1.749 |
| tree | moving | 42.723 | 39.632 | 9.587 |

Parallel preparation benefits wide moving workloads. Flat static objects incur
a small scan overhead, and dense serial batching can be slower than the original
lazy path. Deep static hierarchies benefit from prepared reads. A single narrow
chain has limited parallelism because children depend on their parents.

## Validation

- Regression tests compare float/double matrices against independent hierarchical
  TRS evaluation across 2,047 transforms, including child-first input ordering,
  direct leaf/parent edits, explicit dirty notifications, reparenting, deletion,
  external parent edits, cycles, duplicate inputs, layers, and serial/parallel modes.
- Repeated streamed-leaf insertion/removal preserves unrelated matrices.
  Scene integration tests cover structure-change fallback, descendant updates,
  and resumption of cached preparation after streaming stops.
- Five focused CTests pass: GrassPerformance, HumanoidRigMapping,
  ParticleEmitterRegression, TerrainColliderStreaming, and AssetPreviewAsync.
  Twelve Python capture-evaluator tests pass.
- Debug Editor, ProjectScripts and Game are rebuilt. Grass GPU compaction,
  cross-tile placement, LOD/fades at five distances, and shadow-receiver image
  comparisons match CPU references across DX11, DX12, and Vulkan.

## Full-scene recordings

The warmed 441-cell grass diagnostic recorded 240 measured frames on each route:

| Route | Batch stage average ms | Structure fallback frames | Scope |
|---|---:|---:|---|
| Resident | 0.263 | 6 / 240 | Mostly unchanged authored transforms |
| Streaming | 0.002 | 238 / 240 | Owner validation stays inside item/bone preparation |

The initial unconditional topology rebuild measured 3.367 ms per streaming frame.
The final adaptive path avoids that added batch-stage overhead. This does not
mean the streaming scene performs no CPU transform work: the fallback flag and
evaluator warning identify its original lazy validation path. Captures are
`.local/performance-logs/parallel-stage4-{resident,streaming}.jsonl`, with generated
evaluator reports beside them. No full-scene FPS improvement is inferred from
these diagnostic runs; timing conditions and streaming publication can differ.

The final full-editor trace `.local/performance-logs/parallel-stage4-editor.jsonl`
completed 120 warmup and 120 measured Play frames. The measured transform stage
averaged 0.011 ms for two unchanged objects. All 276 accepted writer records were
written with zero drops/failures, and the trace ends in `capture_complete`.

Graphics calls and cache publication stay on their existing owner. Component
lifecycle, animation/physics scheduling, immutable full-scene snapshots, and the
dedicated simulation/render threads remain separate TODOs.
