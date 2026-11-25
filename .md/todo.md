# Todo

Open work only, as checkboxes. Finished items go to todo_history.md; notes, research and plans are in landscape_notes.md, foliage_notes.md, grass_notes.md and engine_notes.md. Code comments that cite "todo.md Foliage/Grass Phase N" or a todo.md section: the open items are here under the same heading, the description in the matching notes file.

## To investigate

### Terrain and grass detail at distance (2026-11-25, user: "steep terrain doesn't load in details soon enough", "no grass visible at the bottom of the hill and beyond, we should see it swaying")
Findings:
- Steep faces: past the loaded cells (`loadedRadius` 3 = ~400 m) terrain is the far volume
  (`TerrainManagerFarVolume.cpp`): unlit, far-palette vertex colour, no textures. The escarpment's faces
  stay flat tan until their cell loads, then switch to scanned rock (meadow walk end,
  `.local/transitions/p7far_walk`).
- Grass: from MeadowSlope the meadow below is far-ring and ground-cover territory. The far ring's tufts
  (1.1/m²) are under a pixel seen from above at 100+ m, and the ground-cover shells average to a flat
  colour there, so nothing reads as grass or moves. Detailed grass 50 -> 80 m and the far ring
  200 -> 400 m made no visible difference (`.local/foliage/gr_base_MeadowSlope_full.png` vs `gr_80_400_*`).
- Meadow walk, 1080p 6 m/s, interleaved, two runs each (`.local/routes/mw_{base,L4,G70}_r{1,2}`):

  | config | median ms | p99 ms | >50 ms | terrain GPU | grass GPU | camera rec | update | peak |
  |---|---|---|---|---|---|---|---|---|
  | base (loaded 3, grass 50) | 18.55 / 19.44 | 40.1 / 43.8 | 0 / 3 | 1.97 / 1.99 | 2.29 / 2.45 | 7.4 / 7.7 | 1.6 / 1.7 | 4.3 GB |
  | L4 (`ENGINE_TERRAIN_LOADED_RADIUS=4`) | 19.10 / 18.58 | 43.1 / 42.2 | 0 / 0 | 2.19 / 2.24 | 2.30 / 2.46 | 8.1 / 7.8 | 2.1 / 2.0 | 4.7-4.8 GB |
  | G70 (`ENGINE_LANDSCAPE_GRASS_RADIUS=70`) | 18.23 / 18.43 | 41.0 / 40.7 | 2 / 1 | 1.90 / 2.17 | 2.27 / 2.36 | 7.3 / 7.2 | 1.5 / 1.5 | 4.2-4.3 GB |

  L4: terrain GPU +0.25 ms, update +0.5, camera record ~+0.4, +0.45 GB, no new hitches; median within
  noise. G70: blades reach 80 m instead of 60 (reach = grassRadius + 10), but density already falls as
  1/d² past 7 m (~14 % more blades), so no measurable cost and no visible gain.
- Wind over distant grass (in progress, uncommitted): `GrassGroundWind` (Object.hlsl) shades the blades'
  broad gust fronts (same field, direction, speed) on the ground cover past the blades; terrain draws
  with ground cover now get scene time (`SceneRendering.cpp`). Two MeadowSlope captures 6 s apart
  (`ENGINE_WIND_TIME=10/16`, `.local/foliage/wind2_*`): bands faint on the slope, none on the lit plain
  below (presumably far-volume nodes, which skip this shading; not confirmed).
- A/B overrides added: `ENGINE_TERRAIN_LOADED_RADIUS`, `ENGINE_LANDSCAPE_GRASS_RADIUS`,
  `ENGINE_FAR_GRASS_RADIUS`.

Recommendations:
- [ ] Set `loadedRadius` 4 in `landscape_demo.scene` (cheap per the table). Re-check the forest walk and
      23 m/s runs before calling it settled; 5 not measured.
- [ ] Keep `grassRadius` 50: the radius is not the lever for distant grass.
- [ ] Make the wind bands read: confirm which surface draws the meadow plain from MeadowSlope; if it is
      the far volume, give its nodes ground cover + `GrassGroundWind` (they need scene time, view
      direction and the grass paint share, e.g. in a vertex channel). Then tune the strength (now
      `.34 * lean + .12 * |gust|`) against a moving capture (`ENGINE_WIND_FRAME_STEP`), not stills.
- [ ] Distant steep faces: textured rock on far-volume nodes (triplanar rock on the steep band) so faces
      past the loaded radius stop reading as flat tan; this would let `loadedRadius` stay lower.
- [ ] Far grass ring read from above: the tufts vanish at 100+ m from raised views; consider the ring's
      density/size or the ground-cover colour carrying it there instead.

### Volume-first conversion leftovers (section in todo_history.md, 2026-11-24/25)
- [ ] Frame rate goal not met: meadow walk median ~18.5-19.5 ms (~51-54 fps) against 100+ fps; forest walk,
      Vista and Escarpment not measured after the conversion; landscape median 15.8 -> 17.0 ms in one
      Vista-style run, re-measure with the walk runner.
- [ ] Far volume: terrain GPU ~1 ms over the deleted far tiles; peaks a little rounder.
- [ ] Near volume: frame p99 +7 ms, terrain GPU +0.55 ms, VRAM +110 MB vs the height grid; next a compact
      volume vertex (position, normal, packed layer weights, rock tint).
- [ ] Cell-crossing frames 74-98 ms (mostly `CommitPendingObjectChanges`, the new cells' objects).
- [ ] Remove the unused `lod2Radius`, `lod4Radius`, `maxLodChangesPerUpdate`, `batchFarCells`
      (TerrainManager, its serialization, landscape_demo.scene).
- [ ] Field seeds without painting (split the trail carve out of `PaintCell`; 5 ms a cold cell).
- [ ] Escarpment and cliffs as volume edits (content); click through the sculpt tools in the editor.
- [ ] `TerrainCapsuleTraversalTests` does not build (Scene::Start/Update gone; predates the conversion).

### Foliage Phase 7: World detail (2026-11-19, user request: paths, streams, small things, cliffs/overhangs)
- [ ] **Streams**: not done. The erosion flow map is too coarse (macro grid) to place a stream bed, and
      running water needs a carved channel in the height archive plus water surfaces that follow the
      slope (the water system is flat lakes). Plan: trace downhill polylines from springs in
      `build_landscape_demo.py`, carve them into the heights, export them, and draw ribbon water meshes.
- [ ] **Vines on trees**: not done (needs per-trunk geometry or a trunk-wrap generator). Ground ivy only.
- [ ] **More terrain materials** (mud, moss, leaf litter, scree): needs more than six paint layers in the
      terrain shader, or a detail/decals layer.
- [ ] Scanned shrubs (Poly Haven shrub_02-04, nettle_plant; alpha-masked, so they need card-friendly LODs)
      to replace the generated bushes. The small scanned props have not been checked close up in a
      capture, and the mossy rocks show little moss.

### Foliage Phase 2: Pay for it (performance)
- [ ] **Moving camera, remaining hitches.** Walking still has clusters of 80-120 ms frames each time a
      cell boundary is crossed (3 clusters in 400 frames): `update_post_physics_ms` 1.3 → 22,
      `update_objects_ms` 1.6 → 13, `prepare_graphics_ms` 0 → 7, `stream_lod_ms` 0 → 6. Walking also
      raises the steady cost (update 1.8 → 4.6 ms, prepare 4.7 → 8.9). Profile a walking run
      (`--drive-forward 380 --cpu-sample`) and spread that work over frames. Every later performance
      check uses the three moving runs as well as the still poses.
- [ ] **CPU draw recording on the vista** (Phase 0: camera record 18 ms, shadow record 6 ms, scene prepare
      11 ms, run serially; now 11 / 3.9 / 6.6). Candidates beyond the steps above: moving more far forest
      onto GPU-driven instance draws (InstanceCull) and overlapping prepare with recording.
- [ ] **Standalone prefab trees**: find why `shadow_submission_ms` is ~64 ms for 84 non-instanced prefab
      trees (Phase 0 finding; `build_foliage_test_scene.py --standalone` reproduces it). The fine shadow
      timers (`geometryDrawMs`, `maxBatchMs`) only record with `Scene::SetUpdateProfilingEnabled`, which
      only the editor profiler sets; expose it to GameBackendCapture first.
- [ ] Follow-ups of **Temporal upscaling** (done; todo_history.md):
      - [ ] Landscape demo walk at 0.67 (the 1080p FieldWalk load did not finish in 10 minutes on this
            machine at native scale on 2026-11-23; stopped).
      - [ ] Scanned bushes' coarse level looks broken (eroded leaves, a dark card) wherever it shows,
            e.g. near the camera at 1280x720 native: an asset LOD problem, not upscaling.
      - [ ] UI draws into the scene pass, so it renders at the render size and goes through TAA.
      - [ ] DX11 and Vulkan ignore the scale (render at full size).
      - [ ] Grass minimum pixel width and other in-shader pixel-size rules still follow the render size.

### Foliage Phase 4: Forest floor and undergrowth (RDR2 / GTA VI shots)
- [ ] Grass clumping (Grass Phase 7 open item). Dry blades: their share went 10 % → 22 % in Foliage Phase 1d
      and `dry_grass_clump` grows on the forest floor; a dry species mixed by the dry/lush noise is open.

### Foliage Phase 6: Air and camera
- [ ] Froxel volumetric fog (sun shafts through canopy, RDR2/pine shots). Re-use the cascade shadow; ~160x90x64
      froxels, temporally reprojected; budget ≤ 1 ms. Replaces the per-pixel shaft march in the forward shaders.
- [ ] Depth of field (shots 2, 4, 5): half-res gather bokeh, off by default, for cinematic/photo mode.

## Grass


### Grass Phase 3: Density falloff with width compensation (coverage at mid range)
- [ ] Re-evaluate the fixed LOD distances (7 / 13 m in LandscapeDriver). With smooth thinning, push LOD
      switches further out, or regenerate LOD1/LOD2 meshes with less aggressive sward removal.
- [ ] Consider Tsushima-style view-space widening of blades seen edge-on (GodotGrass reference).
- [ ] Use the saved cost to push the grass radius out, or hold it steady under the detail budget.

### Grass Phase 4: Lighting as one surface
- [ ] Sky (SH) ambient instead of the flat `ambientUnlit` for grass and meadow plants (`PSGrassMain`,
      `PSFoliageMain`); changes meadow brightness everywhere, so it needs its own tuning pass. Canopy shade
      already scales the ambient (Foliage Phase 3).

### Grass Phase 5: Build tiles ahead
- [ ] Create and build tiles in a ring beyond the radius (`retentionRadius` exists) so they are resident
      before they reach the fade band.
- [ ] Prioritise tiles in the direction of travel (dot of tile offset with viewer velocity) over pure distance.

### Grass Phase 6: Grass texture past the radius (flat distance)
- [ ] Shell texturing is likely too expensive on the current low-spec target; revisit only if 6a/6b fall short.

### Grass Phase 6½: Antialiasing (grass edges stair-step and shimmer)
- [ ] MSAA and TAA are done on DX11 and DX12 (todo_history.md); Vulkan has neither (renders without MSAA, no
      jitter applied). Not exercised: live MSAA switching in the editor's DX12/DX11 views. Capture overrides:
      `ENGINE_MSAA`, `ENGINE_FXAA`, `ENGINE_TAA`, `ENGINE_GRASS_MIN_WIDTH`, `ENGINE_AUTO_EXPOSURE`.

### Grass Phase 8: Larger, thicker foliage
- [ ] Bushes share the grass radius (~36 m); larger bushes should reach further (their own range or a
      props/foliage layer).

### Grass Phase 7: Mesh/coverage tuning (optional)
- [ ] Clumping (Voronoi cells with shared height/colour/facing) to make the field read as real grass while
      gaps look natural.

## Landscape look (large-scale vista with layered haze)

### 0. Baseline + test shot
- [ ] Profile and cut CPU frame cost for the vista (camera command recording, scene prepare)
  - Why serial: `SimulationLoop` (Game/Application/GameApplication.cpp) runs Update, then PrepareRenderFrame, then
    Render (shadow + camera recording) on one scene-owner thread while the window thread waits; only replay is on the
    render thread. Overlapping update N+1 with recording N needs a scene snapshot boundary; bigger change, do after the
    per-stage cuts below.
  - [ ] Shader compile time itself: Object.hlsl ~21 s per entry in fxc (the fog include added ~2 s). Look at what
    fxc unrolls/flattens (lighting loops, terrain layers); cold-cache startup is still ~65 s
  - [ ] GrassField::LoadSources (0.18 ms) re-derives species LOD paths from string maps every frame, including a
    by-value copy of `ObjectLodProvider::GetObjectLodMeshes()`; needs revisions on its inputs to memoize

### 1. Aerial perspective + height fog

#### Light shafts (shadowed in-scatter) are done (todo_history.md)
  - [ ] Sky pixels are not shadowed (the skybox has no shadow map), so shafts never cross open sky
  - [ ] Animate the shaft dither now that TAA exists (on by default since 2026-11-19; the LOD dissolve dither
    already rotates per frame, Foliage Phase 1b); 4-6 samples may then suffice

### 2. Terrain scale + erosion (Tools/build_landscape_demo.py)
- [ ] Far-terrain silhouettes are still the 128 m polygon outline (visible on peaks against the sky). A finer far
  grid (e.g. 32 m from the archive's 16 m samples) needs sub-cell vertices in the far-tile mesher
- [ ] Editor tool: terrain generation window (size, relief, seed, erosion iterations) that regenerates and reloads the landscape

### 3. Distant terrain color + forest coverage
- [ ] Macro colors use the materials' authored means while the near cells' distant blend uses measured layer-array
  means; at 0.85 brightness distant meadows read a little duller than textured near meadows. Tune, or feed the
  measured means into the composer

### 4. Lighting + post
- [ ] Long-range shadow for non-terrain objects (far trees/impostors stay sunlit inside a ridge shadow; needs a global texture slot on all three backends)
- [ ] Optional authored 3D LUT input for grading (the split tone is analytic today)
ph
### 5. Sky computed from the sun

#### Physical aerial perspective, volumetric clouds, the cloud-shadow cookie and the half-resolution clou
  - [ ] Froxel aerial-perspective volume (32x32x32, compute) once 3D textures exist; would add cloud/terrain
    shadowing of the in-scatter beyond the current shaft march
  - [ ] Cloud shadows that sit under the actual volumetric clouds (project the coverage/density toward the sun)
  - [ ] Temporal reprojection (march a quarter of the pixels per frame) if the march needs to get cheaper still
  - [ ] Clouds in portal views and fog/aerial perspective between camera and cloud

### 7. Demo composition
- [ ] Final landscape demo vantage points: the MeadowSlope and Escarpment viewpoints of the expectations section
  (sunlit meadow, forest edge, forested hills behind). The earlier idea (elevated camera over a valley, backlit
  sun, dark foreground silhouette) stays as the Vista viewpoint, not the main shot
- [ ] Second reference set (`.local/reference-images/`): still water with reeds, lily pads and reflections (01-03);
  tall flowering meadow to the horizon (04, 05); sun haze between conifer trunks and a foggy broadleaf forest
  (01, 06, 07); layered ridge haze (08). Today the far forest reads as a flat light-green wall and the meadow
  foreground is dark (`.local/foliage/mv2_walkturn.png`)
