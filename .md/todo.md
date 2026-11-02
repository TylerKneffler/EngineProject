# Engine TODO

Last updated: 2026-11-01.

## Parallel scene updates and rendering

- [ ] Identify and eliminate graphics-submission stalls with native-call timings for buffer mapping, dispatch, resource binding, indirect arguments, draws, and fence waits.
- [x] Replace the fixed three-worker render pool with a configurable engine job scheduler supporting dependencies, workload-sized batches, and execution/queue/wait measurements.
- [ ] Move the native scene/game loop onto a dedicated simulation thread, separate from window/input/UI handling and graphics submission, with synchronized input snapshots and bounded render handoff.
- [ ] Schedule native update phases as dependent jobs: component updates, spatial scripts, transforms, animation/constraints, physics preparation, simulation, post-physics processing, and render snapshot publication.
- [ ] Publish immutable scene snapshots and defer object creation, removal, hierarchy changes, and component writes to defined commit boundaries.
- [ ] Add component update dependencies and explicit read/write access so independent updates can run concurrently while existing scripts retain a compatible serial path.
- [ ] Add explicit thread-affinity and parallel-safety policies for native component and script callbacks; move eligible Update work onto workers and marshal window, graphics, and shared-state operations to their owners.
- [ ] Run Start, Enabled, Disabled, and OnDestroy on the owning simulation thread with deterministic ordering, once-only lifecycle delivery, and completion of jobs referencing affected objects before destruction.
- [ ] Separate CPU preparation in deserialization and lifecycle callbacks from graphics resource creation, submitting resource work to the render thread.
- [ ] Coordinate fixed-step updates, Play/Stop, pause, scene loading, hot reload, and shutdown across simulation, workers, and rendering without stale jobs or partially published scenes.
- [ ] Route reloadable script-module jobs through the host-owned scheduler and complete or cancel module jobs before unloading their code.
- [x] Parallelize dirty transform preparation for stable hierarchies, retain static matrices, and preserve owner validation during streaming commits.
- [x] Cache unchanged world bounds and prepare changed bounds from copied matrices on workers.
- [x] Retain spatial hierarchy topology and refit changed bounds, using worker jobs for large updates and rebuilding on membership or quality changes.
- [ ] Keep spatial entries keyed to persistent draw slots across LOD fades and temporary visibility changes.
- [ ] Cache static render items, updating only changed objects instead of rebuilding preparation every frame.
- [ ] Prepare render items, material records, visibility, sorting, and compatible draw batches on workers from stable snapshots.
- [ ] Share camera and shadow preparation across views and cascades; schedule independent visibility and batch preparation without unnecessary per-loop barriers.
- [ ] Move terrain-cell loading, decoding, instance preparation, and bounds construction onto workers with bounded queues, cancellation, and scene-generation checks.
- [ ] Parallelize independent animation and rig evaluation while preserving animation, constraints, physics, and final-pose ordering.
- [ ] Parallelize physics preparation and post-processing where independent; preserve simulation ownership and synchronized scene publication.
- [ ] Move graphics submission to a dedicated owning render thread using buffered render packets, frame-safe resource retirement, and bounded frame latency.
- [ ] Add worker command recording through backend-specific contexts: benchmark DX11 deferred contexts and support independent DX12/Vulkan command lists or buffers.
- [x] Advance DX11 frame resources and persistent-cache retirement independently of GPU timing controls.
- [x] Append DX11 compute constants to the shared constant arena; retain fallback uploads and invalidate cached ranges after cross-context arena discards.
- [x] Add opt-in DX11 native-call timing by camera/shadow pass, including call counts, wall-time maxima, thread cycles, recording fields, and capture evaluation.
- [ ] Remove confirmed buffer-reuse synchronization stalls with frame-safe constant, upload, and indirect-argument allocations.
- [ ] Move expensive editor data preparation and asset processing onto workers while keeping window, input, and UI construction on their owning thread.
- [ ] Record per-stage main-thread busy time, worker execution, queue delay, dependency waits, render-thread stalls, and frame latency in the Performance view and recordings.
- [ ] Validate serial and parallel modes with matched-route captures, frame-time percentiles, worker scaling, and regression checks for scene loading, Play/Stop, streaming, transforms, shadows, and all renderers.

## Additional main-thread work from the code audit

- [x] Move performance-record formatting and file writes to a bounded ordered writer queue; capture immutable timing data on its owner, report queue overflow, and drain recordings cleanly on stop.
- [x] Cache asset-browser directory listings, file metadata, and filtered/sorted search results on workers, refreshed through filesystem notifications with a polling fallback.
- [x] Prepare visible asset thumbnails asynchronously with bounded jobs, private decoding, stale-result cancellation, and graphics-owner publication.
- [ ] Move scene/prefab file reads, parsing, snapshot encoding, save writes, and undo-history comparisons onto workers from versioned snapshots; commit live scene changes on the simulation owner and retain edit revision ordering.
- [ ] Run model/motion import, asset copying/remapping, lighting bakes, and generated-asset writes as cancellable background jobs with progress and bounded result publication.
- [ ] Compile and cache shader bytecode and decode textures on workers; prewarm required variants and queue graphics resource/pipeline publication without blocking render preparation on first use.
- [ ] Move CPU particle simulation, surface preparation, skin/morph fallbacks, mesh deformation, and warp sampling onto jobs or existing GPU paths; batch collision requests against a safe physics snapshot.
- [ ] Accelerate editor picking with cached spatial/triangle indexes and worker-prepared skinned geometry; validate asynchronous hover results against camera, pose, and scene revisions.
- [ ] Decode and preload audio files asynchronously, including the current synchronous OGG path; publish playback commands through the audio owner's queue while preserving real-time mixing.
- [ ] Move hot-reload module/symbol copying and fallback filesystem scans off the frame loop; budget build-output processing and publish module replacement only after affected callbacks and jobs finish.
- [ ] Pipeline export readback, image encoding, and output writes with bounded buffers; keep GPU readback operations on the graphics owner and preserve deterministic frame ordering.
- [x] Replace edit-preview whole-scene scans with typed particle-emitter and animation registries.
- [ ] Schedule preview CPU preparation through the same dependency system as gameplay.
- [ ] Extend thread accounting to UI, simulation, rendering, workers, and writer queues; separate recurring frame work from import/save/bake/startup hitches in recordings and benchmarks.

## Static instance storage and streaming performance

- [x] Add generic persistent GPU instance pools for saved terrain-layer and prefab placements.
- [x] Keep object and instance IDs stable across visibility, sorting, LOD, and streaming changes; key batch caches by render compatibility instead of submission order.
- [x] Compact visible instance IDs and indirect LOD draw arguments on the GPU for camera and shadow passes without rebuilding placement transforms.
- [x] Upload only new or edited placement ranges and changed object metadata; preserve unchanged placement storage during camera movement.
- [x] Bake tile indexes and render-ready instance records with static placement data to avoid runtime regrouping and transform conversion.
- [x] Stream cell instance allocations with reusable capacity, bounded residency, and GPU-fence-safe retirement.
- [x] Make procedural runtime placement generation explicit opt-in and preserve intentionally empty authored layers.
- [x] Update component registries and render-item caches incrementally without invalidating unrelated scene data when tiles enter or leave.
- [x] Maintain active physics work lists and synchronize static colliders only when changed; skip suspended terrain bodies during routine setup and post-step work.
- [x] Record instance-cache miss reasons, buffer allocations, and placement upload bytes by cause and camera/shadow pass.
- [x] Add matched-route movement and streaming benchmarks for upload spikes, frame-time percentiles, and CPU/GPU costs at fixed viewport and quality settings.

## Main-thread performance

- [x] Break down retained UI, portal/overlay work, worker execution, synchronization waits, and record packing in recordings and the Performance view.
- [x] Centralize grass updates with viewer queries over cached terrain-field bounds and retire fields leaving the active region.
- [x] Prepare visibility, fallback LOD classifications, and copied material/object records on workers.
- [x] Retain grass placement buffers and stable member identities; reuse unchanged batches across camera and LOD draws.
- [x] Add GPU grass visibility, LOD classification, complementary fades, and indirect camera/shadow draws across DX11, DX12, and Vulkan.
- [x] Buffer immutable render records and overlap worker packing with graphics setup and GPU culling; keep scene mutation and graphics calls on their owning thread.
- [x] Validate editor frame time, percentiles, main-thread usage, worker utilization, and GPU timings with a warmed full-editor capture.

## Landscape performance

- [x] Batch compatible grass tiles by species, LOD, material, and render state in camera and shadow passes.
- [x] Cull shadow casters against cascade receiver silhouettes while preserving off-screen shadows and cascade fades.
- [x] Add grass caster distance to shadow quality presets and project preferences.
- [x] Use cached audio source lists and process each source once.
- [x] Use cached light and spatial-volume lists; reuse view visibility and simplify bounds preparation.
- [x] Reuse grass and shadow instance buffers, constants, and CPU staging storage.
- [x] Prepare large instance batches, cascade visibility, and fallback LOD classifications with worker jobs.
- [x] Record shadow submission wall time, active thread cycles, and slowest batches.
- [x] Compare empty, terrain-only, grass without shadows, and full grass with a fixed route, viewport, build, and quality.

## Engine rendering and update performance

- [x] Automatically instance compatible repeated meshes and prefab mesh components in camera and shadow passes.
- [x] Share immutable GPU mesh buffers across duplicate asset, generated terrain, and prefab usage.
- [x] Cache material and render preparation shared across draws, views, and shadow cascades.
- [x] Use component registries and cached update lists for scene-level rig, constraint, portal, tissue, and particle processing.
- [x] Reuse cached transforms and preparation; upload only changed object and instance records.
- [x] Add hierarchical spatial culling for terrain cells, objects, and instance batches.
- [x] Cull instanced shadow batches against each cascade and reuse caster preparation.
- [x] Add GPU instance visibility, optional resident LOD selection, and indirect draws across DX11, DX12, and Vulkan.
- [x] Record separate CPU timings for render culling, sorting, material preparation, data uploads, and draw submission.
- [x] Preserve frame-safe render records and clipping for multiple batched camera views.
- [x] Expose current batching, upload, grass fade, shadow workload, frame-percentile, and process/thread CPU measurements in the Performance view and recordings.
- [x] Add a repeatable capture evaluator with GPU sample deduplication and measurement-scope checks.

## Grass performance

- [x] Replace CPU-expanded grass tiles with GPU instances of shared species/LOD meshes.
- [x] Share compact species placement buffers across each tile's LODs; avoid uploads when changing LOD.
- [x] Apply a scene-wide grass preparation/upload budget and retain nearby tiles beyond the visibility radius.
- [x] Record visible grass instance counts, placement upload bytes, and shared GPU memory without double counting.
- [x] Share grass species geometry across terrain cells to avoid repeated source loading.
- [x] Prevent flat terrain triangle edges from kicking the player capsule upward.

- [x] Keep terrain collision active only under the player and nearby edges/corners, with at most four active cells per landscape.

- [x] Move runtime LOD caching, hysteresis, buffer switching, and memory accounting into each object's Mesh component.

- [x] Reduce CPU overhead in DX11 shadow draw submission.
- [x] Cache grass placement data by tile and remove repeated full-placement scans and hashing.
- [x] Switch resident grass LODs without rebuilding or uploading geometry.
- [x] Support gameplay and editor cameras as explicit grass viewers.
- [x] Record grass scanning, tile generation, upload time, pending work, and rebuild counts in Performance recordings.
- [x] Preserve accurate shadow draw counts after the main render pass.
- [x] Reduce Play/Stop scene transition stalls by excluding generated grass geometry and retiring legacy preview objects.

## Foliage assets and terrain editing

- [ ] Add reusable foliage species assets with placement, material, wind, LOD, shadow, and interaction settings.
- [ ] Store foliage density masks, painted overrides, and stable seeds with terrain edits, saves, bakes, and streamed cells.
- [ ] Add terrain object-layer rename, reorder, selection, and visibility controls.
- [ ] Add foliage paint, erase, and replace tools with species selection, density, spacing, scale, rotation, slope, height, and terrain-layer filters.
- [ ] Render ordinary object layers from mesh or prefab references; allow individual plants with scripts or colliders.
- [ ] Generate deterministic foliage from terrain height, normals, and painted weights; refresh only affected cells after edits.
- [ ] Stream foliage in viewer-centered tiles with bounded preparation, upload, and memory budgets.

## Foliage rendering

- [x] Add shared mesh instancing, tile bounds culling, and placement buffers across DX11, DX12, and Vulkan.
- [ ] Add grass, broadleaf, flower, and shrub materials with masked textures and two-sided lighting.
- [ ] Add vertex-shader wind with pinned roots, varied phases, gusts, and stable pause/export behavior.
- [ ] Add bounded actor-driven grass bending and recovery in vertex shaders.
- [ ] Improve foliage transmission, species color variation, and ground-to-tip shading.
- [ ] Match alpha masking and wind deformation between camera and shadow passes; add shadow distance and density controls.
- [x] Add smooth grass LOD transitions and distance fade into ground coverage.

## Scenes and quality controls

- [x] Expand the grass stress scene to 21 x 21 terrain cells with grass across the 336 x 336 m field.

- [ ] Extend the flat grass scene with wind, interaction toggles, an exclusion path, and tile/instance/draw counters.
- [ ] Add a representative meadow/woodland scene and foliage quality presets based on measured performance.
- [ ] Keep placement, terrain seams, shadows, LOD transitions, and saved edits consistent across editor, gameplay, export, and all renderers.
