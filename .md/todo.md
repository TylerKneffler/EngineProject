# Todo

Open work only, as checkboxes. Finished items go to todo_history.md; notes, research and plans are in landscape_notes.md, foliage_notes.md, grass_notes.md and engine_notes.md. Code comments that cite "todo.md Foliage/Grass Phase N" or a todo.md section: the open items are here under the same heading, the description in the matching notes file.

## To investigate


## Terrain: volume-first conversion (2026-11-24, user request: "convert fully to sculpt and remove height map dependency")

Decision (user, 2026-11-24): **volume-first, heights as seed.** Every runtime consumer (near and far
rendering, collision, tree/grass/prop placement, paint, brushes, ground queries) reads one terrain field;
its base comes from a generator (today the baked `world.heights`), with sculpt edits stored sparsely per
cell. Not a fully stored world volume (~0.8 GB at 4 m, tens of GB finer), not "volume near, heights far".
Starting point: branch `terrain-far-cell-detail` (87d04fa9). Sculpts/stamps were removed in 9f84ec99 in
favour of `VolumeTerrain` (heights + `.volumeedit`); `volumeCells` is off in `landscape_demo.scene`.
Height reads today: 31 files (far tiles and far-cell detail, streaming, colliders, paint, hydrology, sun
shadow, trail paths, forest/prop/grass placement, far objects, macro colour, region edits and brushes,
first-person controller, caches).

- [x] **0. Baseline** (2026-11-24, branch `terrain-far-cell-detail`). Meadow walk 1080p, two interleaved
      pairs, height grid / `ENGINE_VOLUME_TERRAIN=1`: median 18.2-19.6 / 19.7-20.0 ms, p99 39-40 / 59-66 ms,
      terrain GPU 1.15 / 1.84 ms, VRAM 1.69-1.72 / 1.83-1.98 GB, crossing frames 91-124 / 98-119 ms.
      Look (`.local/transitions/volume_baseline.png`): Escarpment close up, plaster face with a grid
      pattern / scanned rock (better; tile repeat visible). From the meadow trail (t903) no difference:
      the escarpment's bare faces there are far tiles either way (flat tan), so they only change with 4-5.
- [x] **1. Terrain field API** (2026-11-24). `Components/Terrain/TerrainField`: `Density`, `Normal`,
      `SurfaceAt(x, z, from)` (first air-to-ground crossing going down: open sky by default, under an arch
      from below it), `Raycast`, `HasEdits`, `SeedHeights`, `Invalidate`. Evaluated exactly (the density
      VolumeTerrain meshes: seed ground, then own and neighbours' volume edits in FeedVolumeNeighbours
      order), never from built volumes; cells cached (LRU 2048), thread-safe. `TerrainManager::
      GetTerrainField()` (shared_ptr, safe on workers): seed = procedural builder (it sinks trail tracks
      0.12 m, so the raw archive was off by that) else archive, plus `.terrainedit` heights for cells the
      manager treats as edited (a stale `-1_0.terrainedit` that loaded cells ignore was 7.9 m off), plus
      live editor edits (RecordEditedCells); AddVolumeEdit invalidates. Checks: `TerrainFieldTests`
      (plain grid exact, crater matches the meshed volume to 2 mm near the surface, edits across a cell
      border, arch top vs ground under it, invalidation, 8 threads with an evicting cache) and
      `ENGINE_TERRAIN_FIELD_CHECK=1` on the landscape: 49 loaded cells, 100,352 samples, 0.0000 m worst.
      Cost: 0.044 us a warm query; 5 ms a cold cell (the builder paints as it seeds).
      - [ ] Before 5 (far cells, ~16k): seed without painting. Split the trail carve out of `PaintCell`
            (it reads the paint's `crossable` layers) so a seed costs the archive read, not 5 ms.
- [x] **2. Near terrain is volume** (2026-11-24). `TerrainManager.volumeCells` (+ sample spacing, steep
      layer) is saved with scenes and in the inspector; `landscape_demo.scene` and build_landscape_demo.py
      turn it on. Main-thread spikes fixed (meadow walk, update p99 21 -> 9 ms, = height grid):
      chunk meshes built with bounds on the meshing worker and their vertex buffers made there; GPU
      buffers installed as chunks arrive under a shared 2 ms/frame budget, not at the group swap (up to
      96 ms); chunks make their own buffers instead of `AcquireMeshBuffer`, whose one lock is held while a
      worker creates a large buffer (tiny installs waited 25 ms; `Mesh::InstallOwnBuffers`); replaced chunk
      meshes released 1 ms/frame (`Mesh::TakeRuntimeLod`; swaps released ~100 at once, 7 ms a cell); level-0
      colliders kept across seam-only swaps (post-physics p99 7.2 -> 1.05 ms). Holes: loaded volume cells
      draw at any distance (600 m left the loaded area's far corners empty); far seam cells hang an 8 m
      skirt under edges bordering loaded cells (volume edges are not the far edge: a slit showed the lake
      plane at MeadowSlope; same slit with the old 600 m limit, so it predated this). Cell crossings
      unchanged (88-116 ms both). Logs: `ENGINE_VOLUME_TERRAIN_LOG=1` (slow updates, installs, swaps),
      `ENGINE_SHADER_COMPILE_LOG=1` (FXC compiles; an uncached volume shader variant compiled through
      every earlier volume measurement).
      Distant steep faces as cliff rock (user, 2026-11-24): the macro colour map and far tiles blend to the
      scanned cliff rock's colour on the volume's steep band (1 - normal.y .25 .. .45); far tiles use their
      vertex normals (`FarTerrainPalette::steepColor`). The alpine range's faces went from pale to dark rock.
      - [ ] Still behind the height grid: frame p99 +7 ms (38.8 vs 31.9, mostly update hand-off waits),
            terrain GPU +0.55 ms and VRAM +110 MB (chunks use the 264-byte mesh vertex and ~6x the draws).
            Next: a compact volume vertex (position, normal, packed layer weights, rock tint).
- [x] **3. Queries move to the field** (2026-11-24). Forest, props, undergrowth, `GrassField`/`GrassBlades`, far
      objects, first-person controller, streaming camera, `LandscapeDriver` placement: ground from
      `SurfaceAt` (upward faces only, so nothing grows on overhang ceilings or inside carved holes).
      - Placement: `ObjectLayerContext::field` (set only where `HasEdits`) and
        `ObjectLayerContext::SampleSurface`; forest, edge belt, props and scree go through it, for
        streamed, synchronous and far-object cells. No edits: the same code as before (heights).
      - Grass: `GrassField` keeps the field for its cell (`TerrainManager::EditedFieldFor`, refreshed
        on `GetTerrainFieldRevision`); CPU tiles sample `SurfaceAt`, GPU blade heights come from it,
        and blade paint is 0 where the surface normal.y < .7 (crater walls, faces).
      - `TryGetHeightAtWorldPosition` (first-person controller, streaming camera, perf driver): the
        first ground below 2 m above the query where edits reach the cell, else the top surface.
      - Checked: a test shaft (`subtract capsule 79 40 19 79 400 19 15 2` in `-3_-8.volumeedit`, seen
        from above MeadowSlope) cuts rock walls, grass stops at the rim, nothing floats over the
        hole; without edits the landscape takes the unchanged height path.
      - Not moved: `LandscapeDriver` far splat trees (archive heights, far cells, no field yet), grass
        path-share test (paths only).
- [x] **4. Paint on the volume** (2026-11-24). Layer weights, hydrology tint, lushness and trail contour paths
      evaluated on the field per vertex (slope from the density gradient), not per height sample.
      - `VolumeTerrain::surfacePainter`, made per cell by `TerrainManager::SetSurfacePainter` (fed in
        `FeedVolumeNeighbours`, so cached cells get it too); `LandscapeDriver::MakeSurfacePainter` runs
        `PaintSample` + `ApplyErosionPaint` at each vertex's own position and height. On the seed ground
        the rock codes and the 32 m erosion slope come from the seed grid (`ErosionSlopes`, shared with
        `PaintCell`); 0.5-2 m off it, or facing down, from the vertex normal. Steep blend and scanned
        rock unchanged. `ENGINE_VOLUME_PAINT=0`: the painted grid, interpolated.
      - Fixed on the way: volume vertices had no lushness (uvChannels1.w); now the painter's, or the
        grid's interpolated.
      - Trail contour paths were already per pixel (world position and height), nothing to move.
      - Checked: Vista and MeadowSlope near ground vs the height grid |diff| 1.55 / 2.51 (grid paint
        1.68 / 2.68); test shaft walls rock as before. Cost 0.43 us a vertex on meshing workers
        (grid 0.14), 26 ms over all loaded cells (`ENGINE_VOLUME_TERRAIN_LOG` "paint so far").
- [ ] **5. Far terrain from the field.** Far chunks meshed from the field at coarse strides (surface nets
      levels with skirts, screen-error selection, the far-cell detail lessons: no pops at the loaded
      border, compact static buffers, no crossing hitches); replaces far tiles, `world.fargrid` and
      far-cell detail. Sun shadow height map, macro colour and far objects read the field.
      - [x] (2026-11-24) Done as field-first far tiles, not a second far renderer: where no volume edit
            reaches a cell the field *is* its seed heights, so far tiles keep meshing those (quadtree,
            screen error, far-cell detail levels, compact static buffers, disk cache: all the lessons
            kept, nothing to re-tune). Cells edits reach (`TerrainManager::FarVolumeCells`, from
            `VolumeEditOwners`, a one-time scan of `*.volumeedit`) are meshed from the field instead:
            seed heights + neighbours' (`TerrainField::SeedHeights`) and the edits, `BuildTerrainVolume`
            at 128 m / level (4-16 m), surface nets with a 2x spacing skirt, coloured by the far palette
            (steep faces rock). Edits are in the tile cache fingerprint. Stride-1 tiles only (coarser
            parents: an edit is under a cell, a few pixels there). Checked: a 200 m pillar and a 56 m
            shaft in far cell (1,-7) from ~650 m (`.local/transitions/Far_cmp.png`).
      - [x] `TerrainField::HasEdits` no longer seeds the cell (it did: 5 ms through the landscape
            builder, on the main thread for every streamed cell since phase 3, and on workers for every
            far object cell); it reads only the 3x3 edit lists.
      - [x] Cell crossings: volume cells no longer re-stride their hidden height mesh (stream_lod_ms
            18-21 ms, up to 30). Meadow walk (2 runs each) crossing frames 95-122 -> 74-98 ms, p99
            47-50 -> 43-49 ms, median 19.4-20.8. What is left there is `update_post_physics_ms` 17-22 ms,
            which is the scene commit at the end of the update (`CommitPendingObjectChanges`: the new
            cells' objects), not terrain.
      - [x] Holes in loaded volume cells at coarse levels (found from 500 m: a dark patch showing the
            water plane in cell 4,-9; `ENGINE_VOLUME_TERRAIN_CHECK` counted 4,393 empty columns over 49
            cells). `BuildTerrainVolume` put the bottom on the 32-sample lattice but not the top: a
            volume 31 cells tall left level 3's last cell (samples 24..32) reading past it, so ground
            near 48 m had no vertex. The top now rounds up to the lattice too; the check reads 0 holes,
            0 open edges inside or on cell borders. Reproduced offline with `.local/tests/
            VolumeHoleRepro.cpp` (re-meshes an `ENGINE_VOLUME_TERRAIN_DUMP` cell as the jobs do).
            Predates this phase (phase 2), seen less from the ground: distant loaded cells at level 3.
      - `ENGINE_VOLUME_SKIP_LOD=0` re-strides volume cells' hidden height meshes again (A/B).
      - Not moved (reasons): sun shadow height map (128 m samples, below an edit's size); macro colour
            (16 m texels, paint rules unchanged); `world.fargrid` stays the horizon beyond the archive
            (no field there); synchronous far-tile path (async off) keeps the height grid for edited
            cells; runtime `AddVolumeEdit` in a far cell is not pushed to built far tiles until they
            rebuild. Far objects read the field since phase 3. Field seeds for far cells still go
            through the painting builder (5 ms; only edited cells and their neighbours now).
- [ ] **6. Sculpting.** Editor brushes on the volume (raise, lower, smooth, flatten, add rock, carve) with
      undo, writing per-cell edits; `TerrainRegionEdit`/`TerrainManagerEditing`/`TerrainBrushShape` move
      to it. Escarpment and cliffs re-made as volume edits (rock faces, overhangs).
      - [x] (2026-11-24) Terrain tools Rock, Carve (Shift swaps) and Ledge (level to the stroke's start
            height: cut a box above, fill a slab below). They aim with `ScenePlacementAndPicking::
            FindFieldSurfaceHit` (TerrainField::Raycast: overhangs, holes) and lay capsules from the last
            dab each third of a radius across the ground (holding still adds nothing). Hardness sets the
            blend. Need a Terrain Manager.
      - [x] Undo: the manager's scene state holds every cell's edit list (Serialize `volumeEdits`);
            Deserialize (load, undo, redo) writes back the `.volumeedit` files that differ (within the
            files' six digits) and removes those the scene has none for, then refreshes the field and the
            loaded volumes (`SetVolumeEdits`, `RefreshVolumeCells`). Scenes without the key (generated
            landscape) leave the files alone. Once a scene is saved with it, the scene is the source: a
            `.volumeedit` added by hand after that is removed on the next load.
      - [x] Objects: `AddVolumeEdit(edit, followObjects)` / `FollowVolumeEdit`: placements under an edit
            that end up buried rise to the top surface, ones left in the air drop to the ground below,
            ones then on ground steeper than ~60 degrees (or none) are removed.
      - [x] The field seeds authored cells (scene-saved, no builder or archive) from their loaded heights
            (`SeedAuthoredCells`), so picking, placement and follow work on authored terrain too.
      - Test: `TerrainEditingToolsTests` TestVolumeSculpt (carve saves the file, field sees the shaft,
        object drops to its floor, the other stays; snapshot before/after removes and restores the file;
        reloading the same scene does not rewrite it). Vista unchanged (clouds and wind only).
      - Kept as seed edits, by the volume-first decision (heights as seed): Raise, Lower, Smooth, Flatten,
        Paint, Stamp and the region operations (erosion, terrace, noise) still edit the seed heights, which
        the volume meshes; no need to move `TerrainRegionEdit`/`TerrainBrushShape`.
      - [ ] Escarpment and cliffs as volume edits: content work, not done. The tools are there to do it
            by hand, or a script could add overhang edits along the steepest faces.
      - Not checked by hand: the tools in the running editor (UI paths built, not clicked through).
- [ ] **7. Remove the height dependency.** Progress (2026-11-24, user: "convert all scenes then delete"):
      - [x] Every terrain is a volume. Terrain Managers always give cells volumes (`volumeCells`,
            `compactCellVertices`, `ENGINE_VOLUME_TERRAIN`/`ENGINE_COMPACT_TERRAIN` removed); a Terrain of
            its own gets one (`Terrain::EnsureVolume`: fromTerrain, spacing = its grid, drawn and collided
            at any distance). Scenes need no edit: foliage_test, grass_test, spin_test, stamp_test and the
            .local stamp/forest scenes convert on load. Volumes follow their Terrain
            (`GetSurfaceRevision`); height brushes also refresh the neighbours' volumes.
      - [x] Deleted the height-grid cell mesh: `Terrain::Rebuild` builds no mesh (vertices, baked LODs,
            compact cells, row rebuilds, LOD striding, lateral offsets all gone), streaming hands over
            heights/weights/lushness only, the cell cache is version 8 (heights, weights, lushness),
            render batches and grid colliders are gone. Far tiles leave a loaded cell out once its volume
            shows (`VolumeTerrain::IsShowing`; they checked the deleted mesh and drew flat green over the
            meadow, hiding grass and trails, until fixed).
      - Checked: before/after captures of all five tracked terrain scenes: landscape |diff| 1.8, grass 2.4,
        spin 2.5, foliage 4.5 (wind), stamp 11.7 (texture tiling phase: the volume's triplanar layers).
        Brush dab on a 256x256 cell 1.6 -> 0.87 ms (no mesh). TerrainEditingTools and TerrainField pass.
        `TerrainCapsuleTraversalTests` does not build (Scene::Start/Update gone; predates this).
      - [ ] Landscape median 15.8 -> 17.0 ms in one Vista-style run: measure with the walk runner.
      - [x] (2026-11-25, user: "delete the far tiles too") Far tiles, far-cell detail, the far tile
            worker, cache and LOD edges are deleted. Distant terrain is volume
            (`TerrainManagerFarVolume.cpp`): a quadtree of nodes 1-64 cells across, each surface nets of
            the field's density on 32 x 32 (4-256 m); a node splits within `farVolumeDetail` (2) node
            sizes of the viewer, nodes over loaded showing cells split to single cells and leave those
            out, so near and far tile exactly. Density from edited cells' heights, else the archive in
            memory (finer than a cell), else the 128 m corner grid; volume edits included. Skirts,
            far palette colours, corner grid as occluder, compact static buffers, 4 workers. The set
            is chosen per `farVolumeStepMetres` (32) and swaps in whole when built. Cells unload once a
            shown node covers them (nodes only leave out showing cells inside the loaded radius).
            `ENGINE_FAR_VOLUME_LOG`, `ENGINE_FAR_VOLUME_DETAIL` (A/B).
            Fixed on the way: volumes stored density in int16 at 256 units/m (+-128 m), so coarse
            volumes (band 2x spacing, up to 512 m) clamped into terraces, spikes and skirt walls;
            units now scale with the band.
            Checked (2560x1440): Vista/MeadowSlope look like the far tiles; detail 1.5/2/3 = ~200/300/
            550 nodes, 0.4/0.56/1.0 M vertices; MeadowSlope terrain GPU 1.81 (tiles) -> 2.85 ms at 2,
            frame 23.3 -> 22.4 ms. Meadow walk median 19.4, p99 43-45, 0-2 frames > 50 ms (phase 5:
            6-8). Burst capture of the walk: no step over 203 px (no pops or holes). The edited
            escarpment cells now show at every distance (far tiles only drew edits up close).
            `world.fargrid` stays as the corner grid (sun shadows, occluders, coarse nodes).
      - [ ] Far volume: terrain GPU still ~1 ms over the far tiles; peaks a little rounder.
      - [ ] Unused now: lod2Radius/lod4Radius/maxLodChangesPerUpdate (cell mesh LODs), batchFarCells.
      Original plan: Delete the height-grid cell mesh path, far tiles, far grid and
      direct `Terrain::heights` readers; the archive is only the seed generator's input; caches and
      scene format migrated; meadow walk, forest walk, Vista and Escarpment at or above today's frame
      rate (RTX 4090 target: 100+ fps on large landscapes, 60 floor with rich foliage).
- [ ] Open from the far-cell detail work: cell-crossing frames 105-139 ms vs 52-69 ms before (fold into 5).

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
