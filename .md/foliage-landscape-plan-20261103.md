# Foliage and large-landscape plan

Research and repository review: 2026-11-03. This is an implementation plan, not a
measured claim that any individual change will reach 100 FPS.

## Current engine and performance

- `GrassField` uses deterministic 4 m tiles, two species, three authored mesh
  LODs, distance fades, and a viewer radius. `TerrainObjectLayer` stores stable
  placement IDs and baked tile data. The scene has a 21 x 21 cell, 336 x 336 m
  grass stress landscape.
- Explicit foliage shares geometry and compatible species/LOD draws across cells.
  A persistent 64-byte placement pool, GPU frustum compaction, indirect draws,
  incremental uploads, and CPU spatial indexing already exist on DX11, DX12,
  and Vulkan. These should be extended rather than replaced.
- The 2026-11-03 interactive editor capture at 790 x 589 averaged 31.67 ms per
  Play interval. Resolved GPU stages averaged grass 8.38 ms, shadows 7.67 ms,
  terrain 4.54 ms. These asynchronous stages overlap CPU work. A separate
  1848 x 1069 moving Debug grass audit recorded roughly 63 ms GPU grass and
  10 ms GPU shadows; it is a different workload and should not be compared as
  an FPS before/after result.
- Current foliage TODOs include species assets, density masks and painting,
  bounded streaming, more complete materials and wind, and quality presets.

## What current production engines suggest

1. Keep abundant ground cover as instanced data rather than independent actors.
   Unreal groups static foliage into hardware-instanced batches; Unity recommends
   instanced detail meshes with persistent instance buffers. We already do the
   rendering part; our missing work is authoring and scalable residency.
2. Partition generation and streaming at multiple spatial scales. Unreal PCG
   uses coarse cells for trees and fine cells for grass, and World Partition
   streams cells around a viewer. Its HLOD system uses low-detail instancing for
   distant trees. Our terrain cells and grass tiles provide a starting grid.
3. Treat screen coverage and shadow reach as explicit budgets. Unity exposes
   detail distance, density scaling, tree billboard distance, and fade length.
   Unreal recommends limiting dynamic shadows and distant animation for dense
   foliage. Large masked surfaces and overlapping layers still create overdraw,
   even with virtualized geometry.
4. Use GPU-driven indirect culling where it saves CPU work. Direct3D 12 and
   NVIDIA document this pattern. We already use it; changing to mesh shaders
   or a Nanite-like pipeline is a later experiment, not the first optimization.
   Unreal's new Nanite Foliage feature is documented as experimental.

Sources: [Unreal Foliage Mode](https://dev.epicgames.com/documentation/unreal-engine/foliage-mode-in-unreal-engine),
[Unreal PCG generation modes](https://dev.epicgames.com/documentation/unreal-engine/using-pcg-generation-modes-in-unreal-engine),
[Unreal World Partition HLOD](https://dev.epicgames.com/documentation/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine),
[Unity terrain details](https://docs.unity.com/en-us/engine/6000.3/manual/creating-environments/script-terrain/terrain-grass),
[Unity terrain settings](https://docs.unity.com/en-us/engine/6000.3/manual/creating-environments/script-terrain/terrain-other-settings),
[Unreal virtual shadow maps](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine),
[Unreal Nanite foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage),
[Direct3D 12 indirect drawing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing).

## Ordered work

### 0. Establish a repeatable landscape baseline

- Capture a fixed and a moving route through the same meadow/woodland scene,
  including a camera turn, sprint, and teleport. Run Release and Debug at
  790 x 589 and 1920 x 1080, with the same quality settings on each backend.
- Record frame interval P50/P95/P99; foliage color, shadow, terrain, cull and
  upload GPU passes; CPU recording/streaming; visible tiles/placements; drawn
  triangles; VRAM; tile-build spikes. A/B grass off, grass shadows off, and
  individual species/LODs. Use a GPU overdraw capture to distinguish shader
  and fill costs from geometry costs.
- Decide a concrete target scene, resolution, hardware, and quality tier for
  the 100 FPS objective. Ten milliseconds is the entire frame budget, not a
  foliage budget.

### 1. Reduce expensive pixels and shadow work in the existing renderer

- For each current grass mesh/LOD, measure projected coverage, triangles,
  masked or transparent pixel area, and LOD transition overlap. Tighten
  foliage geometry and material masks; use simpler middle and far clumps and
  terrain-color coverage in the distance. Keep crossfade coverage stable.
- Add species-level color distance, shadow caster distance, and density scale
  to quality presets. Start with short near-field grass shadows and test a
  simplified shadow mesh/material for the next band. Preserve silhouettes and
  camera/shadow fade agreement in pixel tests.
- Tune tile granularity empirically: too-large tiles admit offscreen foliage;
  too-small tiles increase dispatch and metadata cost. Use the existing GPU
  culling and persistent pool as the baseline, with an optional hierarchy or
  depth-occlusion experiment only if the GPU capture shows missed culling.
- Gate: same-route visual comparisons on DX11/DX12/Vulkan, no popping or
  missing casters, and measured improvement in foliage color/shadow GPU time
  and P95 frame interval. Keep quality changes behind a preset rather than
  silently reducing existing authored scenes.

### 2. Make foliage authorable and streamable across a large world

- Add a reusable foliage species asset with mesh LODs, material, density,
  placement filters, wind/interaction flags, and shadow policy. Preserve
  `GrassField`/`TerrainObjectLayer` scene compatibility through migration.
- Store per-cell density/biome masks, painted add/erase overrides, exclusion
  paths, and stable seeds. Generate placements deterministically from terrain
  height, slope, layers, and species rules; rebuild only changed cells.
- Stream compact cell/tile placement data around viewers with prefetch and
  hysteresis. Bound generation, uploads, residency, and retirement per frame.
  Keep collision/scripts only for promoted nearby plants; use instancing for
  the rest. Avoid multiplying full prefab graphs just to represent ground
  cover.
- Gate: camera crossing, sprint, teleport, terrain edit, save/reload, and
  Play/Stop produce stable placements without visible holes or unbounded
  memory. Report peak upload/build time and residency as well as average FPS.

### 3. Add large vegetation and distant landscape representations

- Apply the same species/placement data to shrubs and trees. Use mesh LODs
  close, coarse instancing or impostors at distance, and cell-level HLOD only
  for content whose distant silhouette needs it. Keep tree collision and
  interactions on a small nearby subset.
- Author distant terrain and vegetation color together so ground-cover fade
  does not expose bare terrain. Add wind by distance/screen size; stop fine
  animation where it no longer changes the image, especially for shadows.
- Gate: distant vistas retain density and silhouettes while resident memory,
  draw work, and streaming spikes stay bounded as world size grows.

## Decision gates

- If grass-off barely changes GPU time, prioritize terrain/material work before
  foliage redesign. If shadows-off gives the largest gain, do shadow policy
  before changing placement storage. If color dominates, inspect overdraw and
  LOD assets before adding more culling compute.
- Re-run the matched route after each stage. Do not infer frame gains by adding
  overlapping CPU and GPU timers. A 100 FPS claim requires an average interval
  at or below 10 ms on the agreed route, with P95 and visual quality reported.
