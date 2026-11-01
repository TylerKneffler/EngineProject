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


## Main-thread preparation and GPU grass

Grass fields opt into a reusable spatial update scheduler. Scene updates query
cached field bounds for each distinct viewer, refresh only candidate fields, and
retire fields that leave the region. Region revisions cover transform, terrain
size, viewer selection, and range changes. Component removal/reload refreshes
cached references before reading their revisions. Editor grass publication occurs
after the scene-object preview walk, avoiding appending objects while iterating
that owning container. Ordinary component callbacks honor `RequiresUpdate()`.

View visibility and terrain-occlusion classification use worker jobs against a
stable frame snapshot. Two retained record packets copy object/material values
into disjoint CPU buffer regions on workers. Graphics setup and GPU instance
culling can proceed while packing runs; a fence publishes changed records before
the first draw. Fence cleanup also waits on exceptional exits. Gameplay callbacks,
scene mutation, and graphics API calls remain on their owning thread. This is
buffered preparation overlapping submission, not arbitrary concurrent access to
the live scene.

Explicit instance batches retain shared placement lifetimes and revisioned member
keys. Sorting a camera view no longer renumbers its prepared object records.
Unchanged keys skip reading/merging placement transforms and skip input uploads.
Resident LOD draws can share the same immutable input allocation; outputs and draw
constants remain distinct per pass/frame slot. Authored placement edits must call
`Mesh::InstanceData::MarkDirty()` after updating transforms. Camera changes update
compute constants, rather than rebuilding unchanged placement input.

GPU grass compaction reads conservative placement spheres, rejects invisible
placements, classifies the target resident LOD, and writes indirect arguments and
visible affine/object records without CPU readback. Each tile prepares its material
and object record once for all resident LODs. Shader coverage intervals retain
complementary LOD transitions and outer distance fade. Custom shaders, selected
meshes, portal views, and unavailable compute paths keep compatible fallbacks.
Shadow-only records now include explicit-instance and fade flags as camera records
do. Vulkan shadow passes use compatible HDR attachment/dependency descriptions and
can preserve depth around compute dispatches.

Retained UI exits through cached component lists when a scene has no UI. Image,
button, and canvas collection uses typed registries. CPU render timings now split
retained UI and portal/overlay work from the remaining unclassified work. Worker
wall times are summed across workers and overlap other phases; they must not be
added to total frame time. Record packing and its fence wait are separate counters.
GPU grass batch counts, cache hits, placement upload bytes, and active/dormant field
counts are recorded.

Editor audit controls now count actual rendered Play frames, accept warmup frames,
and support `ENGINE_EDITOR_AUTO_PROFILE_KEEP_OPEN`. An optional
`ENGINE_EDITOR_PROFILE_LAYOUT` isolates audit docking changes. Audit traces also
include process and main-thread CPU samples over intervals of at least 500 ms;
short-pass CPU accounting is deliberately avoided.

Regression coverage includes scheduled field enter/leave, moved bounds, disabled
ancestors, deletion, asynchronous packet results, unchanged placement uploads,
per-species offsets, GPU LOD/fade comparisons at five distances, and receiver-image
comparisons for GPU grass shadows on DX11/DX12/Vulkan. Vulkan validation no longer
reports the shadow pass compatibility failures exposed by these tests. Existing
wireframe-feature and swapchain semaphore validation findings remain separate
backend work.

## Persistent placement storage and streamed cells

`PersistentInstancePool` stores generic `Mesh::InstanceData` records independently
of camera visibility, material grouping and LOD. Each 64-byte record contains a
48-byte affine transform plus a saved source/placement identity. Per-frame-slot
buffers apply a journal of changed ranges. Replacement data with the same stable
source identity compares records and retains its allocation; edits call
`InstanceData::MarkDirty()`. Free ranges coalesce, capacity grows geometrically,
and source lifetimes and replaced buffers remain pinned until the owning graphics
slot's fence completes. Capacity is retained for reuse at its residency high-water
mark rather than shrunk and reallocated while moving.

Camera and cascade batches contain 144-byte tile descriptors. Compute reads the
persistent pool and writes visible IDs and indirect draw arguments, without copying
placement matrices. Compatibility keys use source geometry, material bindings,
pipeline, camera/pass and LOD; tile membership is ordered by stable object slot.
Unchanged camera movement does not upload placement transforms. Selected/custom
shader fallbacks allocate native slices only when used. Ordinary duplicate meshes
continue to share geometry and use automatic instancing.

`TerrainObjectLayer` saves stable placement IDs and a versioned, source-signature
checked tile/affine cache. Legacy or invalid caches are prepared once and saved on
the next authoring save. `Scripts/bake_instance_layers.py` prepares existing scene
and prefab XML without rewriting unrelated formatting; `--check` verifies caches.
Changing tile size rebuilds that derived cache. Empty layers stay empty unless
`GrassField.allowProceduralGeneration` is explicitly enabled.

Typed component lists update only the changed owner, and unrelated typed-list
revisions remain stable. Render lookup entries and update-list membership refresh
only for changed owners. Physics caches unsuspended body/collider candidates by
typed component and activity revision; static transform synchronization retains
its world-revision check.

Recordings expose `instance_*` and `shadow_instance_*` upload causes: new records,
edited records, pool growth, tile metadata and fallback visible IDs. Allocations,
retirements, resident pool bytes and cache misses (cold, membership, metadata,
mode) are separate. Pool growth includes initializing the new capacity and is
deliberately distinguished from ordinary incremental uploads. Resident bytes is
a high-water gauge across views; event counters sum across views. These counters
exclude shared source geometry and ordinary object-record buffers.

`GrassPerformanceTests --static-instance-audit` runs resident movement and streaming
routes at 1848 x 1069 on DX11, using the saved scene's quality settings, with over
300 warmup frames and 240 measured frames per route. Repeat the same route/version,
viewport, renderer and quality settings for before/after comparisons. JSONL includes
CPU phases, resolved GPU timings, upload causes and actual shadow settings. The
helper covers grass preparation and rendering; full-editor, UI, simulation and
physics costs require an editor recording. The evaluator reports P95/P99 and peak
upload bytes rather than deriving performance from the smoothed FPS display.
