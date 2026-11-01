# Rendering and update performance

Implemented 2026-10-31. These engine features extend the explicit grass instancing
path to compatible repeated ordinary meshes and prefab mesh components.

## Shared geometry and automatic batches

Vertex and index buffers are shared by device, usage, stride, size, and geometry
content. Cache hits verify the bytes rather than trusting a hash alone. Generated
terrain can share buffers even when its asset paths differ. GPU ownership is weakly
cached; geometry edits detach writable buffers before publishing changes. Each
component retains its own CPU geometry for editing, collision, and deformation.

The renderer batches compatible geometry, pipelines, textures, draw flags, light
counts, and shadow state. An index buffer is supported. Every instance still reads
its own object record for transforms, material values, lighting, and clipping.
Prefab objects, scripts, and physics keep their independent identities. Blended,
skinned, deformed, custom-shader, selected/editor-preview, portal, and ordered 2D draws retain
their individual paths where batching would change behavior. Explicit grass
instance tiles continue to use their existing placement buffers and fades.

Large automatic batches (at least 32 instances) use GPU sphere/frustum culling and
indirect draws. Mesh's optional GPU LOD selection supports two or three contiguous
resident runtime LODs and its LOD distance settings. LOD distance uses the same
object origin for every variant, independent of differences in their bounds.
It selects existing source
geometry; it does not generate LOD meshes. Existing grass LOD crossfades are kept.
Small groups and unsupported compute scopes use direct batches; shadow cascades
retain CPU culling where compute cannot run inside their depth pass.

DX11 copies the generated argument header into a raw indirect argument buffer with
a tiny GPU kernel, without CPU readback. DX12 keeps indirect command signatures
alive in its context factory. Vulkan pauses and resumes scene passes with color,
depth, and stencil preserved around compute. Its descriptor caches use buffer
identities, survive native handle recycling, and recycle pools at safe frame-slot
boundaries. Render records and batch buffers are retained separately by frame slot
and render invocation so multiple views do not overwrite earlier recorded draws.
DX11 also enables and preserves scissor clipping for those independent views.
Custom surface wrappers that forward to the engine pixel shader must forward the
flat `uint` object index at `TEXCOORD11`, before `SV_IsFrontFace`; the bundled warm
overlay uses this interface.

## CPU updates and preparation

Component registries and render lookups refresh on scene structure changes.
Animation/constraint ordering is preserved while rig, portal, tissue, and particle
systems avoid repeated searches through unrelated terrain and grass objects.
Static render-data components opt out of frame callbacks. Enabled hierarchy state
is checked when callbacks run; adding components or deleting objects invalidates
their cached lists.

Frame preparation builds a cached world-bounds hierarchy. Camera views and shadow
cascades query the same hierarchy; changed bounds rebuild it. Skinned, morphed,
nonlinearly warped, or otherwise unbounded draws retain conservative fallbacks.
Material preparation is shared within a prepared frame across views. Its storage
is retained across frames while values refresh to observe script changes. Changed
object and shadow records are compared before upload; unchanged instance lists
reuse their buffers. DX11 shader-resource uploads support changed byte ranges
without discarding untouched records.

## Measurements and checks

Performance recordings include the latest rendered view's `render_culling_ms`,
`render_sorting_ms`, `render_material_ms`, `render_upload_ms`,
`render_submission_ms`, `render_batching_ms`, `automatic_batches`,
`automatic_instances`, `gpu_instance_batches`, and `render_changed_upload_bytes`.
Existing frame, shadow, GPU-stage, physics, and grass counters remain available.

The automatic rendering check compares a grid of independently colored indexed
and non-indexed meshes against individual draws. It checks direct/GPU batches,
three LOD distances, shared geometry edit isolation, and zero repeated uploads for
an unchanged scene on DX11, DX12, and Vulkan. All compared color channels match.
The component registry test covers adding a callback to a static object, disabling
and re-enabling its hierarchy, and removal. The full 441-field grass audit writes
`.local/performance-logs/grass-automatic-instancing-render-audit.jsonl`; it excludes
the editor UI and full gameplay/physics updates, so its timings are not editor FPS.

The 240-frame moving audit at 1848 x 1069 averaged 17.545 ms per helper frame,
compared with 26.540 ms before automatic batching (33.9% less frame time).
Grass updates averaged 1.415 ms and CPU shadow submission 4.764 ms. The main view
averaged one automatic GPU batch containing 149.3 terrain instances and 72.3 total
physical mesh draws including explicit grass batches. Material preparation averaged
0.923 ms, spatial/view culling 1.516 ms, draw submission 2.890 ms, and uploads
0.095 ms. The comparison is stored in
`.local/performance-logs/automatic-instancing-comparison.json`.


## Landscape submission and preparation

Compatible explicit grass tiles now share species/LOD draws across terrain cells,
in both camera and shadow passes. Each placement retains its affine transform and
object-record ID, so tiles keep independent world transforms, material data,
lighting, and complementary LOD/visibility fades. Custom shaders, selected meshes,
blending, deformation, portals, and incompatible render state retain their existing
submission paths. Source blade geometry remains shared on the GPU; batching copies
placement records, never blade vertices.

Placement staging vectors, direct instance records, upload buffers, and constants
are retained in frame slots. Unchanged records skip uploads. A small persistent
worker pool prepares large placement batches, cascade masks, and fallback resident
LOD classifications from read-only snapshots; graphics calls and scene mutation
stay on the owner thread. Small workloads remain inline to avoid wake-up costs.

Shadow culling tests the projected receiver silhouette as well as the light
frustum. Projection is perpendicular to light rays, allowing off-screen casters
whose shadows reach visible receivers. Receiver bounds include cascade blend
coverage and filter padding. Project shadow presets now also select grass caster
distance (Low 6 m, Medium 10 m, High 16 m, Ultra 24 m), capped further by the terrain
layer's own range. Existing projects retain their existing grass range until a
preset or the new preference is changed.

Audio coordination, realtime light collection, spatial sampling, and camera warp
checks use component registries rather than full terrain hierarchy scans. Visibility
queries retain a small per-view cache, invalidated when bounds change. Lighting
bounds use affine center/extents instead of transforming eight corners, and view
preparation avoids repeating frustum tests already performed by the BVH.

Recordings and the Performance view include `shadow_submission_wall_ms`,
`shadow_submission_thread_cycles`, and `shadow_max_batch_ms`. The evaluator compares
cycle density during long submissions with the capture median. Unusually low
thread cycles per millisecond can indicate scheduling or waits; frequency changes
also affect that ratio, so it is diagnostic evidence rather than proof of a GPU
stall. Per-pass OS thread CPU accounting was rejected because its 15.625 ms
quantization exceeds these short passes.

Run `GrassPerformanceTests --landscape-matrix` for a fixed 1848 x 1069 Debug
comparison. It loads the stress scene once, warms each mode over the moving route for at least
300 frames, and records 240 frames
per mode using the same sinusoidal camera route and shadow settings. The default
mode is the same scene with mesh geometry hidden; the other modes enable terrain,
then grass without shadows, then full grass. Output files are
`.local/performance-logs/landscape-matrix-*.jsonl`. Records mark warmup frames and
include GPU timestamps; `Scripts/evaluate_performance.py` excludes warmup and
removes repeated asynchronous GPU samples. This helper measures rendering and
grass updates, without editor UI, audio, or full gameplay/physics updates; its FPS
is not editor FPS.
