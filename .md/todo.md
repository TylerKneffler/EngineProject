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
- [x] Gaps where loaded cells meet distant nodes (2026-11-25; white skirt walls at the escarpment foot,
      z = -640 from the hill pose). Border chunks were at a coarse LOD (8-16 m) beside 4 m nodes, and
      each side computed its own edge heights. Now border vertices come from the face's own crossings
      (`MeshSurfaceNets` `snapped`), border chunks mesh at the node spacing (`VolumeTerrain::BorderLevel`),
      and nodes use the seam mesher too, so both sides share their edge vertices; skirts remain as a
      backstop (`SurfaceNetsSeams::skirt`). Open: the hole checker hasn't been rerun since; small
      mismatches can remain on steep ground (density is scaled by each side's own normal).

### Foliage Phase 7: World detail (2026-11-19, user request: paths, streams, small things, cliffs/overhangs)
- [ ] **Streams**: not done. The erosion flow map is too coarse (macro grid) to place a stream bed, and
      running water needs a carved channel in the height archive plus water surfaces that follow the
      slope (the water system is flat lakes). Plan: trace downhill polylines from springs in
      `build_landscape_demo.py`, carve them into the heights, export them, and draw ribbon water meshes.
- [ ] **More terrain materials** (mud, moss, leaf litter, scree): needs more than six paint layers in the
      terrain shader, or a detail/decals layer.
- [ ] Scanned shrubs (Poly Haven shrub_02-04, nettle_plant; alpha-masked, so they need card-friendly LODs)
      to replace the generated bushes. The small scanned props have not been checked close up in a
      capture, and the mossy rocks show little moss.

### Foliage Phase 2: Pay for it (performance)
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

### Editor parity audit (2026-11-25)
Audit of `landscape_demo.scene` against the rule in landscape_notes.md ("every landscape feature usable from the
editor"). Achievable in the editor already: render settings, sun, viewpoint cameras, MeadowWalkCamera + spline
routes (tedious by hand), mist/cloud prefabs, TerrainManager scalar settings, sculpt/volume edits.

**Direction (user, 2026-11-25).** The terrain's stored data is the single source of truth, and everything derived
from it follows manual sculpting and painting. Content is authored and saved, not generated by the landscape at
runtime.
- [x] Bake erosion into the terrain's data (2026-11-25). `Terrain::erosionFlow` / `erosionDeposition` per vertex;
  the archive carries them in an erosion section after the cells (`TerrainHeightArchive::ErosionLayout`, baked
  losslessly from the old maps, which are deleted with `TerrainHydrologyMap`); cell caches (version 9),
  `.terrainedit` (version 2) and authored cells keep them. Erosion ops (hydraulic, thermal) lay sediment and
  hydraulic adds its drained flow in the baked map's units (`DrainedFlow`: catchment m^2, lush past ~13,000 m^2);
  shaping (raise, lower, flatten, smooth, noise, terrace, stamps) covers sediment and raised ground halves the
  flow per metre. The volume painter reads them per vertex. **Superseded 2026-11-25:** once the paint was
  baked (below) nothing read flow or deposition, so the erosion section (cut from `world.heights`, 23.1 to
  11.5 MB), `Terrain::erosionFlow`/`erosionDeposition`, `UpdateErosionAfterSculpt`, `DrainedFlow` and the
  `.terrainedit` erosion entries were removed (edit files are version 3; version 2 files still read, their
  erosion entries skipped). Erode strokes change heights only
- [x] Rebuild height-derived data from the actual surface (2026-11-25). Every edit (brush, region op, paint,
  volume edit) marks its cell and the eight around it (`TerrainManager::MarkMacroCellsDirty`, from
  `RefreshVolumeCells`); `MacroColorSource::makeCellUpdate` recomposes them and `PatchMacroColorImage` writes
  them over the map, and a whole build marks the edited cells so saved edits show from the start
  (`ENGINE_TERRAIN_MACRO_LOG=1` logs builds and patches). LandscapeDriver's patch: heights and slopes from the
  terrain field (sculpts, volume edits, unsaved strokes), rock class from the surface's slope where it left
  the archive (generator thresholds), the cells' erosion and hand paint, canopy from loaded cells' real trees
  (generated for unloaded ones). Far grid corners and patches take saved height edits at
  `ConfigureFarTerrain`; far corner colours (`gridColors`, used without a macro map) follow patches. Hand
  paint now shows on landscape cells (`Terrain::paintOverride`, removed with the painter: paint is the cells'
  weights, baked then hand-painted). Open: the whole build still starts from the archive (edited cells patched after);
  an editor undo that restores a cell through scene deserialization may not mark it dirty; checked by tests
  and a landscape run (39 edited/neighbour cells patched), not yet by sculpting in the editor
- [x] Rule-based placement as an editor tool: paint forests and props with a brush or region (Select tool) that
  runs placement rules (species mix, altitude bands, slope limits, edge belt, spacing, trail clearance) and
  **saves the placements** in the cell data. Rules are tool settings/presets, not runtime behaviour
  - [x] Brushes carry the rules (2026-11-25). `.brush` moved to `Core/Brushes` (version 2): optional Relief
    (height map, ratio, facing, backed), Terrain (tool, Rules: height/slope bands with fades, on/avoid paint
    layers named by material, so a brush means the same ground on any terrain) and Placement (weighted assets with scale ranges, density, min spacing, align, yaw, sink). The
    layer a stroke paints or places into is chosen in the toolbar (Paint layer combo; Objects tool layer
    combo with a new-layer field), never stored in the brush. Terrain tools pick a brush from the toolbar (any `.brush` under Assets, or
    dropped): tip shape, falloff, rotation, scatter and pen pressure apply to sculpt, paint and place;
    rules gate every stroke and the Select tool's region paint; choosing a brush takes its tool. The Objects tool with a Place brush runs `ApplyPlacementBrush` (Core, so scripts can stroke the
    same way); without one it keeps the single-prefab scatter. Inspector edits the new sections.
    Stamps are relief brushes: `tools/stamp_brush.py` converted the five (TerrainData/Brushes), the
    baker writes brushes too, and the engine's `.stamp` loader is gone (the `.stamp` files stay as bake
    sources for the stamp pipeline's Python tools). Examples: Meadow grass (paint), Meadow bushes (place),
    Oak stand (place)
    placement density/spacing/weights/erase)
  - [x] Layer settings (2026-11-25): `TerrainObjectLayer::Settings` per layer name on the manager (view,
    detail and shadow distances; saved with the scene), set from the Objects tool's "Layer..." popup; new
    layers (painted, scattered, restored) take them, changes reach every loaded cell's layer. Far blocks
    read saved layers (files), so painted trees with LOD chains draw far; the macro canopy patch counts
    every placement with a known crown (painted ones, saved ones of unloaded cells), placing marks its
    cells dirty, and a whole build marks cells with object files
  - [x] Storage (2026-11-25): procedural cells' objects go to `<dir>/<x>_<z>.objects` (binary asset
    format, with a revision), written on save or unload only when the cell's layers differ from what it
    loaded with; untouched generated cells write nothing and the scene keeps no placements of procedural
    cells. Undo/redo: the scene lists each file's revision and a session history restores them. Older
    scenes' saved layers of procedural cells move to files at the next save
  - [x] Rules (2026-11-25): edge band (`maxLayerShare` on the "on" layers: a shrub belt where forest
    soil meets meadow), trail clearance (`TerrainManager::TrailDistance`, trail objects since stage 3), per-asset height bands
    (species by altitude). The generator's own belt/trail/species logic stays until forests are painted
  - [x] Region placement (2026-11-25): Select tool "Place objects" runs the Place brush over the
    selection (`ApplyPlacementRegion`, Shift thins); the brush and object layer controls show there
- [x] Stop generating forests and props at runtime (2026-11-25). The whole map's trees, rocks and bushes are
  baked into an object archive, `TerrainData/Landscape/world.objects` (`TerrainObjectArchive`, `TRNROBJA` v2:
  per-cell columnar zlib blocks, layer templates stored once; 16.1 M placements, 144.6 MB at full precision),
  named by `TerrainManager::objectArchive`. Lookup per cell: loaded layers, then the cell's `.objects` file
  (an edit wins), then its archive block. Layers "Landscape forest", "Landscape forest edge" (near-only extras,
  `farObjects=false`) and "Landscape props". LandscapeDriver no longer sets a generator
  (the runtime path is removed); far blocks, the canopy whole build and
  the macro patch read saved placements (archive signature in `MacroColorRevision`). The baked forest first looked sparser than the
  generated one (clearings along the trail): `TerrainObjectLayer`'s packed placements wrote their asset table in
  name order but numbered assets by first appearance, so loaded layers swapped species (rocks for bushes, big
  oaks for edge shrubs). Fixed 2026-11-25 (layers saved before keep their swapped assets); a check against
  the old generators then matched 4,750 of 4,768 placements around the hill pose, and the hill capture
  matches the generated one. The generators (`GenerateForest`, `GenerateProps`, species, reach tables), the
  bake and check code and `tools/LandscapeBake` were then removed (2026-11-25; in git history): the archive
  is the forest now, edited by hand or by placement brushes
- [x] Bake the rule-based paint into the terrain data (2026-11-25, stage 2). `TerrainData/Landscape/world.paint`
  (`TerrainPaintArchive`, `TRNRPNTA` v1: per cell, zlib planes of u8 layer weights plus lushness; 35,721 cells,
  28.7 MB), named by `LandscapeDriver::paintArchive`. The builder loads it as
  the cells' paint (hand paint in `.terrainedit` on top); the vertex painter (`SetSurfacePainter`) and the rules
  are gone from the runtime, and the macro map, its edit patches and the far corner colours read the baked
  paint. Checked at the hill pose against the rule paint: mean pixel difference 1.25/255, same grass share,
  and per layer against the per-vertex rules over 16 cells (grass 0.871 vs 0.870). The rules (`PaintSample`,
  `PaintCell`, erosion paint), `SetSurfacePainter`, `VolumeTerrain::surfacePainter` and the bake were then
  removed (in git history). The trail carve (`CarveTrails`) moved to the trail objects (stage 3, below);
  `paintArchive` lives on the script, not the TerrainManager (as `objectArchive` does)
- [x] Trails as spline objects in the scene, with spline rendering and carving, replacing
  `TerrainManager::contourPaths` set in code (stage 3, 2026-11-25). A trail is an object with a `SplinePath`
  (children are control points) and the `TerrainTrail` script (Scripts/Landscape): at startup, and whenever its
  points or settings change, it samples the spline (1 m, Douglas-Peucker to 5 cm) into a polyline in the
  manager's space and publishes it (`TerrainManager::SetTrail`; settings: half-width 1.6, edge 4.2, strength,
  carve depth 0.12, paint layer, crossable layer mask, far colour). The manager's `TerrainTrailNetwork`
  (TerrainTrails.h) bins the segments (16 m bins); the terrain shades it per pixel on every level from an
  RGBA32F texture in slot 3 (`Object.hlsl TerrainTrailSample`, nearest segment, per-trail layer/crossable/far
  colour), grass and placement rules read `TrailShare`/`TrailDistance`, and every procedural cell is carved by
  the builder wrapper (`SetProceduralBuilder`) over crossable paint; the carved network's signature joins the
  cell cache key, and an edit reloads the cells it touches once it has settled (0.4 s). The noise contour paths
  (`ContourPaths`, `TerrainPathWeight`, LandscapeDriver `TrailDistance`/`CarveTrails`, `ENGINE_TERRAIN_PATHS`)
  are removed. `tools/landscape_trails.py` traced the old contour over the 4 m play area, cut it where the old
  height band hid it, and wrote 60 trail objects (82 km, 4,320 control points) under a "Trails" object placed
  before the terrain in landscape_demo.scene (`build_landscape_demo.py` carries it over). Open: trails beyond the
  play area (the old noise ran map-wide) are not traced. Follow-ups (2026-11-25): grass tiles and GPU blades
  key on the trail signature, so they rebuild when a trail changes; in edit mode (scripts get no Update) each
  trail registers a watcher the manager calls every editor frame (`SetTrailWatcher`, from
  `UpdateEditorStreaming`), so dragged points redraw the trail (tested by call, not yet by hand in the editor). Cost (DX12, 1080p, interleaved A/B with and without the
  Trails group, 300 frames): MeadowSlope 18.9 vs 19.0 ms and 18.7 ms (one 23.0 ms outlier run), Trail 19.6/19.5
  vs 20.0/19.8 ms: within run-to-run noise
- [x] Decide the same for ground cover (grass, ferns, bushes: currently runtime by paint layer); at minimum its
  settings become saved layer/GrassField data instead of script overrides. Decided and done 2026-11-25 (user):
  sparse, large plants are baked like painted placements; dense ground cover (more than ~3 GB as placements:
  grass ~21 GB map-wide, shrubs/ferns/flowers ~5 GB) stays generated, where a 3D foliage density says
  - [x] Bushes baked: `.local/tests/LandscapeBushBake.cpp` (target `LandscapeBushBake`) wrote a "Landscape
    bushes" layer into `world.objects` by the meadow bush rule (clusters on meadow and forest soil, off the
    trails): 8.33 M bushes, archive 144.6 to 208.8 MB. Edited with the placement brushes like the forests;
    drawn by the layer's GrassField from its saved placements. Heights from the archive (hand sculpts not
    followed at bake time)
  - [x] Foliage density (`TerrainFoliage.h`): per foliage layer (named like its ground-cover object layer), 3D
    voxels of 1 m in 8^3 chunks, stored as offsets from the layer's baseline (its GrassField's paint rule,
    facing up), only where painted: `<x>_<z>.foliage` per cell, written when the manager serializes and listed
    by signature so undo restores it. `TerrainManager::PaintFoliage` (Add, Erase, Set; a shell of 1.5 m
    around any surface)
  - [x] GrassField grows by it, repeatably: candidates per world 1 m column, layer seed and group (integer
    hashes, no std distributions), so the same plants grow for any tile size, on any compiler; thickets, drifts
    and fields keyed in manager space (they repeated every cell before). Where a cell's field has volume edits
    it scatters on every surface (vertical columns plus x and z columns, each surface point owned by the axis
    its normal faces most): faces and ceilings grow what is painted there. Blades read the density too. Tested
    in TerrainEditingToolsTests (`TestFoliage`: tile sizes 4/16 m give identical plants, erasing changes only
    the plants it reaches, save and undo, plants under a rock slab once its ceiling is painted, a Foliage brush)
  - [x] Settings out of the script: `TerrainObjectLayer::Settings` carry snap/conform, the detail layer and the
    layer object's components ({type, value}: each ground-cover layer's GrassField), saved in the scene's
    `objectLayerSettings`; LandscapeDriver's `SetObjectLayerSetup`, `grassRadius`, `forestLayerName` and its
    ground-cover `ENGINE_*` switches are gone (the scene's values are what the defaults gave, checked by
    dumping both). "Procedural meadow bushes" is now "Procedural meadow shrubs" (bushes are baked); the far
    ring reads the meadow's density (`GrassField::foliageLayer`)
  - [x] Foliage brushes: a `.brush` Foliage section (layers with amounts, mode, shell; tool "foliage"),
    edited in the brush inspector; choosing one puts the Paint tool on foliage, painting whatever surface the
    cursor meets, with the brush's rules per voxel. Brushes: Meadow, Forest floor, Cliff ferns (slopes
    40-90 degrees), Clear foliage (TerrainData/Brushes)
  - Open: painting in the editor not yet tried by hand; frame times after the change 0.7-1 ms above earlier
    runs the same day (FieldWalk 18.5, ForestEdge 21.9, ForestFloor 21.0, MeadowSlope 19.5 ms), within this
    machine's run-to-run noise but not ruled out; tall-grass fields still come from the field noise (paint
    could take them over as their own foliage layer)

Not achievable in the editor or by a script component (offline tools, code constants, env vars):
- [ ] The map's base heights only come from `tools/build_landscape_demo.py` (macro ranges, whole-map droplet
  erosion, hills, escarpment, lake basins). Hand sculpting works, but only as edits on top of the archive
  (`SaveEdit` needs the builder's baseline), and re-running the tool deletes them. The editor can't create or
  regenerate a map-scale base (see 2, "terrain generation window")
- [x] `world.flow` / `world.deposition`: baked into the archive and deleted, then the archive's erosion section
  removed too (2026-11-25, see Direction); the generator no longer writes one. Note: re-running the generator
  rebuilds heights only; `world.paint` and `world.objects` stay as they are (no tool rebuilds them now)
- [ ] `world.fargrid` written only with the archive (see Direction: rebuild from the volume)
- [ ] The scene file is generated (`write_scene`): 189 `proceduralRuns` rows, mist/cloud heights. Re-running the
  tool deletes `.terrainedit` sculpts of cells whose heights changed. Check whether the editor can add procedural
  cell runs
- [x] Trail network (`TerrainManager::contourPaths`: periods, seeds, widths, layer) is set in LandscapeDriver code
  and not serialized; expose it as scene data. Done 2026-11-25: trail objects (SplinePath + TerrainTrail), see
  Direction stage 3
- [x] ~20 `ENGINE_*` env switches in LandscapeDriver change the default look (e.g. `ENGINE_BROADLEAF=lone`,
  `ENGINE_LANDSCAPE_TALL_FIELDS`, `ENGINE_SCAN_BUSHES`, `ENGINE_TREE_SHADOW_DISTANCE`, `ENGINE_LANDSCAPE_GRASS_*`);
  defaults that matter should become driver properties. 2026-11-25: the look is scene data now (layer settings);
  only `ENGINE_FAR_PATCHES` and `ENGINE_TERRAIN_LOADED_RADIUS` (A/B switches) remain
- [ ] The crown footprint table is pasted into LandscapeDriver.cpp from `measure_tree_crowns.py` (the canopy
  splat reads it); derive from the assets at load instead. (The reach table went with the generators.)
- [ ] `add_landscape_viewpoints.py` replicates ForestMask/ForestBand to place viewpoints; the driver's
  ForestMask is gone (2026-11-25), so it now stands alone: place viewpoints from the saved forest instead.
  (`landscape_cliffs.py`, dead, removed 2026-11-25)

Requires the custom script `LandscapeDriver` (scene-specific):
- [ ] Cell heights: reads the archive through `SetProceduralBuilder`; without the script the procedural cells are
  empty
- [ ] Six terrain layer materials (the rule-based paint is baked and its rules removed, 2026-11-25)
- [x] Trails carved into the cell heights (2026-11-25: the TerrainManager carves trail objects into every
  procedural cell; see Direction stage 3)
- [x] Forests (`GenerateForest`), baked and the generator removed (see Direction, 2026-11-25). Was: species mix, conifer share by altitude, slope limits, edge shrub belt/skirt,
  lone meadow broadleaves, limbs clear of trails, shadow ranges, "Trees" detail layer. The Objects brush can't do
  rule-based or map-wide (far block) placement. To be replaced by saved placements (see Direction)
- [x] Props (`GenerateProps`): rocks, boulders, logs, stumps, branches, ferns, bushes. Baked with the forests
- [x] Ground cover (`SetObjectLayerSetup`): `MeadowGrass::Apply` (script-side) plus ~30 GrassField overrides
  (tall fields, far-ring radii, tile size, shrub reach, GPU blades, per-plant detail, paint layer masks).
  2026-11-25: saved layer settings with their GrassField; where it grows is the foliage density
- [ ] Macro color map (`SetMacroColorSource`) incl. the canopy splatted from forest placements. Follows
  sculpting and paint through patches (see Direction), and the canopy reads saved placements; still set up by
  the script
- [ ] Far terrain setup (`ConfigureFarTerrain`: far grid, palette, corner colors)
- [ ] Lake water: a runtime-only 24 km plane built in code. Could be an ordinary editor object (plane Mesh +
  `LakeWater.material`); move it out of the script
- [ ] Editor preview (`OnAfterDeserialize`: loads cells around the explorer, far silhouette, moves the editor camera)
- [ ] Terrain cache key hashed in code
- Reusable script (not scene-specific): `FirstPersonController` on Landscape Explorer

Minor:
- [ ] `Landscape` object carries an empty `Mesh` (`file=""`) and a `Material` that look left over
- [ ] `MeadowWalkCamera` and `Landscape Explorer` both have `Camera.active="true"`
- [ ] Viewer object name set twice (TerrainManager and LandscapeDriver)
