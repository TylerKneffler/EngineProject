# Persistent instance storage validation

Implemented the static instance storage and streaming tasks in `.md/todo.md`.
Placement matrices now live in a reusable renderer pool. Camera and shadow compute
passes compact IDs and indirect arguments; they do not rebuild or upload a merged
matrix buffer when visibility, draw sorting or LOD changes.

The shared implementation accepts `Mesh::InstanceData`; it is not restricted to
grass. Saved terrain layers carry stable IDs and baked tile/affine records. The
bundled grass scene and terrain-cell prefab were baked with
`Scripts/bake_instance_layers.py`. Ordinary duplicate meshes retain shared geometry
and automatic instancing.

## Measurements

Command: `GrassPerformanceTests --static-instance-audit`.
Debug DX11, 441 terrain cells, 1848 x 1069, four 2048-pixel shadow cascades;
over 300 warmup frames followed by 240 measured frames for each fixed route.

| Measurement | Resident movement | Streaming route |
|---|---:|---:|
| Average frame wall time | 11.118 ms | 16.827 ms |
| P95 frame wall time | 17.255 ms | 21.378 ms |
| P99 frame wall time | 25.751 ms | 24.582 ms |
| Main rendering CPU | 5.505 ms | 6.250 ms |
| Shadow rendering CPU | 2.256 ms | 1.888 ms |
| Grass updates | 0.438 ms | 1.959 ms |
| GPU shadow stage | 6.052 ms | 5.397 ms |
| GPU terrain stage | 0.313 ms | 0.537 ms |
| GPU opaque stage | 1.697 ms | 1.596 ms |
| New tiles prepared | 16 | 766 |
| Placement rescans | 0 | 0 |
| Edited placement upload bytes, both passes | 0 | 0 |
| Frames with zero placement uploads, both passes | 229 / 240 | 34 / 240 |
| New placement bytes, both passes, entire route | 31,104 | 1,241,600 |
| Pool growth upload bytes, entire route | 0 | 524,288 |

Resident movement retained a 262,144-byte placement pool. Streaming grew it to
524,288 bytes; temporary retirement of the previous allocation peaked at
786,432 bytes. Growth remains an occasional full-capacity upload, separately
recorded from new or edited ranges. Tile metadata uploads remain necessary when
candidate membership changes. Rescans are zero because cells load baked records.

Raw captures and evaluator outputs are under
`.local/performance-logs/static-instance-{resident,streaming}.*`.

These routes are separate workloads, not a before/after comparison. They exercise
grass updates and rendering without the editor UI or physics. CPU wall time and
asynchronously resolved GPU stages overlap. The historical user captures have
different workloads/viewports, so these figures do not establish a full-editor
FPS improvement. GPU shadows remain the largest measured GPU stage.

## Regression coverage

The Editor Debug build succeeded. Nine targeted CTest regressions and nine Python
performance-evaluator tests passed. Automatic instancing image checks also passed
on DX11, DX12 and Vulkan.

- DX11, DX12 and Vulkan image comparisons cover instances with no legacy slice
  buffers, independent tile fades, GPU compaction, five LOD distances and shadow
  receiver images.
- Persistent storage checks verify zero unchanged uploads, a single 64-byte record
  upload after an edit, allocation reuse when a saved tile is replaced, and bounded
  capacity during repeated source streaming.
- Baked records round-trip exact affine data and stable IDs; edits and invalid
  cache data fall back to preparation safely. Empty authored layers remain empty,
  with explicit procedural generation available as an opt-in.
- Registry tests verify unrelated additions preserve typed registry revisions and
  storage. Terrain collision checks cover center/edge/corner activation, teleport,
  suspend/resume and a walking capsule crossing cell seams.

Recordings and the Performance view now separate new, edited, growth, metadata,
visible-ID and custom shader fallback upload bytes. They also expose allocation,
retirement and cache-miss causes independently for main and shadow rendering.

## Scene switching regression fixed (2026-11-01)

Opening the grass scene after normal editor startup exposed a missing component
cache notification in object deserialization. Caches initialized by the default
scene stayed empty for newly loaded grass fields, so the scheduler produced no
grass in either view. Startup-scene captures bypassed this ordering and therefore
missed the defect. Deserialization now publishes each completed object's components
to the existing typed, update and render caches. A regression warms the caches
before loading, then verifies grass scheduling in both Edit and Play. It fails
with the original loader and passes with the notification restored.
