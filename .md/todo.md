# Engine TODO

Last updated: 2026-10-30.

Status convention:

- `[x]` Feature implemented (record failed checks separately)
- `[ ]` Implementation or follow-up work remains

## Next priorities

### Realistic grass and vegetation

Goal: author dense grass, flowers, shrubs, and small plants on editable and
streamed terrain. Keep individual placed plants available as normal scene
objects; use a dedicated instance path for large populations. Complete work
in the order below, with DX11, DX12, and Vulkan parity before calling the
feature done.

Existing foundations: `Terrain` has up to six painted material weights;
`TerrainManager` owns cell streaming, sparse edits, bakes, and budgets;
`Material` already supports base color and normal textures, metallic/roughness,
`Mask` with `alphaCutoff`, `doubleSided`, shadows, and subsurface controls.
`SceneRendering.cpp` has material pipeline variants and an instanced particle
draw, but scene meshes currently submit one instance per draw. The terrain
textures named grass are ground textures, not 3D foliage.

#### Grass-first design decisions

- Use a few authored low-poly grass clump meshes or crossed alpha-masked cards
  as reusable species. Generate **placement transforms** deterministically at
  runtime or bake time; do not create a unique mesh or scene object for every
  blade. A procedural clump mesh generator can be added later as an authoring
  option, but it should build reusable geometry once per species, not every
  frame.
- Render grass only around the active game/editor camera or player viewer. Use
  small world-space tiles inside a tunable radius, with an initial prototype
  range of roughly 25-35 m and a dithered fade near its edge. The ground's grass
  texture carries the appearance beyond that radius; far terrain tiles should
  not hold hundreds of grass meshes. Measure before fixing the final radius.
- Animate wind in the vertex shader and apply local bend impulses from nearby
  actors using a small bounded list of influence spheres. Ordinary grass has
  **no per-blade rigid-body collider**. Give a hand-placed, large plant a
  normal scene collider only when gameplay needs one.
- Start with a standalone flat-plane grass scene, then integrate the same
  species and placement system with editable and streamed terrain.

Prototype meshes for that scene are `Assets/Mesh/Grass/meadow_short.obj` and
`Assets/Mesh/Grass/field_tall.obj`, with matching vertex-color materials under
`Assets/Materials/Grass`. `Scripts/generate_grass_test_meshes.py` rebuilds both.
They are loose blade patches, not radial tufts; an instanced placement path
and broader visual tuning remain to be done.

First implementation slice (2026-10-30):
`Assets/Scenes/terrain/grass_flat_test.scene` has a flat 16 m terrain, a
first-person physics capsule, light, and editor preview patches. Its `Grass
Layer` child holds serialized `TerrainObjectLayer` placements; `GrassField`
in `Assets/Scripts/Grass` batches those placements into small near-viewer tile
meshes, with a per-frame build cap and distance unload. `TerrainManager`
captures layer data when a cell unloads and recreates layer children when it
streams back in. Its inspector lists object layers and can add or remove a
layer across loaded cells; new cells receive the same layer children. The
flat test now uses overlapping placement, fuller blade patches, and a meadow
ground texture; its 648 placements batch into 16 runtime tiles. GPU
instancing, painted
terrain masks, wind, interaction bending, fade/LOD, and renderer parity checks
remain in the unchecked tasks below.

#### 1. Asset and data contract

- [ ] Define a reusable foliage species asset: mesh or crossed-card source,
  material, scale and yaw ranges, density, slope/height limits, optional terrain
  layer weights, random seed, wind stiffness, render distances, shadow policy,
  and interaction policy. Reference existing mesh/prefab and `.material` assets
  rather than copying them. Specify defaults and a versioned serialized format.
  **Done when:** save/reload and portable build resolve the same species and
  preserve settings; missing assets produce a clear fallback or warning.
- [ ] Define foliage placement data separately from terrain material weights:
  species IDs, per-cell density masks or sparse painted overrides, and stable
  seeds. Put reusable mechanics under `Engine/Core/Compoonents/Obj/` beside
  `TerrainManager`, and choose how these records participate in scene save,
  `.terrainedit`, runtime bake, cache invalidation, and undo. Keep placement
  stable when a cell streams out and back in. **Done when:** edited placement
  survives save/reload, streaming, and rebake without duplicates or seam gaps.

#### 2. Terrain placement and streaming

- [ ] Finish object-layer controls in terrain editing: rename, reorder, and
  select a `TerrainObjectLayer` child per terrain cell. The manager now has a
  persistent layer list with add/remove controls. A selected
  layer is the destination for painted objects; keep its asset references and
  cell-local transforms in terrain cell data and include them in scene save,
  `.terrainedit`, runtime bake, undo/redo, and cell streaming. Account for
  deleted layers and edited procedural cells. **Done when:** multiple layers
  with distinct object sets survive save/reload and stream-out/stream-in.
- [ ] Extend terrain paint tools with object paint, erase, and replace for the
  active object layer. Expose asset/prefab choice, brush radius, density,
  minimum spacing, scale and yaw ranges, and a stable stroke seed. Sample the
  terrain at each candidate point for local height and normal; add an option
  to rotate local up to that normal, plus yaw about the aligned up axis and
  an optional height offset. Include slope and height limits, preview,
  clear undo groups, and seam-safe cell selection. **Done when:** painted
  placements follow slopes with the chosen alignment and edits affect only
  the selected layer after save/reload.
- [ ] Render ordinary object layers from their asset/prefab references and
  transforms, with per-layer visibility and cell lifetime. Keep simple grass
  on the batched path; allow individual plants to use ordinary child objects
  when their scripts or colliders require them. **Done when:** painting a
  non-grass asset creates the expected child objects under its layer.

- [ ] Generate instances deterministically per cell from the heightfield and
  painted layer weights, using the shared world coordinate grid so neighboring
  cells agree at boundaries. Sample/interpolate height and normal, reject
  excessive slope, and expose density and exclusion masks. Rebuild only dirty
  cells after height or weight painting. **Done when:** grass follows terrain
  edits, roads or bare layers can be excluded, and repeated generation yields
  the same transforms.
- [ ] Stream grass instance buffers in small, camera-centered tiles within the
  grass radius, sourcing heights and paint weights from loaded near terrain
  cells. Keep stable tile seeds and avoid rebuilding instances for tiny camera
  movements. In `TerrainManager`, cap preparation/upload work per frame,
  release buffers outside the radius, and account for CPU/GPU bytes in budget
  reporting. Do not require far visual tiles or distant terrain cells to be
  resident for grass. **Done when:** moving and teleporting the camera retains
  a bounded instance count without gaps, duplicates, or load spikes.

#### 3. Rendering and plant materials

- [ ] Add a shared instanced mesh submission path in `SceneRendering.cpp` and
  `IGraphicsContext` usage: batch by mesh/material/cell, pass per-instance
  transform, tint, size, and wind phase, and cull batches by frustum and
  distance. Implement DX11, DX12, and Vulkan resource/input binding without
  changing ordinary scene mesh draws. **Done when:** a dense field uses bounded
  draw calls and all three backends render identical placement.
- [ ] Author a first foliage material set: grass cards/clumps, broadleaf,
  flower, and shrub, each with color and normal maps, roughness, `Mask` alpha,
  and appropriate `doubleSided` settings. Reuse the importer and material
  serialization in `Material.cpp` / `ModelAssetWriter.cpp`; document texture
  color space and a reasonable alpha cutoff. **Done when:** imported leaves
  have clean silhouettes from both sides and remain lit at all viewing angles.
- [ ] Add vertex-shader wind with pinned roots, height or vertex-weighted bend,
  per-instance phase, low-frequency gusts, and separate fine leaf motion.
  Share scene wind direction/strength between grass and larger plants; support
  zero wind and stable time during pause and video export. Expand culling bounds
  by maximum bend. **Done when:** adjacent clumps do not sway in lockstep and
  roots stay attached to the terrain.
- [ ] Add lightweight actor-to-grass interaction: collect nearby player/actor
  positions and radii once per frame, limit the influence count per tile, and
  bend only visible grass in the vertex shader with a short recovery time.
  Provide a zero-influence fast path. Avoid Bullet bodies and collision events
  for ordinary blades. **Done when:** a moving capsule parts grass locally,
  grass recovers, and interaction cost stays bounded as grass density rises.
- [ ] Improve foliage lighting in `Object.hlsl`: verify two-sided normal
  handling, test existing subsurface/transmission controls on thin leaves,
  add species-level color variation and ground-to-tip shading only where it
  improves appearance. Validate masked leaves in direct light, shade, and
  sunset. **Done when:** back faces and overlapping leaves do not become
  unnaturally dark or uniformly bright.
- [ ] Render masked foliage consistently in camera and shadow passes, with
  matching alpha cutoff and wind deformation. Add shadow-distance and density
  controls for grass to bound cost; retain useful shrub/tree shadows farther
  out. **Done when:** wind-driven leaves do not detach from their shadows and
  distant grass shadows do not dominate GPU time.
- [ ] Implement grass distance LOD: detailed clumps near the viewer, cheaper
  or sparser clumps at mid range, then a dithered fade into the grass-colored
  ground before the grass tile radius ends. Larger shrubs can retain separate
  farther LODs. Evaluate alpha-to-coverage only on
  multisampled targets where the backends support it. Add hysteresis to cell
  or cluster LOD selection. **Done when:** camera motion has no conspicuous
  ring, popping, or bright/dark fringe at the transition.
  The test `Grass Layer` now has view, LOD, hysteresis, and shadow distances;
  `GrassField` switches among three blade-count meshes. Fade and GPU timing
  remain open; see `.md/grass-benchmark.md` for the first comparison.

#### 4. Editor and validation

- [ ] Create `Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene` as the
  first visual milestone: one flat ground plane or single flat `Terrain`, one
  camera/player capsule, one directional light, one wind control, and two grass
  species with contrasting clump shapes. Add a simple marked path or exclusion
  strip and on-screen counters for visible tiles, instances, and draws. Keep
  unrelated foliage, far terrain, and complex assets out of this scene.
  **Done when:** the scene opens in editor and game, the camera can cross the
  grass radius and walk the path, wind/actor bend can be toggled, and DX11,
  DX12, and Vulkan show the same placement and fade.
- [ ] Add a foliage mode to `SceneView` and its edit toolbar: species picker,
  paint/erase, brush size, density, slope and layer filters, and an instance
  preview. Route terrain hits through the existing brush/raycast flow; keep
  placement edits undoable and serializable. Allow a single plant to be placed
  or promoted to an ordinary prefab when it needs scripts or collision.
  **Done when:** an artist can build and revise a mixed meadow without editing
  JSON or generating meshes externally.
- [ ] Batch the grass test scene's editor preview instead of keeping one
  preview object per authored placement. Preserve the same coverage as the
  runtime layer while reducing edit-mode draw calls and scene load time.
- [ ] Add a representative meadow/woodland scene and checks for deterministic
  placement, cell boundaries, serialization, terrain edits, culling, backend
  parity, shadow alignment, and editor/game/export rendering. Benchmark at
  1080p with recorded near/far instance counts, draw calls, CPU frame time,
  GPU frame time, and memory before setting quality presets or numerical
  performance targets. **Done when:** a saved scene reloads identically and
  the three renderers have no visible gaps or major performance regression.

### Additional grass-scene performance bottlenecks (2026-10-30)

- [x] Replace editor preview-per-placement objects with batched preview tiles.
  The current authored preview path can create hundreds of separate scene
  objects in edit mode. Reuse the runtime tile batching approach for editor
  visualization so Scene View does not submit one draw/object per placement.
  **Done when:** opening `grass_flat_test.scene` keeps editor FPS stable while
  showing equivalent grass coverage.
- [x] Stop grass tile destroy/recreate churn in `GrassField::Refresh`.
  Keep persistent tile objects/meshes and update only tiles whose placement
  payload or LOD mesh actually changed. Do not rebuild unchanged tiles each
  update tick.
  **Done when:** camera movement across the grass radius does not trigger
  frequent object creation/removal spikes.
- [x] Move grass tile mesh generation and GPU upload behind a strict frame
  budget with deferred work queues.
  Build/update only a bounded number of dirty tiles per frame and carry over
  the rest. Prioritize nearest visible tiles first to avoid long editor stalls.
  **Done when:** no single frame stalls due to mass tile rebuild/upload.
- [x] Reduce per-frame `FindObjectByName` and scene-scan costs in grass update.
  Cache viewer and terrain references with invalidation on scene structure
  changes instead of resolving by name every frame.
  **Done when:** grass steady-state update work stays near-constant with static
  camera/object state.

Research behind these tasks:

- [Khronos glTF 2.0 material specification](https://github.com/KhronosGroup/glTF/blob/main/specification/2.0/Specification.adoc): masked alpha, cutoff, double-sided normals, and normal-map semantics for imported plant assets.
- [Unreal Engine foliage mode](https://dev.epicgames.com/documentation/unreal-engine/foliage-mode-in-unreal-engine): mesh instance batching, paint/erase workflow, cluster culling, and per-instance distance fade.
- [Unity terrain grass and details](https://docs.unity3d.com/Manual/terrain-Grass.html): density-painted terrain details and instanced mesh species.
- [NVIDIA GPU Gems 3, procedural wind for trees](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees): vertex-shader wind and instancing for many plants.
- [NVIDIA GPU Gems 3, SpeedTree rendering](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering): masked foliage, alpha-to-coverage, and LOD fading.
- [NVIDIA GPU Gems 2, virtual botany](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-1-toward-photorealism-virtual-botany): batched grass clumps, grid cells, pinned-root wind, and distance fade.
- [Khronos Vulkan multi-draw indirect sample](https://docs.vulkan.org/samples/latest/samples/performance/multi_draw_indirect/README.html): a later optimization option if measured CPU submission becomes the bottleneck; indirect draws are not required for the first pass.
