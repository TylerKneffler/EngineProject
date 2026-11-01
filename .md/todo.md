# Engine TODO

Last updated: 2026-11-01.

## Parallel scene updates and rendering

- [ ] Identify and eliminate graphics-submission stalls with native-call timings for buffer mapping, dispatch, resource binding, indirect arguments, draws, and fence waits.
- [ ] Replace the fixed three-worker render pool with a configurable engine job scheduler supporting dependencies, workload-sized batches, and measured worker utilization.
- [ ] Publish immutable scene snapshots and defer object creation, removal, hierarchy changes, and component writes to defined commit boundaries.
- [ ] Add component update dependencies and explicit read/write access so independent updates can run concurrently while existing scripts retain a compatible serial path.
- [ ] Update dirty transforms and world bounds in parallel according to hierarchy dependencies; retain unchanged static results.
- [ ] Cache static render items and spatial-index entries, updating only changed objects instead of rebuilding their preparation every frame.
- [ ] Prepare render items, material records, visibility, sorting, and compatible draw batches on workers from stable snapshots.
- [ ] Share camera and shadow preparation across views and cascades; schedule independent visibility and batch preparation without unnecessary per-loop barriers.
- [ ] Move terrain-cell loading, decoding, instance preparation, and bounds construction onto workers with bounded queues, cancellation, and scene-generation checks.
- [ ] Parallelize independent animation and rig evaluation while preserving animation, constraints, physics, and final-pose ordering.
- [ ] Parallelize physics preparation and post-processing where independent; preserve simulation ownership and synchronized scene publication.
- [ ] Move graphics submission to a dedicated owning render thread using buffered render packets, frame-safe resource retirement, and bounded frame latency.
- [ ] Add worker command recording through backend-specific contexts: benchmark DX11 deferred contexts and support independent DX12/Vulkan command lists or buffers.
- [ ] Remove confirmed buffer-reuse synchronization stalls with frame-safe constant, upload, and indirect-argument allocations.
- [ ] Move expensive editor data preparation and asset processing onto workers while keeping window, input, and UI construction on their owning thread.
- [ ] Record per-stage main-thread busy time, worker execution, queue delay, dependency waits, render-thread stalls, and frame latency in the Performance view and recordings.
- [ ] Validate serial and parallel modes with matched-route captures, frame-time percentiles, worker scaling, and regression checks for scene loading, Play/Stop, streaming, transforms, shadows, and all renderers.

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
