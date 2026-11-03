# Engine TODO

Last updated: 2026-11-03.

## Editor Play performance and Stop freezes

- [x] Overlap editor Play simulation with graphics submission through owned Scene/Game view command packets, bounded owner handoffs and GPU resource retirement.
- [x] Reuse completed render packets and command storage; expose simulation work, completion wait, submission overlap and packet recording in Performance recordings and evaluation.
- [ ] Extend worker preparation beyond current culling/packing jobs to the remaining render-item, material and command-recording work from immutable inputs.
- [ ] Validate editor scaling on matched moving/streaming routes across DX11, DX12 and Vulkan; keep stationary measurements separate from gameplay gains.
- [x] Retain the authored scene in memory during Play and restore its original objects, selection, history and edit sessions on Stop.
- [ ] Prepare the separate runtime scene incrementally so Play startup does not block while cloning large terrain/prefab graphs.
- [ ] Retire the stopped runtime scene with bounded owner-thread cleanup after GPU completion; remove the remaining destruction hitch.
- [ ] Cache versioned immutable prefab documents across repeated instances, with file-change invalidation and independent per-instance state.
- [ ] Prepare runtime-copy configuration snapshots from owned data and publish at the owner commit boundary without blocking UI for first serialization.
- [ ] Record separate Play/Stop fence, GPU-wait, snapshot, deserialization and publication timings alongside full-recording transition stalls.

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
