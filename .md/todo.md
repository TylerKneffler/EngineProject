# Engine TODO

One plan for the landscape demo: look, foliage, grass and performance. `grass.md` and `foliage.md` were merged into
this file on 2026-11-19; their phases are the "Foliage" and "Grass" sections below, and code comments that say
`.md/todo.md Foliage Phase N` point there. Finished work older than this file is in [todo_history.md](todo_history.md).

**Goal:** the landscape demo meets the expectations below (the look of the Chlorophyll Engine frames in
`.local/reference-images/chlorophyll-video/`) and holds 60+ fps at 1920x1080 on the RTX 4090 **while walking and
turning**. 100+ fps on typical poses stays the aim. The earlier reference sets in `.local/reference-images/` (eight
images, and six in `first-set/`) are variants to reach afterwards: water with reeds, fog, forest interiors.

## Landscape demo expectations (2026-11-19, from the Chlorophyll Engine video)

What `landscape_demo.scene` is expected to look like and how it is checked. Reference: four frames of
"[Chlorophyll Engine] 10M Trees - 15km² Test" (youtube.com/watch?v=VC_NUxbeA0E, 7:18) in
`.local/reference-images/chlorophyll-video/` (PNG copies in `png/`): 01 forest edge above a meadow slope, 02 meadow
with flowers and hazy ridges, 03 slope with single trees over a forested valley, 04 meadow, lone tree and a rock
escarpment under a forested plateau. All four are a clear summer midday.

Numbers are means over a region of the 8-bit image: sRGB, and luminance after sRGB decoding (0-1). Measured with
`.local/foliage/measure_look.py`; demo values from `.local/foliage/exp_ForestEdge.png`, `exp_Vista.png` and
`mv2_walkturn.png`. Video compression and the uploader's grading are in the reference numbers, so they are targets
to land near, not to match to the digit.

| What | Reference frames | Demo, 2026-11-20 after look pass 1 | Expected | Passes |
|---|---|---|---|---|
| **Sunlit meadow brightness** | luminance 0.07-0.12 (sRGB about 57-79 / 75-98 / 23-35) | 0.077 at MeadowSlope (66 / 82 / 19); 0.22 at ForestEdge, where auto exposure brightens for the dark forest | 0.07-0.12 in sun; about a third of that under a cloud shadow (frame 02: 0.03) | MeadowSlope yes; ForestEdge no |
| **Meadow colour** | yellow-green: red is 0.76-0.81 of green, saturation 0.68-0.75 | red 0.81 of green, saturation 0.76 | red at 0.7-0.8 of green in sunlit grass | yes |
| **Grass cover** | unbroken to the forest edge, 100 m and more; no ground visible; fine blades; sparse small white and yellow flowers | ground between blades now reads as shaded undergrowth, and the open ground past the grass is within 15 % of the meadow's luminance (0.087 against 0.077); far ring now reaches 200 m (was ~50 m in practice); still: detailed grass ends at ~20 m, clumps read as separate plants at 20-60 m, broad blades, generated bushes that read as cartoon leaves | terrain share (render mode 6) at most 5 % from the camera to the forest edge; no visible band where the detailed grass ends; blades thin enough that tussocks do not read as separate plants at 3 m | no (band gone; reach and blades open) |
| **Forest edge** | a closed wall of crowns from the ground up: shrubs and saplings fill below the crowns, no trunk line, no soil, almost no sky through the canopy; heights vary; single trees stand out in the meadow | open woodland: trunks, bare red-brown soil under the trees and sky through the crowns | from 50-150 m, no bare soil or trunk line under the edge and sky through the forest silhouette under 5 % of its area; a shrub belt at the edge; single trees in the meadow | no |
| **Crown tone** | near crowns are darker than the meadow: 0.03-0.05 against 0.07-0.12 (about half), with lit tops | shaded crown undersides 0.024 against a 0.077 meadow (a third); were 0.015, nearly black | sunlit meadow about 2x the mean luminance of the adjoining crowns | close |
| **Far forest** | hills covered to the top in canopy with visible crown texture and pale field patches; luminance 0.08, no brighter than the sunlit meadow, shifting blue with distance (62 / 79 / 83) | front-lit far forest 0.082 against a 0.077 meadow (was 0.105, 1.5x the meadow); still flat: 10th to 90th percentile 0.066-0.098 where the reference spans 0.033-0.129; no blue shift at 300-600 m; against the light (Vista) it drops to 0.02 | far forest no brighter than the sunlit meadow; crown texture readable to ~2 km; hue moves toward blue with distance | brightness yes; texture and haze no |
| **Sky** | saturated blue (79 / 124 / 177, saturation 0.56) rising to a bright hazy horizon (luminance 0.6); white cumulus with soft shaded bases | 94 / 148 / 197, saturation 0.50-0.53; horizon luminance 0.66 on the test meadow | clear-sky saturation at least 0.5 away from the sun; horizon luminance at least 0.5 | yes |
| **Terrain in view** | rounded hills a few hundred metres high, forested to the top, with meadow clearings on the slopes; one sheer rock escarpment under a forested plateau (frame 04), vegetation running down its gullies | flat valley floor under bare pale alpine peaks; rock faces only as outcrops of scanned pieces | around the start: rolling forested hills with meadow slopes; the alpine range as a distant backdrop; one escarpment at least 60 m high and 300 m long with forest above and below | no |
| **Tree variety** | several silhouettes in one view: tall narrow crowns, broad round ones, a dark flat-topped lone tree, low shrubs | eight oak models and four conifers | at least three clearly different broadleaf silhouettes in any edge view, plus the shrub belt | no |
| **Frame rate** | reported by the developer, not verified: about 60 fps in the 10M-tree video's HUD; the earlier showcase 130 fps average, ~80 in dense forest, ~200 from the air, at 1440p on an RTX 3080, without upscaling, frame generation or hardware ray tracing | ~42 fps standing, ~35 fps walking (28.5 ms median, p95 42, 11 frames over 50 ms of 400) at 1080p on the RTX 4090 | unchanged: 60+ fps while walking and turning, no frame over 33 ms in a 400-frame walk, 100+ on typical poses; native resolution, no upscaling | no |

**Look pass 1 (2026-11-20).** Captures `.local/foliage/i1_MeadowSlope.png`, `i1_ForestEdge.png`, `i1_Vista.png`,
`i1_ForestFloor.png`; the test meadow matches on DX11, DX12 and Vulkan (`x_dx11.png`, `x_vulkan.png`, `x_dx12.png`).
- [x] **The landscape had no sunlight in captures.** The sun's cloud-shadow cookie path ("Assets/Textures/...") went to
      the texture loader unresolved, so it loaded only when the working directory was the asset root. From the
      capture tool (run from the repository root) the decode failed, the null (black) texture was bound, and the
      cookie multiplied the sun by zero: every landscape capture since cloud shadows were added (Foliage Phase 3)
      was lit by ambient light only, with auto exposure brightening the result. That is the "dark meadow, bright
      far forest" this section first recorded as the demo's state (meadow 0.011-0.035), and the Foliage Phase 1d
      colour pass on the landscape was judged under it. Whether the editor or the game showed the same depends on
      their working directory; not checked. Fix (`SceneRendererLighting::ResolveViewLighting`): cookie and IES
      paths go through `Mesh::ResolveFilePath`, and a missing or still-loading texture is replaced by a white
      texel, so it can never black out its light. Measured at fixed exposure: a uniform 0.3 cookie now gives
      exactly the image of a 30 % sun.
- [x] **Ground tint was off.** The forest-floor grass field (no tint) cleared the meadow field's ground cover on
      every refresh, so the bare terrain texture showed pale between the blades and "raising the tint did
      nothing" (Foliage Phase 1d open item, now closed). `Terrain::GroundCover` records its owner and a time
      stamp; only the owner changes it and an unrenewed claim expires after 2 s. Meadow preset: tint strength
      0.5 → 0.8, root shade 0.7, cover colour factor 0.8 → 0.45 (the open ground past the blades was 1.6x the
      meadow's luminance; now 1.13x).
- [x] Scene settings (scene and `tools/build_landscape_demo.py`): tone mapping AgX → ACES (AgX could not reach the
      reference sky: saturation 0.44 against 0.56), output exposure -0.4 → -0.25, grading saturation 1.1, displayed
      sky intensity 1 → 2.4 with the sky fill light (`hdriIntensity`) 0.55 → 1, haze `mieScale` 4 → 3, exposure
      floor `minEv` -4.8 → -4. Sun 40 → 57 degrees up, colour 1 / 0.91 / 0.77 → 1 / 0.96 / 0.9.
- [x] Cloud shadows 0.3 → 0.42 under cloud (`generate_sky_sprites.py`, texture regenerated): at 0.3 the shaded
      meadow was 4.5x darker than the sunlit one; the frames show 2.4-4x.
- [x] Far trees: oak impostor materials had twice the near leaves' flat ambient and the same tint, and read 1.5x the
      meadow front-lit. Ambient halved, tint x0.8, and `foliageTranslucency` 0.7 on all 28 impostor and cluster
      materials so they do not go black against the light.
- [x] Near crowns: shaded leaves under a sunlit canopy get scattered light (`FoliageTransmission`, Object.hlsl:
      0.3 x (1 - shadow), more toward the shell). Crown undersides 0.015 → 0.024. It follows the leaf's shadow,
      so a tree in terrain shade keeps a little of it.
- [x] Viewpoint **MeadowSlope** added (`tools/add_landscape_viewpoints.py`: a meadow slope with forest 50-150 m
      ahead, lower ground beyond, no lake in view, sun behind or beside the camera); eye (-240, 12, 1416).
      The tool's sun direction now follows the scene's sun pitch.
- [x] **Grass reach** (2026-11-20, user: "grass vision doesn't extend very far"). Cause: grass radius is multiplied
      by the detail budget's scale, which sits at its 0.6 floor on the landscape (0.53 for the Grass layer), so the
      detailed grass ended at ~20 m instead of 36 and the far ring at ~50 m instead of 90. Now the far ring keeps
      its authored reach (`GrassField::OuterRadiusScale`; its inner edge follows the detailed field's scaled
      radius, which also closes the gap between the two), reaches 200 m on 64 m tiles at 0.9 clumps per m²
      (`MeadowGrass::ApplyFar`), and its clump is wider (`far_grass_tuft`: 14 blades 7.5 cm wide, ~0.9 m across,
      42 triangles). MeadowSlope: the ground from ~60 m to the forest edge measured 0.074-0.091 against a 0.078
      meadow, and reads as grass (`.local/foliage/j3_MeadowSlope.png`, `j3_crop.png`). Cost at MeadowSlope:
      grass GPU 2.0 → 3.8 ms, 16.7k → 127k grass instances, tile count unchanged (268), camera record
      11.1 → 12.3 ms (single runs).
- [x] **Grass shells in the terrain** (user: "should the grass materials for terrain have some sort of 3D effect"):
      `GrassGroundShells` (Object.hlsl) shades the ground under and past the blades as tufts ~0.3 m deep, four
      parallax shells through a procedural tuft height field: tufts hide the ground behind them at low angles,
      show dark gaps from above, and near the horizon the result goes to the tip colour. Replaces the grain and
      sheen of Grass Phase 6a. Terrain GPU unchanged (0.86 ms). Runs on DX11, DX12, Vulkan (test meadow).
      Limit: at eye level over flat ground a pixel row covers metres of depth, so the shells only show their
      depth from a raised view or on a slope facing the camera; standing blades carry the look otherwise.
- [ ] Grass follow-ups: the far ring still reads as rows of separate clumps at 20-60 m (dark ground between them);
      the shells have not been judged from a raised view; the +1.8 ms of grass GPU is not budgeted; the detailed
      grass still ends at ~20 m because the detail budget is at its floor; coverage numbers (render mode 6) not
      re-measured.
- [ ] **Exposure at forest edges**: with a dark forest filling the frame, auto exposure brightens the meadow to
      0.22 (ForestEdge), about a stop over the target. A higher floor (-3.5) brings it to 0.16 but turns the forest
      floor nearly black (whole frame 0.016); metering on the brighter 30 % makes MeadowSlope too dark (0.044).
      Needs metering that weights sunlit ground, or partial adaptation.
- [ ] **Far forest texture and haze**: front-lit impostors are flat (no shadow between crowns) and there is no
      blue shift at 300-600 m. Needs occlusion baked into the impostor atlases (Foliage Phase 5) and a look at
      aerial perspective strength at short range.
- [ ] Forest interior is dark after this pass (ForestFloor whole frame 0.027, floor 0.012): check it against the
      first reference set before changing it.
- [ ] Not re-checked after this pass: the look in the editor, grass_test coverage numbers, the moving-camera runs
      other than one walk.

**Terrain and forest edge pass (2026-11-20, second machine: RTX A2000 laptop).** Built (`devoptimized`,
GameBackendCapture) and the map regenerated; **not yet seen in the engine**: no landscape capture was run on the
laptop, so every look statement below is from the generator's top-down and elevation previews and from counts on
the written archive, not from a rendered frame. The expectations table above is unchanged until it is captured.
Smoke test of the new build only: lighting_showcase on DX11 at 640x360 renders correctly.
- [x] **Rolling hills in the lowland** (`build_landscape_demo.py` `lowland_hills`, added in `carve` after erosion):
      swells up to ~150 m on a 1.5 km wave with flat valley floors between, none within 260 m of the start (the
      start meadow and its view of the first lake stay) or on the lakes, fading out 1.9-2.9 km from the start.
      Within 2 km of the start the ground was -38..21 m (5th-95th percentile) and is now -38..116 m; 40 % of it
      slopes 10-25 degrees, 2.8 % is steeper than 30 (no trees there).
- [x] **Escarpment** (`ESCARPMENT`, `escarpment`): a cuesta 700 m south of the start, facing it (centre
      (-150, -660), face toward +z, so it is sunlit and visible from the start meadow). Rock face 66-95 m high,
      steeper than 50 degrees along 730 m, buttresses to 70-80 degrees with gentler gullies, a 12 m talus apron,
      the plateau dipping away over 650 m behind. 15,600 m2 of it passes `GenerateCliffs`' steep-rock test along
      the whole face, so the scanned rock pieces should stand on it; 93 % of the plateau and 96 % of the foot are
      plantable (under 30 degrees).
- [x] The rest of the map is bit-identical: the builder reproduced the stored archive before the change, and hills
      and escarpment are exactly zero outside their footprints. The builder now keeps the sculpt edits of cells
      that come out identical (`unchanged_cells`; all 20 `.terrainedit` files survived) instead of deleting all of
      them. The archive signature changed, so terrain caches rebuild on first load. `world.flow` and
      `world.deposition` are from the erosion and do not know the new hills.
- [x] **Forest prefers higher ground** (`ForestMask(x, z, height, a)`, LandscapeDriver.cpp; replicated in
      `add_landscape_viewpoints.py`): the mask noise is raised by 0.06 from 30 to 80 m above the water level, by
      my estimate from the noise's spread about 44 % forest on valley floors and 60 % above. This applies to the
      whole map, so the foothills carry roughly a third more trees, near and far. Cost not measured.
- [x] **Closed forest edge** (`GenerateForest`, second pass with its own random stream; the first pass's trees
      keep their places where the mask did not change): medium and small oaks and saplings (young spruce and fir
      in the conifer band) across the mask's transition, up to one per ~30 m2, smaller toward the meadow, leaning
      out up to 9 degrees. Loaded cells only, not in far blocks. No new assets.
- [x] **Shrub belt** (`GenerateProps`, undergrowth, own random stream): the stand's tall bush (30 % the low one)
      at 1-1.9x scale along the meadow side of every edge, up to one per ~12 m2, ragged by the 16 m clump noise.
      Drawn to the props layer's 220 m.
- [x] **No soil line under the edge** (`PaintSample`): forest soil is painted only from mask 0.3 up (full at
      0.85), so the outer edge trees and the belt stand in meadow grass. `PaintRevision` bumped.
- [x] Viewpoint **Escarpment** (`find_escarpment`): eye (-350, -10.7, -370) on open ground 350 m in front of the
      face, looking at its middle. **MeadowSlope** moved to (-368, 120.3, -1000), a 20 degree slope on the hill
      behind the escarpment with a 149 m drop to the valley (it was (-240, 12, 1416)); the numbers in the
      expectations table are from the old pose.
- [ ] **Capture and judge all of the above**: Escarpment, MeadowSlope, ForestEdge and the start pose; forest
      edge rows of the table (sky through the silhouette, soil, trunk line); whether the belt bushes read as a
      shrub layer or as a hedge; whether the scans cover the face or leave bare painted rock; the escarpment's
      ends and rim. Then the moving-camera runs: more trees, edge trees and belt bushes all add draws.
- [ ] Not done here: denser trees inside the forest, more broadleaf silhouettes, scanned shrubs, hills in the
      flow map (lush valley floors in the macro colours ignore them), forest placed by slope or curvature
      (height only so far).
- [ ] The lower, warmer sun (40 degrees, 1 / 0.91 / 0.77) as a second lighting preset; the scene object is still
      named "Late afternoon sun".
- [ ] Forest density: `treesPerCell` is 220 attempts per 128 m cell, at most ~13,400 trees per km² (one per 75 m²,
      before the masks reject any). The video's title gives 10M on 15 km², ~670,000 per km², which must count
      shrubs and saplings (my inference). The expectation is canopy closure as in the table, not a count: raise
      tree density at the edge, and add the shrub and sapling layer that hides trunks and soil.
- [ ] The earlier target for the main shot (elevated camera, backlit sun, dark foreground silhouette) becomes the
      Vista viewpoint only. The RDR2/GTA VI/Everglades references (fog between trunks, water with reeds and lily
      pads) are later variants, not the first look to hit.

**What is known about the reference engine** (press and aggregator reports; none of it checked against the engine):
- Hand-written engine by one developer, seven or eight months in (reports differ), built to do one thing: draw very
  many high-polygon vegetation instances. Not public. It is not a game: no gameplay, physics or AI load.
- Geometry: clustered LODs ("Nanite-like virtual geometry"), voxel-based representation of distant vegetation
  instead of impostors, and proxies without per-instance data for the farthest objects (Wccftech, PC Games
  Hardware, 4 and 7 November 2026).
- Lighting: screen-space techniques plus voxel-based global illumination, no hardware ray tracing. Shadows, AO,
  leaf translucency and antialiasing are not described anywhere.
- Scale claims disagree: "about 1 million instances and 4 million triangles" in one scene, against "3 million
  trees, ~4 million polys each" on the showcase's title card. The 10M-tree video is a later, larger test.
- Streaming (aggregator summary only, could not be opened to confirm): a voxel hierarchy for tile traversal and
  proxy tiles that stream instance data on the GPU; the showcase includes clouds, rivers and hiking paths.
- The developer's earlier Godot forest demo (80.lv, 5 August 2025): terrain height, colour and grass-density maps
  and tree/rock points generated in Houdini from curvature and slope; grass as GPU particles; volumetric clouds;
  60 fps capped, ~170 uncapped on an RTX 3080.
- Unfinished by the developer's account: weather.

**What this changes for this engine, and what it does not:**
- Not adopted: virtual geometry, voxel far vegetation and voxel global illumination. Each is months of work, and
  the measured gaps above are exposure, colour, density and content, which the current renderer can close.
- Same direction as "Next" below: per-frame CPU cost that does not grow with visible tiles (their tile traversal
  and instance streaming run on the GPU; here every tile and asset costs scene-thread time).
- Worth copying cheaply: placing forest, clearings and rock from terrain curvature and slope (the paint rules and
  `ForestMask` are the place), and far vegetation lit like the near trees (impostor re-bake, Foliage Phases 1 and 5).
- Sources: [Wccftech](https://wccftech.com/custom-forest-engine-delivers-stunning-visuals-at-130-fps-on-an-rtx-3080-at-1440p-without-hardware-ray-tracing/),
  [PC Games Hardware](https://www.pcgameshardware.de/Spiele-Entwicklung-Thema-35261/News/Custom-Engine-Wald-RTX-3080-130-Fps-1559175/),
  [kantan.news](https://kantan.news/news/custom-forest-engine-delivers-130-fps-without-hardware-ray-tracing-on-rtx-3080),
  [80.lv](https://80.lv/articles/godot-developer-showcased-impressive-forest-scene-demo),
  [Dropday.ai](https://dropday.ai/c/chlorophyll-engine) (search summary only),
  [the video](https://www.youtube.com/watch?v=VC_NUxbeA0E) (title and four frames only).

## Static objects (2026-11-20)

`Object::isStatic` ("Static" checkbox under Enabled/Layer in Properties; scene attribute `static="true"`, written
only when set; children are static through their parent). Code: `Engine/Core/Scene/StaticObjects.{h,cpp}`, called
from `Game::Start` and `Game::Update`.
- [x] **Bake at play start**: static prefab instances that are plain geometry (Mesh, Material,
      PrimitiveObjectCollider, Static RigidBody; no blended material; no prefab overrides; rotation + uniform
      scale; two or more of the prefab; not named by another component's field) are removed from the scene and
      become placements on one runtime-only `TerrainObjectLayer` ("Static objects (baked)"), the path forests use.
      Editor edit mode is untouched; play mode restores the scene on stop. `ENGINE_STATIC_BAKE=0` keeps the objects.
- [x] **Warnings** (stderr and debugger in the game, Console in the editor, and under the checkbox): non-static
      parent, Dynamic/Kinematic rigid body, a component that moves its owner (controller, spline follower,
      animation, IK, cloth; by type name), non-uniform, mirrored or sheared scale, prefab overrides, referenced by
      name; and while playing, a static object whose transform changes (`MotionWatch`, 32 objects checked a frame).
      Static lights, cameras, terrain and emitters stay scene objects without a warning.
- [x] Terrain scenes: every top-level object except the explorer is static in landscape_demo, grass_test,
      foliage_test and woodland_assets_preview (`tools/scene_static.py`; the three scene builders call it).
- [x] Checked on woodland_assets_preview (DX11, 960x540, laptop): 16 conifers of 4 prefabs baked, no warnings;
      a copy with a 1 2 1 scale and a Dynamic body on the ground printed both warnings and baked the other 15.
- [x] **Baked trees follow the prefab's LOD distances** (checked 2026-11-20, DX12, young spruce alone, authored
      23.75 / 85.5 m, 640x360 where the view scale is ~0.77): submitted triangles 50.8k at 18 m (LOD0), 13.2k at
      30 and 38 m (LOD1), 316 at 66 and 96 m (impostor). The layer receives the distances from the instance's
      mesh (printed: fir 37.5/135, pine 50/180, tall spruce 60/216, young spruce 23.75/85.5) and, like the
      forests, keeps LOD0, LOD1 and the last level (the impostor) of a longer chain.
- [ ] **The same trees as scene objects (unbaked) look different, and that path is the odd one.** Scene objects
      keep LOD0-2 and never the impostor, so the far row (420-470 m) is whorl stars and nearly invisible; and
      the young spruce turns sparse between 34 and 38 m, where the authored distances say LOD1 until 85 m (65 m
      at that view scale). Not caused by the static work (`ENGINE_STATIC_BAKE=0` shows it); cause not found.
      It means ticking Static changes how a tree looks at mid range: fuller at 38-65 m, an impostor beyond.
- [x] **foliage_test with the grove as static objects** (2026-11-20, laptop, DX12 1280x720, `ENGINE_DETAIL_SCALE=1`,
      `ENGINE_AUTO_EXPOSURE=0`; `build_foliage_test_scene.py --standalone` copied to
      `.local/static-test/foliage_standalone.scene`, images in `.local/static-test/foliage/`): 77 of the 84 grove
      objects of 19 prefabs baked, no warnings; the other 7 are single instances of their prefab. Baked and
      unbaked (`ENGINE_STATIC_BAKE=0`) look alike at ForestFloor, Backlit and Meadow (3-5 % of pixels differ by
      more than 0.16, the LOD differences above). Frame interval, 5 frames each, rough: scene's own grove layer
      18-22 ms, baked 28-57 ms, unbaked 85-115 ms. The baked layer has no fixed shadow level and a 1000 m
      shadow distance, which is most of its gap to the scene's layer (GPU shadow 2.2-3.1 ms against 0.8-1.0 ms).
- [ ] **DX12: trees past the first detail distance draw as fragments when frames run back to back** (found with
      the checked-in foliage_test, Meadow view, laptop RTX A2000, 1280x720). Trunk bases and scattered leaf
      pieces with sky behind them, as if the foliage depth prepass had written depth there and the depth-equal
      shading pass had not covered it (inferred, not shown). DX11 draws the grove. Repeated 600-frame runs, counted against a DX11
      reference image: uncapped 13 of 22 broken (8 of the 9 good runs came in one seven-minute stretch); `--max-fps 30` 0 of 4; `ENGINE_FOLIAGE_DEPTH_PREPASS=0`
      0 of 4; the layer's `shadowDistance` 0, 0 of 3; `ENGINE_GPU_DEBUG=1` 0 of 2 (and every earlier run with
      it on). Still broken with `ENGINE_TREE_WIND=0` (2 of 2), `ENGINE_TAA=0` (3 of 3),
      `ENGINE_GPU_OCCLUSION=0` (3 of 3), `ENGINE_SHADER_CACHE=0` (1 of 1), no `shadowLod` on the layer (1 of 1,
      2500 frames), and with the static-object calls in `Game.cpp` commented out (3 of 3, committed scene).
      A layer held at LOD0 was fine (1 run). So it needs the prepass, a shadow-casting layer and no idle time
      between frames; which data the two passes read differently is not found. Checked and not it: two
      groups sharing one persistent batch key in a frame (none), a compute pipeline left bound at an
      indirect draw (none), different pipelines between working and broken runs (same set), draw records
      starting unzeroed (`objectData{}`). Not tried: a build of HEAD without this session's other changes.
      An earlier version of this entry blamed `shadowLod` and wind; those tests all ran with the debug
      layer on, which hides the fault. The debug layer reports error #613 (render-target format mismatch,
      pipeline with no colour target) on the prepass draws when the layer has `shadowLod`; not explained.
      The landscape forest uses the same prepass and casts shadows, so it is probably affected on DX12 when
      GPU-bound; not checked (no landscape load on the laptop).
      It switches while running: in 120 consecutive uncapped frames (`ENGINE_GPU_WORKLOAD_COUNTERS=1`) the
      scene was broken, turned correct for ~15 frames straight after a 25-39 ms frame, then broke again.
      Same triangles submitted in both states (leaves 142k, prepass 146k). Pixel-shader invocations, good
      against broken: depth prepass 3.0M / 2.0M, bark 38k / 18k, leaf shading 161k / 250k. So in the
      broken state a third of the prepass fragments and half the bark are rejected before shading, as if
      nearer depth were already there when the prepass starts. With GPU occlusion on, the broken state also
      halves `foliage_gpu_selected_lod0` (5310 -> 2830): a consequence, since it stays broken with
      occlusion off (240 of 240 frames). Harness: `.local/static-test/` images, and
      the repeat/compare scripts in `.local/static-test/harness/`.
- [ ] The scene's grove layer still reports `prefab_layers_pending` 1 after `--warmup 200
      --wait-for-residency 120` on the laptop and 0 after `--warmup 2500` (not measured in between); the
      foliage_test captures above used `--warmup 2500`. A later 600-frame run reported 0.
- [ ] Not run: the editor checkbox and Console output (the editor builds), landscape_demo with the
      flag (no landscape load on the laptop), a static object that actually moves (a Dynamic body with
      gravity on the collider-less ground did not move it, so the watch has never fired), prefabs with colliders
      through the bake, save/load round trip of `static` from the editor.
- [ ] Not done: baking static objects that are not prefab instances; a single instance of a prefab; per-cell
      layers (one layer holds all baked placements, in 256 m groups); skipping per-frame work for static objects
      that stay scene objects; an override list so baked placements can be removed at runtime by id.

## Status (2026-11-19)

Landscape demo, DX12, 1920x1080, RTX 4090, RelWithDebInfo, uncapped; moving runs are 400 frames.

| Run | Median | p95 | Frames over 50 ms |
|---|---|---|---|
| Standing, forest floor (three runs) | 24.0-25.0 ms | 27-29 ms | 0 |
| Standing, forest edge / trail / vista (before the last two CPU changes) | 26.9 / 28.5 / 25.7 ms | 31 / 33 / 29 ms | 0 |
| Turning in place (`--look-yaw 6`, forest edge) | 24.7 ms | 38 ms | 3 |
| Walking (`--drive-forward 380`, forest floor) | 31.3 ms (28.5 on 2026-11-20) | 51 ms, p99 115 (42, p99 103) | 20 (11) |
| Walking and turning (`--drive-forward 380 --look-yaw 3`) | 35.2 ms | 59 ms (p99 115) | 34 |

- Standing still the frame is CPU-bound: update ~1.8 + prepare ~4.7 + draw recording ~16 ms (camera view ~11.5, shadow
  view ~3.9) run one after another on the scene thread while the render thread is idle ~89 % of the frame. GPU is
  9-11 ms. Trimming hot spots gave ~2 ms a round; 16.7 ms needs the recording split across threads.
- Moving adds hitches: walking spikes to 80-120 ms each time a terrain cell boundary is crossed, and raises the steady
  cost (update 1.8 → 4.6 ms, prepare 4.7 → 8.9 ms).
- Checked on DX12 only since the colour pass: trails, scans, TAA dither, the 3x3 AO lookup, CPU round 3 and the
  moving-camera fix. Look pass 1 was checked on the test meadow on DX11, DX12 and Vulkan.
- **Landscape captures taken between the cloud-shadow work (Foliage Phase 3) and 2026-11-20 had no direct
  sunlight** (cookie texture not found from the capture tool's working directory). Frame times are unaffected;
  every look judgement made from those landscape captures is suspect, including the Phase 1d colour pass.
- Committed through `28210b49` (2026-11-19 22:34). Uncommitted: CPU round 3, the moving-camera fix, the 3x3 AO lookup
  in Object.hlsl, the merge of the plan files and look pass 1.

## Next, in order

1. Done 2026-11-20: viewpoint and the settings half of the look (look pass 1 under "Landscape demo expectations").
   Left over from it: exposure at forest edges, far-forest texture and haze.
2. **Moving-camera hitches** at cell crossings: profile a walking run (`--drive-forward 380 --cpu-sample`) and spread
   the streaming work over frames (Foliage Phase 2, "Moving camera, remaining hitches").
3. **CPU pipeline to 16.7 ms**: shadow view recorded on a worker, a dirty list for frame items, one retain per
   distinct mesh buffer, shadow scene preparation skipped when nothing changed (Foliage Phase 2, "Next CPU steps").
   Measure with the walking and turning runs, not only standing poses.
4. **Forest and meadow content**: the shrub and sapling belt and edge trees are in but uncaptured (2026-11-20,
   "Terrain and forest edge pass"; done ahead of 2 and 3 on a machine where timings mean nothing, so their cost
   is unmeasured). Left: more broadleaf silhouettes (scanned shrubs, Foliage Phase 7), grass reach and the fade
   band (Grass Phases 1, 3, 6).
5. **Terrain**: rolling hills around the start, the escarpment and its viewpoint are in the archive but
   uncaptured (same pass). Left: judge them in the engine, the cliff follow-ups of Foliage Phase 7.
6. **Variants from the earlier reference sets**: still water with reeds and lily pads, haze between trunks and
   layered ridge haze, streams (Landscape look 7; Foliage Phases 6 and 7).

## Did not work / reverted

- Glossier oak leaves (roughness 0.6): white sun glints on crowns; reverted.
- Raising the meadow ground tint (0.5 → 0.8): no effect on the pale ground; reverted.
- Shifting the dissolve dither per frame: left diagonal hatching; replaced by rotating thresholds.
- Generated cliff blocks and crags: an outcrop, not a cliff (see Foliage Phase 7).
- Single-run CPU timings: machine-wide swings of ±30 % hid every CPU change; only profiler shares or
  interleaved A/B pairs are usable.
- Texture pointers out of the instance batch key: no measurable change while turning; reverted (Landscape look 0).
- Sky saturation through the atmosphere settings (`mieScale` 4 → 1.2, `rayleighScale` 1.5) under AgX: 0.32-0.45,
  never the reference's 0.56; the tone curve was the limit (ACES reaches it).
- Metering auto exposure on the brighter 30 % (`lowPercent` 0.7): the open meadow went too dark; reverted.
- Rebinding the light cookies before the camera's draws: no change (the texture had never loaded); removed.

## Not done (index; details in the sections below)

- **Look:** brightness, colour and sky rows pass at the old MeadowSlope pose; grass reach, far-forest texture
  and tree variety do not; forest edge and terrain were reworked on 2026-11-20 and are not captured yet
  (expectations table, "Terrain and forest edge pass").
- **Frame rate:** 60 fps standing or moving (Status above).
- **World:** streams and rivers; vines on trees; more than six terrain paint layers (mud, moss, litter, scree); a
  continuous cliff line; scanned shrubs; water with reeds and lily pads; far-terrain silhouettes (128 m polygons).
- **Light and air:** bounce lighting; contact shadows (blocked by the forward renderer); soft shadow penumbra; froxel
  fog; depth of field; leaf gloss and wetness; SH sky ambient for grass and meadow plants; cloud shadows matched to
  the volumetric clouds; long-range shadow on non-terrain objects.
- **Trees:** oak-pack impostors not re-baked with the leaf lighting; octahedral impostors; trimmed leaf cards;
  standalone (non-instanced) prefab trees take 64 ms of shadow submission; more tree models without the per-asset
  CPU cost.
- **Grass:** fade band still visible at ~25-35 m; far palette not matched to the cover colour; LOD distances not
  re-evaluated after thinning; clumping; tiles built ahead of the camera; bushes limited to the grass radius; pale
  ground between blades in the test meadow; blade-mottling cost not measured.
- **Antialiasing:** Vulkan has no MSAA and no TAA; temporal upscaling is serialized but unimplemented.
- **Editor:** terrain generation window; asset-generator tool not exercised in a running editor; AO in editor
  viewports; optional 3D LUT for grading.
- **Not measured:** cost of the 9-tap AO lookup; cliff pieces per cell; the detail model against measured GPU cost
  while turning.

## To investigate

- [x] **Diagonal stripes on plant materials** (user screenshot): reproduced on the landscape ForestFloor with
      `ENGINE_TAA=0` (`.local/foliage/stripe_notaa.png`). Cause: screen-space ambient occlusion. The AO pass
      rotates its two slices per pixel with interleaved gradient noise, and `ScreenAmbientOcclusion`
      (Object.hlsl) read it back with a bilinear 2x2 lookup; on wind-moved leaves the AO history is
      rejected, so the noise's diagonal structure showed as dark bands in occluded areas (TAA only smeared
      them into blotches). Fix: a depth-matched 3x3 tent lookup (9 taps instead of 4), the footprint that
      noise is uniform over. `.local/foliage/ao_taa0_crop.png`: gone with TAA off too. Not the shadow map
      (ground plants cast no shadows) and not the canopy taps. Cost of the 5 extra taps not isolated.
- [x] Mossy rock scans read as large orange sandstone: tinted [0.42, 0.5, 0.4] and shrunk
      (`.local/foliage/ao_taa1.png`). The scan still shows little actual moss.
- [ ] Scanned props raised draw counts (ForestFloor 292 → 367 submitted, 1371 → 1825 candidates) and the
      ground views now measure ~31 ms (CPU record ~19 ms; single runs): the per-asset CPU cost again.
      CPU round 3 brought the same view back to 24.0-25.0 ms; the per-candidate cost itself is unchanged.
- [ ] Detail budget sits at its 0.6 floor on every landscape view (`detail_geometry_ms` 13.3 against a
      6 ms budget, `detail_cpu_render_ms` 17.8): check what it measures now that GPU geometry is ~9 ms.
- [ ] One of fourteen landscape runs on 2026-11-19 ended during loading with zero frames and no error text (the
      window-closed path in `GameApplication`, exit code 2). Not reproduced in the seven runs after it. It came one
      build after the static-item reuse test moved onto workers, so watch for it.
- [x] Reference video "[Chlorophyll Engine] 10M Trees - 15km² Test" (user: "can we achieve this?"): four frames
      saved, press coverage read, the frames and the demo measured. Result: the "Landscape demo expectations"
      section above. The look needs no technique this renderer lacks; the tree count and frame rate claimed for
      that engine are developer-reported and were not compared.
- [ ] Capture runs that render nothing: 4 of ~50 runs on 2026-11-20 produced a uniform grey image with
      `camera_record_ms` 0, `scene_replay_ms` 0 and a 6-9 ms frame, yet reported "valid" (twice Vista, once
      ForestFloor, once the first no-cookie Vista). The benchmark should fail a run whose camera view never
      recorded; until then check `camera_record_ms` before trusting a capture.
- [ ] Count this demo's trees per km² in forest (near placements plus far blocks) and record the frame time of
      a walk through the densest forest, for a like-for-like scale number.

---

## Foliage and world realism

Started 2026-11-19 from six reference shots (RDR2 forest floor, GTA VI undergrowth and coastal palms, mangrove
channel, backlit pine): light, trees, undergrowth, atmosphere and the performance to pay for them. Line numbers are
at `88efe11` and will drift.

### What the references have that we don't

Looked at side by side with `.local/grass/final_start.png` / `final_thicket.png` and `.local/woodland-gen/cap4_*.png`.

| In the references | Engine today |
|---|---|
| Canopies shade as **volumes**: lit rim, dark interior, soft falloff | Each leaf card is lit by its own flat normal; crowns read as speckled flat green |
| Leaves **glow when backlit** (pine, palms, RDR2 sun patch) | Oak `Cluster_Mat 2`: `subsurfaceEnabled false`, `subsurfaceTransmission 0`. `PSFoliageMain` (bushes) is Lambert only. Only grass transmits |
| Ambient is **sky from above, dark below**; forest interiors much darker than meadows | `PSFoliageMain` / `PSGrassMain` use the flat `ambientUnlit.rgb` ([Object.hlsl:2349](../Engine/Core/Shaders/Object/Object.hlsl#L2349), [2452](../Engine/Core/Shaders/Object/Object.hlsl#L2452)), not the SH sky (`EvaluateEnvironmentIrradiance`). No large-scale canopy occlusion; GTAO is half-res and short range |
| **Dappled light** on the forest floor; small plants shadow each other | Cascade shadows only; grass `castShadows = false`; no contact shadows; no soft/contact-hardening penumbra |
| **Glossy leaf sheen** (RDR2 broadleaves, wet GTA VI leaves) | Oak roughness 0.86, `reflectionStrength 0.18`; bushes have no specular term at all |
| Dense, varied **undergrowth**: ferns, broadleaf herbs, dead stalks, leaf litter, fallen needles | Meadow tussocks, accents, bushes. Nothing made for under trees; the forest floor is bare terrain (`.local/grass/final_thicket.png`, dark red-brown strip under the treeline) |
| Everything **moves** | One wind model: the grass root-pinned bend (`grassAnimation`, [Object.hlsl:553-569](../Engine/Core/Shaders/Object/Object.hlsl#L553-L569)). Meshes can opt in with `Mesh.foliageWindStrength`, but the oak/bush prefabs set it to 0, and there is no branch or leaf motion |
| Light shafts in haze, depth of field, muted filmic greens | Shafts marched in the forward shaders (good start, not yet seen as distinct shafts); no DOF; ACES + split-tone grading |

**Not the gap:** grass blade shading (pelt normals, transmission, Kajiya-Kay sheen), TAA/MSAA, aerial
perspective, volumetric clouds, GTAO, coverage-cooked alpha mips. Those exist and are fine.

**Performance when this started:** the fully settled vista measured **42.8 ms** (Foliage Phase 0, 2026-11-19;
~25 ms now, see Status), and it was CPU-bound: draw recording (25 ms) + scene prepare (11 ms) + update (5 ms) run back to back, while the GPU needs
~13 ms. Grass is 3.6-3.8 ms on grass_test. So better-looking foliage has to come with savings, not just new
cost, and the vista needs CPU work before GPU work.

### Frame budget (16.7 ms, 1920x1080, DX12)

Starting split to measure against; revise after Phase 0.

| Stage | Budget |
|---|---|
| Near foliage (trees < ~150 m, bushes, undergrowth) incl. depth | 4.5 ms |
| Far forest (impostors, clusters) | 1.5 ms |
| Grass (detailed + far ring) | 3.0 ms |
| Shadows (cascades + contact) | 2.0 ms |
| Terrain, sky, clouds, water | 2.0 ms |
| Post (TAA/upscale, AO, fog/volumetrics, bloom, DOF) | 2.0 ms |
| Headroom | 1.7 ms |

### Research references (foliage)

From memory, not re-checked this session.

| Source | Takeaway |
|---|---|
| Tiago Sousa, *Vegetation Procedural Animation and Shading in Crysis* (GPU Gems 3, ch. 16) | Three-level vertex wind (trunk bend, branch sway, leaf flutter) driven by vertex colour channels; leaf translucency term. |
| Gilbert Sanders, *Between Tech and Art: The Vegetation of Horizon Zero Dawn* (GDC 2018) | Crown-shaped normals, baked per-vertex occlusion, translucency; how they kept foliage cheap on PS4. |
| Unity, *Book of the Dead* (2018) | Practical foliage translucency, normal tweaks and wind on scanned assets. |
| Ryan Brucks, *Octahedral Impostors* (shaderbits.com, 2018) | Hemi/octahedral view atlas with normals + depth, frame blending; far trees that relight correctly. |
| Emil Persson, *Particle trimming* (humus.name, 2012) | Fit card geometry tightly to alpha; cuts alpha-tested overdraw roughly in half. |
| Haar & Aaltonen, *GPU-Driven Rendering Pipelines* (SIGGRAPH 2015) | Two-phase Hi-Z occlusion culling in compute; forests occlude heavily. |
| Fabian Bauer, *Creating the Atmospheric World of RDR2* (SIGGRAPH 2019 Advances) | Froxel volumetrics and how much of RDR2's foliage look is light in the air. |

Phases are ordered by payoff per cost. Phase 1 is shader and asset-cook work with almost no frame cost; Phase 2 buys
the headroom for Phases 3-6. Phases 1b, 1c, 1d and 7 came from user reports during the work.

Ordered by payoff per cost. Phase 1 is shader and asset-cook work with almost no frame cost and should
close most of the visual gap on trees. Phase 2 buys the headroom for Phases 3-6.

### Foliage Phase 0: Baseline and look targets
- [x] **Test scene** `tools/build_foliage_test_scene.py` → `Scenes/terrain/foliage_test.scene`: the grass
      test cell plus a grove (oaks, a conifer edge, saplings, pack bushes; 84 placements in an instanced
      `TerrainObjectLayer`) and saved viewpoint cameras **ForestFloor**, **Backlit** (looking up through
      the crowns into the sun) and **Meadow**. Loads in seconds. Capture with
      `.local/foliage/capture.sh <tag>` (DX12, 1920x1080, 300 warm-up + 120 frames; `topng.py` makes
      half-size PNGs).
- [x] Baseline (`88efe11`, RTX 4090, DX12, median frame / GPU leaves / bark / grass, ms):
      ForestFloor 6.7 / 0.76 / 0.92 / 2.5, Backlit 7.5 / 0.53 / 0.69 / 1.7, Meadow 4.3 / 0.33 / 0.32 / 1.9.
      Captures `.local/foliage/base_*.png`: crowns evenly lit flat green with no glow or dark interior
      even straight into the sun; the floor under the grove is as bright as the meadow; the generated
      meadow bushes read as large flat polygons up close.
- [x] **Found: standalone prefab trees hit a very slow CPU path.** The same 84 trees as scene prefab
      objects (`--standalone`) take 55-70 ms a frame, almost all in `shadow_submission_ms` (64 ms CPU
      while `gpu_shadow_ms` is 0.5). Instanced layer placements: 0.3 ms. Matters for hand-placed hero
      trees; see Phase 2.
- [x] Vista baseline (`--viewpoint Vista`, settled, 600 + 300 frames): **42.8 ms median**, p95 50.3.
      **CPU-bound, not GPU-bound:** GPU stages sum to ~13.4 ms (opaque 9.9: foliage depth 4.75, leaves
      2.0, other 2.7; clouds 0.8, shadow 0.95, terrain 0.7, grass 0.6), while update 5.1 + prepare 10.8 +
      record 25.4 (camera 18.1, shadow 6.3) run back to back and add up to the frame. `detail_scale`
      sits at its 0.6 floor. So the 60 fps floor on the vista is mostly a CPU draw-recording problem;
      Phase 2 needs a CPU item, not just GPU overdraw work.
- [x] Reference shots saved whole in `.local/reference-images/` (second set of eight; the first six in
      `first-set/`). No per-pose crops were made.

### Foliage Phase 1: Leaves lit as volumes (biggest visual win, ~free)
- [x] **Crown normals + canopy occlusion** (cook step `tools/cook_foliage_lighting.py`): finds every
      alpha-masked, non-impostor leaf mesh through the woodland prefabs (30 chains, 76 meshes: oaks,
      conifers, pack bushes). Per chain, LOD0's leaf vertices are fitted with Gaussian lobes (k-means, one per
      ~4 m of crown, up to 8) whose sum is the crown field; every LOD gets the same crown. Each vertex:
      normal bent 75 % toward the field's outward normal (card normal flipped to the outer side first,
      tangent re-orthogonalised, handedness kept); `uv1.x` = canopy occlusion
      (`(1 - 0.8 * depth^0.85) * (0.4 + 0.6 * up)`, floor ~0.16); `uv1.y` = 4 + depth into the crown. The 4.0
      offset marks the mesh as cooked (leaf uv1 was 0 or a copy of uv). Cooking overwrites normals, so it skips
      cooked meshes; restore from git before re-cooking. `generate_conifers.py` writes
      `foliageTranslucency` and calls `cook_prefabs` after writing its meshes.
- [x] **Leaf lighting** (`Material::foliageTranslucency` 0-2, carried in `subsurfaceTransmission.y`; no
      object buffer growth). `Object.hlsl`: `GetFoliageLeaf`, `FoliageWrapExtra`, `FoliageTransmission`,
      used by `ObjectSurfacePS` (oaks, conifers) and `PSFoliageMain` (pack bushes; it now takes `uv1`).
      With it on: cooked normals are not flipped on back faces; ambient/environment × canopy occlusion;
      direct × `lerp(0.45, 1, occlusion)`; wrapped diffuse past the terminator; transmission = back term
      0.35 + forward scatter `pow(dot(-v, l), 4)` × 0.9, filtered yellow-green, × `lerp(0.08, 1, shadow)` ×
      occlusion², so the shell glows and the interior stays dark. Set on `Cluster_Mat 2` (1.0),
      `Conifer_Needles` (0.8), `phong3 1` bushes (1.0). Oak leaf `ambientColor` halved (it is a flat extra
      ambient on top of the SH sky light and lit crowns evenly from every side).
- [ ] **Leaf gloss**: tried oak leaf roughness 0.86 → 0.6. No visible gain in the test poses, and on the
      vista (looking toward the sun) crown tops caught a white grazing sun glint (69 px > 170 in the
      near-forest crop vs 0 before). Reverted to 0.86. Revisit with the wetness look, likely with a
      leaf-specific specular lobe rather than plain GGX.
- [x] Cooked leaf normals that face away from the viewer (crown far side) are bent just into the view
      hemisphere (`n.v >= 0.1`) in `ObjectSurfacePS`, so GGX cannot divide by n.v ~ 0.
- [ ] Oak-pack impostors (`*_Impostor`, the pack's Billboard_Mat) are not re-baked, so they still carry the
      old flat leaf lighting. The forest clusters were re-baked from the cooked meshes (Foliage Phase 1c).
- [ ] **Sky ambient for grass and meadow plants**: `PSGrassMain` / `PSFoliageMain` still use the flat
      `ambientUnlit` instead of SH irradiance on the (pelt/crown) normal. Changes meadow brightness
      everywhere; needs its own tuning pass with the Grass section.
- [ ] `wetness` scene value (lower roughness, darker albedo) for the GTA VI rain look.
- [x] Check (foliage_test, `.local/foliage/{base,p1e}_*.png`): Backlit crowns have dark interiors and lit,
      glowing shells toward the sun (before: evenly lit flat green with white glints); looking up from
      ForestFloor the canopy has depth instead of uniform bright green; Meadow crowns read as rounded.
      Backlit looks the same on DX11 and Vulkan (`.local/foliage/p1_Backlit_{dx11,vulkan}.png`).
      Leaf GPU change +0.02-0.08 ms (ForestFloor 0.72 → 0.77, Backlit 0.56 → 0.64, Meadow 0.31 → 0.33);
      other stage differences are run-to-run noise. Tuning runs: p1a = translucency only (uncooked: whole
      crown glows evenly), p1b = cooked, p1c = + gloss, p1e = + darker undersides / halved ambient.
- [x] Vista (`.local/foliage/p1g_Vista`): far forest unchanged (impostors/clusters dominate, no visible
      near/far mismatch at this pose); GPU leaves 2.01 → 1.59 ms, GPU stage sum 13.4 → ~11 ms, i.e. no
      regression. Frame time can't be compared today: four runs with no relevant change measured
      42.8 / 30.7 / 27.7 / 21.9 ms, all stages moving together (CPU load on the machine).
      Final test-pose numbers (p1): ForestFloor 7.5 ms / leaves 0.76, Backlit 7.7 / 0.59, Meadow 4.6 / 0.33.

### Foliage Phase 1b: Blocky bushes and the landscape demo (2026-11-19, user report: "bushes look blocky")
- [x] **Cause: grass LOD measured from height 0.** GrassField tile objects sat at y = 0 with ground heights
      inside the placements, so LOD selection and fades measured distance from a point tens (test cell) to
      hundreds (landscape) of metres above or below the plants: nearly every grass plant and bush drew LOD2
      at the viewer's feet. Fixed with `TileState::originHeight` (ground at the tile centre is the tile object's y; both build
      paths, procedural `AdvanceBuild` and authored `BeginBuild`, subtract it from placements and bounds;
      world positions unchanged). Grass now draws the detail it was tuned for: grass GPU on foliage_test
      2.0 → 3.3 ms (ForestFloor), 1.3 → 2.3 ms (Meadow); the detail budget balances it.
      Found by swapping LOD meshes (LOD2 := LOD0 fixed the near bush), then moving the layer's LOD
      distances to 30/35 m (no change), then a capture near the cell origin.
- [x] **Bush leaves** (`generate_meadow_grass.py`): ovate leaf outlines per LOD (10/6/2 tris) instead of
      diamonds, random leaf roll (no tiers), value jitter, gentler coarse LODs, no core at LOD0. Shared by
      meadow_bush(_tall), meadow_shrub and bramble.
- [x] **Landscape demo viewpoints** `tools/add_landscape_viewpoints.py` (also run by
      `build_landscape_demo.py`): **ForestEdge**, **ForestFloor**, **Backlit**, found near the start by
      replicating LandscapeDriver's `ForestMask`/`ForestBand` and reading ground from the height archive.
      The demo's start pose flies 25 m up, where grass and bushes (26 m radius) are out of view.
- [x] Checked on the landscape (`.local/foliage/land_*.png`, DX12): ForestEdge shows dense full-detail
      meadow grass and leafy bushes; Backlit crowns glow toward the sun; ForestFloor is bare forest soil
      with rocks and pale, over-exposed trunks (no undergrowth: Phase 4, and Phase 3 sky visibility).
      Timings (median frame / CPU record / GPU sum / foliage depth, ms): ForestEdge 27.1 / 16.2 / 14.7 / 5.1,
      ForestFloor 24.9 / 14.8 / 13.3 / 5.1, Backlit 26.3 / 14.5 / 14.2 / 4.2. **37-40 fps at ground level**:
      both CPU recording and GPU (alpha-tested foliage depth) are near 15 ms, so Phase 2 needs both.
- [x] **Dark diagonal stripes on trees** (user report): the per-placement LOD dissolve and the impostor view
      hand-over threshold each pixel with `ScreenDitherThreshold`, which was interleaved gradient noise. Its
      diagonal line structure showed through as dark bottom-left-to-top-right stripes on crowns sitting in a
      dissolve (the forest behind between them; ~1/3 of impostor angles are a hand-over). Now an 8x8 Bayer
      matrix (still complementary per pixel; no line structure). `.local/foliage/dither_zoom.png` vs
      `bayer_zoom.png`.
- [x] Shadow acne guards found on the way: cooked leaves offset their shadow lookup along the real card
      normal (`FoliageShadowNormal`, screen-space facet turned toward the light) instead of the crown-bent
      shading normal; tree-wind vertices pass their rest position to the pixel shader (only the rasterized
      position moves), so shading matches the static shadow casters.
- [x] **TAA on by default with an animated dissolve dither**: while TAA jitters a view, SceneRendering stamps
      the jitter index (1..8) into `ObjectGPUData::skinParams.z` (was unused) and `ScreenDitherThreshold`
      rotates each pixel's Bayer threshold by the golden ratio per frame, so the history resolves LOD
      cross-fades and impostor hand-overs to a smooth blend (`.local/foliage/taa2_zoom.png`; shifting the
      matrix instead left faint diagonal hatching). `AntiAliasingSettings::taa` default false → true and the
      editor's Balanced preset; Performance stays off, Quality stays MSAA 4x (static Bayer there, as on
      Vulkan, which has no TAA yet). Projects with saved Effects settings keep theirs.

### Foliage Phase 1c: Tree variety (2026-11-19, user report: "a lot of the same trees")
- Cause: `LandscapeDriver::GenerateForest` grew one species per size band per 128 m cell (every large tree in
  a cell the same model, varied only by yaw and 0.8-1.25x scale), and uses 8 of the 17 oak models (all 17
  measured ~8 ms more CPU per frame: batches merge across cells only per asset).
- [x] Two species per size band per cell, mixed tree by tree (all eight can appear in one cell); scale
      0.75-1.3x; a lean via the placement normal (oaks up to ~7 degrees, squared so most stand straight;
      conifers 2.5). Derived from the existing per-tree draws, so positions are unchanged; far blocks copy the
      normal, so near and far trees agree.
- [x] Cost (single runs): draws +6-9 % (vista 298 → 316), triangles +1-3 %, no measurable frame-time change
      (vista 21.9 → 21.7 ms). Captures `.local/foliage/p3_*.png`.
- [x] Far clusters: 4 → 7 stands (`generate_forest_clusters.py`: Oak_03/Oak_04 from the oak variants the
      near forest leaves out, Conifer_03), all re-baked from the cooked meshes (crown normals in the normal
      atlases); `TerrainManager.farClusters` lists them (scene + builder). Bake ~8 min.
- [ ] More models without the per-asset CPU cost: needs batches that merge across assets (one instanced
      draw over several meshes sharing a material), or per-instance shape variation beyond scale and lean.
- [ ] Whitish highlights on crowns next to the sun in Backlit (seen before this change too).

### Foliage Phase 1d: Less cartoon (2026-11-19, user report: "style is very cartoonish")
- [x] **Plant palette** near measured albedo: grass blades were ~2x too bright and far too saturated
      (`BLADE_MID` 0.13/0.28/0.055 → 0.065/0.125/0.032, tips, sward, ferns, shrub/bramble/herb sun colours, accent
      stems and leaves likewise); dry blade share 10 % → 22 %. All grass/accents meshes regenerated.
- [x] `GrassLeafiness` is relative to brightness now (the absolute green difference would have halved for
      the darker palette and changed pelt shading).
- [x] Blade mottling in `PSGrassMain`: two octaves of world-space value noise (7 cm, 1.8 cm), ±14 % value with
      a slight hue shift, faded out by 25 m.
- [x] Terrain meadow to match: `grass.material` base colour 0.2/0.26/0.11 → 0.11/0.145/0.065, LandscapeDriver
      layer preview colour likewise.
- [x] Landscape tone mapping ACES → **AgX** (scene + builder).
- Result (`.local/foliage/p8_land.png`, `p9_Vista.png`): dark natural grass, muted sky and greens.
- [x] foliage_test Meadow: ground between blades read paler than the grass, and tint strength 0.5 → 0.8 changed
      nothing. Cause found 2026-11-20: the forest-floor field cleared the meadow's ground cover every refresh, so
      there was no tint at all (look pass 1 under "Landscape demo expectations").
- [ ] Blade mottling cost not isolated: grass GPU on the Meadow pose read 2.4-4.1 ms across runs with
      identical instance counts (other GPU load); needs an on/off pair.
- [ ] Vista: meadows and forest are now close in tone; consider a slightly lighter/yellower far meadow.
- [ ] Generated bushes still read as flat cards up close: textured (photographic) leaf atlases are the next
      step for plants seen at arm's length.

### Foliage Phase 7: World detail (2026-11-19, user request: paths, streams, small things, cliffs/overhangs)
Asset inventory: the repo's only textured, LOD'd vegetation is the oak pack (17 models, 8 used), nine pack
bushes, twelve scanned rocks and one log (`Assets/Other` also holds an unused `grass_patches_-_circle` and the
`real_bush` source). Everything else is generated. The terrain shader has six fixed paint layers.
- [x] **Trails** (`LandscapeDriver.cpp` `TrailMask`): tracks follow the 0.5 contour of a broad noise
      (distance = value / gradient), so they wind across the whole map and join in loops; ~6 m wide (terrain
      vertices are 4 m apart). Painted as gravel on gentle land inside the forest band; trees, undergrowth
      and props stay off; grass and the forest floor follow the paint. `PaintRevision` bumped (terrain
      caches rebuild). `.local/foliage/w_Trail.png`.
- [x] **Cliff bands**: `GenerateCliffs` grows each steep-rock seed along the contour (up to 8 blocks a cell,
      was 2 lone blocks), every block facing downhill. New shapes from `generate_boulders.py`
      (`Rock.overhang` shear): `Cliff_overhang_01` (top leans ~3 m over its foot) and `Outcrop_ledge_01`.
- [x] **Scree**: rocks and boulders gather where a steep slope lies 9-18 m uphill.
- [x] **Forest debris**: `forest_branch` (fallen branch with twigs) and `ground_ivy` (creeping ivy patch) in
      the forest-floor drifts.
- [x] Steep rock rendered near black: the canopy-shade estimate's shadow taps landed inside the hillside.
      Canopy shade now fades out on terrain steeper than ~35 degrees.
- [x] Crags: `Crag_face_01/02` (28-38 m wide), `Crag_spire_01` (30 m tall), LODs 20480/5120/320 tris; up to 40
      pieces a cell (was 8), strides sized per piece so neighbours overlap, leaned a third with the slope.
      `.local/foliage/w_Cliffs3.png`: a rocky outcrop instead of scattered boulders.
- [x] **Scanned rock faces** (downloads approved): `tools/fetch_polyhaven_props.py` fetches CC0 Poly Haven
      models into `.local/polyhaven` (FBX + 2k diffuse/normal/roughness JPGs), cooks LOD chains with PropCook
      and writes materials and prefabs under `Models|Prefabs/Woodland/Scans` (62 MB): `RockFace_01/02`,
      `Mountainside_01`, `Cliff_Scan_01/02` (8-20k / 2.5-6k / 0.5-1.7k tris). `GenerateCliffs` now places
      these as the main pieces (scaled 2.2-4.5x to 20-45 m), generated spire/overhang/ledge as filler, only
      where the slope keeps rising behind them. Materials are two-sided with a cool tint (the scans are warm
      sandstone, the terrain rock grey). `.local/foliage/w_Cliffs4.png`: reads as fractured, bedded rock.
      Pitfalls: PropCook is `EXCLUDE_FROM_ALL` (a stale binary wrote one prefab per LOD; rebuild it by
      name); the scans are open shells, so a camera behind one sees its back.
- [ ] Cliff follow-ups: a proper wide viewpoint (the capture pose ended up behind a piece; the existing
      "Cliffs" viewpoint is too close under the slope); 2k textures stretched 4x are soft up close (use 4k
      or add a triplanar detail layer); colour match to terrain rock is a tint only; bare steep slopes
      where rock paint is under the threshold; pieces per cell (40) and their LOD0 cost not profiled.
- [ ] **Not yet a continuous cliff line** (user: "repetitive flat/steep terrain, nothing like the rocky edge of a
      mountain"): the generated pieces read as separate rounded lumps (one radial shape each), the strata
      pattern looks like honeycomb, and slopes whose rock paint is below the threshold stay bare. Generated
      shapes have hit their limit, and the scanned rock faces (above) read as real rock but still as an
      outcrop. Next: ledges/terraces carved into the height archive (`build_landscape_demo.py`) so the
      scans sit on a real break in the slope, and rock on the bare steep slopes.
- [ ] **Streams**: not done. The erosion flow map is too coarse (macro grid) to place a stream bed, and
      running water needs a carved channel in the height archive plus water surfaces that follow the
      slope (the water system is flat lakes). Plan: trace downhill polylines from springs in
      `build_landscape_demo.py`, carve them into the heights, export them, and draw ribbon water meshes.
- [ ] **Vines on trees**: not done (needs per-trunk geometry or a trunk-wrap generator). Ground ivy only.
- [ ] **More terrain materials** (mud, moss, leaf litter, scree): needs more than six paint layers in the
      terrain shader, or a detail/decals layer.
- [x] **Scanned forest props** (Poly Haven, CC0, via `fetch_polyhaven_props.py`; sets cook one prop per
      mesh node, leaf scans merge their Alpha map into the colour): `Stump_01/02`, `MossRock_01-06`,
      `DryBranch_01-03`, `FernScan_01-04` (masked, `foliageTranslucency`). Placed by `GenerateProps`: stumps
      with the logs, dry branches on the forest floor, mossy rocks instead of plain rocks inside forest,
      ferns in drifts in the undergrowth layer; one stump, one branch, two rocks and two ferns per cell.
      `Models/Woodland/Scans` is 107 MB. `.local/foliage/s_ForestFloor.png`, `s_Trail.png`.
- [ ] Scanned shrubs (Poly Haven shrub_02-04, nettle_plant; alpha-masked, so they need card-friendly LODs)
      to replace the generated bushes. The small scanned props have not been checked close up in a
      capture, and the mossy rocks show little moss.
- Cost (single runs, DX12): Trail 27.1 ms, ForestFloor 26.0 ms, both CPU-bound as before (record ~16.5 ms,
  GPU 11.7-13 ms).

### Foliage Phase 2: Pay for it (performance)
Progress 2026-11-19 (landscape ForestEdge, DX12 1080p, single runs; frame / update / prepare / record ms):
27.1 / 1.8 / 7.8 / 16.2 → 24.1 / 1.6 / 6.8 / 14.4. Still ~41 fps: CPU-serial and GPU (~14.5 ms) both short.
- [x] **Indexed woodland meshes** (`tools/index_woodland_meshes.py`): the oak/bush pack meshes were plain
      triangle lists; only 35 % of their vertices were unique (148 meshes, 1.06M → 377k vertices). Rewritten
      indexed (byte-identical vertices merged, first-use order). foliage_test GPU: bark 1.39 → 0.79 ms,
      foliage depth 1.40 → 1.12 ms (ForestFloor); identical image.
- [x] `PrefabLod::IsPrefab` built a `std::filesystem::path` (with a wide-string conversion) per call, once per
      placement of each object layer it was checked on; now an allocation-free suffix test (same `#lod`
      rules). `PreparePrefabInstances` only rescans placements for prefabs when they change.
      `prepare_prefabs_ms` 3.0 → 2.2.
- [x] Occlusion signature hashed ~90 bytes per visible item one byte at a time (FNV); now 8 bytes per step.
- [x] Camera draw records (`PreparedDraw`, ~2 KB) are built in place in the reserved vector instead of
      zero-filling a local and copying it (`_NLG_Return2` under `push_back` was ~5 % of the thread).
- **Finding: the landscape's foliage cost is geometry, not pixels.** At 960x540 foliage depth only drops
  4.98 → 4.07 ms (leaves 2.06 → 1.33). 41M triangles submitted (25.5M candidates), 59.5k placements at LOD0,
  detail budget pinned at its 0.6 floor (13.2 ms geometry vs a 6 ms budget). The Detail view shows the near
  and mid trees on the right bands; the cost is the forest *behind* the visible edge. The CPU occlusion test
  hides 282 of 2356 tested items (whole tiles). → GPU Hi-Z occlusion per placement is the main lever for
  forest views; card trimming is secondary (leaf cards are only 36-39 % opaque, but this view is not
  pixel-bound).
- [x] CPU, round 2 (from a `--cpu-sample` profile at ForestEdge): prefab LOD profiles refreshed every
      100 ms per forest/props layer (~10 % of the scene thread in string work) → every 1 s, matching the
      file-stamp checks underneath; `TransformHierarchy` membership by a process-unique stamp on each
      Transform instead of an `unordered_map` lookup per render candidate per frame (~3 %). Machine-wide
      CPU swings (±30 % between identical runs today) hide these in single-run frame times; confirm with the
      profiler's shares or interleaved A/B.
- [x] CPU, round 3 (profiles `.local/foliage/prof3` → `prof6`, landscape ForestFloor, profiler attached:
      frame 34.0 → 27.1 ms, prepare 9.2 → 6.4, camera record 15.5 → 12.0; without the profiler 25.5 ms):
  - Prefab preparation ran to its full 3 ms deadline every frame even with nothing to build. The
    deadline is now 0.75 ms once every object layer reports its prefabs complete, 3 ms while any is
    still building (`m_prefabPreparationSettled`). `prepare_prefabs_ms` 3.0 → 0.8.
  - The detail-budget cost model copied the placements of every visible instanced tile on the scene
    thread at each launch (every 100 ms; `memmove` ~5 % of the thread, and a spike on those frames).
    Placements are now snapshotted once per tile revision and shared with the job
    (`DetailCostCollector::snapshots`, dropped ~3 s after a tile leaves view).
  - A view's draw records (~2 KB each, a few thousand per view) lived in a vector allocated and freed
    per `Render` call: several MB of fresh pages per view per frame. Storage is now leased from a
    per-thread pool (`PreparedDrawLease`); the camera path also fills the object record in place.
    Camera material loop 4.8 → 3.3 ms.
  - `TransformHierarchy::Prepare` (called twice a frame over 10.4k transforms) settles unchanged frames
    in one pass instead of building the parent list and scanning again. Small: 6.2 → ~5 % of the
    thread; the cost is touching every Transform, which needs dirty tracking to remove.
  - Cached frame items are copy-constructed in place; texture footprints refresh every 8 frames (was 4):
    footprint stage 2.8 → 2.0 ms.
  - The "is this static item unchanged" test runs on the workers for members of the prepared transform
    hierarchy (merged into the material validation pass; `TransformHierarchy::IsPreparedMember`);
    the serial loop only copies the item. `prepare_items_ms` 4.1 → 3.0-3.7.
  - The per-frame render packet is recycled (`NativePacketRecycler`): its ~3 MB item list was a fresh
    allocation every frame. `prepare_items_ms` → 2.6, `scene_prepare_ms` 6.2 → 4.7.
  - Checked on landscape ForestFloor, foliage_test ForestFloor and the animation showcase (animation
    still advances), DX12 only. One of fourteen landscape runs ended during loading with zero frames
    and no error text (the window-closed path); it did not recur in the seven runs after it.
- [x] **Moving camera, first fix.** Still-camera numbers hid the real problem: turning in place had a
      63 ms p95 and walking spiked to 150-230 ms. Cause: `Scene::MoveObject` ended with a whole-scene
      `NotifyStructureChanged()`, so every far terrain tile, far-object block and streamed cell that was
      parented under the terrain manager reconciled the component registry and cleared the static
      render item cache (all 9,124 items rebuilt, ~40 ms a frame for 3-4 frames, about once a second
      while turning). Appending a child now notifies only that object, as `SetParentLocal` does.
      Landscape, 400 frames each (median / p95 / frames over 50 ms): turning 24.3 / 63 / 41 →
      24.7 / 38 / 3; walking 35.0 / 84 / 49 → 31.3 / 51 / 20; walking and turning 39.5 / 90 / 91 →
      35.2 / 59 / 34. World intact after the walk (`.local/foliage/mv2_walkturn.png`).
- [ ] **Moving camera, remaining hitches.** Walking still has clusters of 80-120 ms frames each time a
      cell boundary is crossed (3 clusters in 400 frames): `update_post_physics_ms` 1.3 → 22,
      `update_objects_ms` 1.6 → 13, `prepare_graphics_ms` 0 → 7, `stream_lod_ms` 0 → 6. Walking also
      raises the steady cost (update 1.8 → 4.6 ms, prepare 4.7 → 8.9). Profile a walking run
      (`--drive-forward 380 --cpu-sample`) and spread that work over frames. Every later performance
      check uses the three moving runs as well as the still poses.
- [ ] **Next CPU steps** (still camera: camera view ~11.5 ms, shadow view ~3.9, prepare ~4.7, update ~1.8):
  - Record the shadow view on a worker while the camera view records (they run back to back today;
    ~4 ms). Needs the per-view state in `SceneRenderer::Render` (timings, upload caches) separated.
  - `PrepareFrameItems` still copies all 9.1k items every frame (2.6 ms); a dirty list would skip
    unchanged objects entirely.
  - `TakeNativeRenderPacket` retains two buffers per item every frame (18k shared-pointer copies, then
    18k `RetainBuffer` calls on the render thread); retain each distinct buffer once.
  - Shadow scene preparation (3.6 ms) reruns for the one cascade refreshed each frame even with a still
    camera and a static scene.
- [ ] **CPU draw recording on the vista** (Phase 0: camera record 18 ms, shadow record 6 ms, scene prepare
      11 ms, run serially; now 11 / 3.9 / 6.6). Candidates beyond the steps above: moving more far forest
      onto GPU-driven instance draws (InstanceCull) and overlapping prepare with recording.
- [ ] **Trimmed cards** (cook step): clip each leaf triangle to the opaque part of its texture (oak cards are
      curved 7-11 triangle clusters, 36-39 % opaque by area). Lower priority: ground views are geometry-bound.
- [x] **GPU occlusion pyramid from real depth** (`Shaders/DepthPyramid/DepthPyramid.hlsl`,
      `SceneRendererLighting::BuildDepthPyramid` / `DepthPyramidForCulling`). After the AO pass copies the
      main view's depth, one compute dispatch reduces it to the CPU occlusion buffer's GPU layout
      (`OcclusionBuffer::SerializeForGpu`: 256-wide, 4 levels, farthest view depth per cell, 3e38 for sky, the
      frame's view-projection in the header). Next frame, InstanceCull's existing per-placement
      `IsBoxOccluded` reads it instead of the CPU pyramid (terrain + mesh occluders only) when the camera is
      the same, the viewport unchanged, it is one frame old, and the camera moved < 1.5 m and turned < ~8
      degrees; otherwise the CPU pyramid as before. No new binding slots; Vulkan SPIR-V prebuilt in CMake.
      `ENGINE_GPU_OCCLUSION=0` disables it. Needs the AO depth copy (AO on). Landscape ForestEdge A/B (DX12):
      GPU 15.75 → **11.45 ms**, foliage depth 5.13 → 3.38, leaves 2.26 → 1.18, GPU-selected LOD0 placements
      59,623 → 360, LOD2 9,362 → 2,130; image diff at the noise floor (0.3 % px > 40, wind). Runs on DX11
      and Vulkan. Single-phase (last frame's depth): a newly disoccluded object can lag a frame; a second
      phase against this frame's depth would close that.
- [ ] **Standalone prefab trees**: find why `shadow_submission_ms` is ~64 ms for 84 non-instanced prefab
      trees (Phase 0 finding; `build_foliage_test_scene.py --standalone` reproduces it). The fine shadow
      timers (`geometryDrawMs`, `maxBatchMs`) only record with `Scene::SetUpdateProfilingEnabled`, which
      only the editor profiler sets; expose it to GameBackendCapture first.
- [ ] **Far blocks follow the detail budget** (also listed under Landscape look, section 0): far forest is outside it and is what
      makes the vista GPU-bound.
- [ ] Optional headroom only, not the route to the budget: **temporal upscaling**
      (`SceneSettings::temporalUpscaling` is serialized but unimplemented). Render at ~0.67-0.77 scale with
      the existing TAA jitter/history and upscale in the resolve; pixel-bound foliage cost drops ~1.7x.
- [ ] Check: vista ≤ 16.7 ms with Phase 1 on; record the stage split against the budget table.
      Latest (2026-11-19, during CPU round 3, single runs, DX12): vista 25.7 ms (CPU record 15.7, GPU
      opaque 7.7); ForestEdge 26.9 (record 16.8, GPU opaque 8.8). Before that: vista 28.4, ForestEdge 27.8. **The GPU is now within budget at ground level; the CPU (update + prepare + record ~27 ms,
      serial) is the blocker.** Every CPU stage moved ±30 % together between runs today (machine load), so
      compare CPU changes with interleaved A/B pairs only. The forest floor adds 133 candidate draws and
      ~110 active grass tiles on the vista.

### Foliage Phase 3: Shadows and sky visibility (dappled light)
- [x] **Canopy sky visibility** without a new texture (a global texture slot is still missing on the three
      backends): `CanopySkyVisibility` (Object.hlsl) averages five sun-shadow taps on a 3 m disk, 2.5 m above
      the surface (`FogShaftSunVisibility`, one bilinear tap each), and scales ambient/environment light to
      35-100 % with it; fades out over the last 30 % of the shadow range. Applied to terrain (both paths),
      objects (not cooked leaves, which have crown occlusion), pack bushes and grass. Cost on foliage_test
      ForestFloor ~0.5 ms GPU (grass 1.31 → 1.61). The estimate sits a little downwind of the crowns.
- [x] **Exposure floor**: auto exposure brightened forest interiors back to the meadow's level, undoing the
      darkening. `autoExposure minEv` -8 → -4.8 on the landscape (builder sets it; test scenes copy it): sunny
      meadows meter above it (Meadow pose unchanged), forest interiors stay ~1 stop darker
      (`.local/foliage/ev4.5_ForestFloor.png` is the -4.5 look). A soft partial adaptation (only part of the
      way below a reference EV) would be smoother than a hard floor; not needed yet.
- [ ] **Canopy sky-visibility map** (texture version): top-down crown density around the camera, if the
      shadow-tap estimate's downwind offset or cost becomes a problem.
- [ ] **Soft shadow penumbra** on cascades 0-1: blocker search (PCSS-style) so canopy shadows on the ground
      are sharp near trunks and soft high up. Needs each cascade's light-space depth range in the object
      data to turn depth gaps into a penumbra width (not there today).
- [ ] Sharper near cascade (tighter first split or higher atlas share) for leaf-sized shadow detail.
- [x] **Cloud shadows** as a scrolling sun cookie: `Light::cookieScrollX/Y` (m/s) fill the cookie axes'
      spare `.w` lanes; `DirectionalCookie` / `SunCookie` (Object.hlsl) apply the directional cookie in every
      sun path (objects, fast near terrain, distant terrain per pixel and per vertex, pack bushes, grass).
      `generate_sky_sprites.py` `cloud_shadow` writes `Textures/Atmosphere/Cloud_Shadow.png` (tiling, linear:
      1 clear, 0.3 under cloud, ~38 % cover). Landscape sun: 2.4 km tiles drifting 6/2 m/s. Statistically
      like the volumetric clouds, not positionally matched to them. The test scenes strip the cookie (fixed
      poses for A/B). `.local/foliage/p7_Vista.png`.
      **2026-11-20: from the capture tool the cookie never loaded** (unresolved asset path), and the null
      texture switched the sun off; fixed, and 0.3 → 0.42 under cloud (look pass 1 under "Landscape demo
      expectations"). `p7_Vista.png` therefore did not show cloud shadows.
- [ ] **Screen-space contact shadows**: blocked by the forward renderer: surfaces shade before the frame's
      depth exists (only the foliage pre-pass and last frame's AO copy), so a screen march would read stale,
      reprojected depth. Revisit with a depth pre-pass for all opaque geometry.

### Foliage Phase 4: Forest floor and undergrowth (RDR2 / GTA VI shots)
- [x] Generators (`generate_meadow_grass.py`, forest floor section): `forest_litter` (fallen-leaf patch with
      twigs, static), `forest_herb` (broadleaf rosette ~0.45 m), `forest_seedling` (oak seedling), `forest_fern`
      (~1 m, darker than bracken; `meadow_fern` is now `fern()` with defaults, byte-identical output),
      `dry_grass_clump`. LOD0 triangles: litter 468, herb 126, seedling 104, fern 1120, dry grass 1080.
- [x] **Forest floor layer**: `MeadowGrass::ApplyForestFloor` for a "Procedural forest floor" object layer
      (litter 2.2/m², herbs 1/m², seedling/dry-grass/plantain drifts, forest fern + herb thickets over 45 %,
      sparse bushes; no ground tint). `LandscapeDriver` points it at the ForestSoil paint; the layer is in the
      demo's `objectLayerNames` (scene + builder). foliage_test paints forest soil under the grove (soft 8 m
      edge) and carries the same field. `.local/foliage/p4b_ForestFloor.png`: ferns, herbs and litter where
      the floor was bare soil.
- [x] Canopy-driven mix, by paint: the meadow field grows on grass paint and the forest floor field on the
      forest-soil paint the trees stand on, so edges blend with the paint weights.
- [x] Forest-floor terrain layer: the landscape already paints `forest_soil` under forests (LandscapeDriver
      `PaintSample`); the litter field covers it.
- [ ] Grass clumping (Grass Phase 7 open item). Dry blades: their share went 10 % → 22 % in Foliage Phase 1d
      and `dry_grass_clump` grows on the forest floor; a dry species mixed by the dry/lush noise is open.
- [x] CC0 scanned plants for hero undergrowth: Poly Haven ferns, stumps, mossy rocks and dry branches are in
      (Foliage Phase 7, scanned forest props). Scanned shrubs are the open part, listed there.

### Foliage Phase 5: Trees
- [x] **Tree wind** without new vertex data (`GrassWind.hlsli` `ApplyTreeWind`, `Mesh::foliageWindTree`,
      `grassAnimation.y` = 2): a cantilever trunk bend (strength x 0.01 x h², mean lean + broad gust + ~0.26 Hz
      per-tree sway), ~0.7 Hz branch sway growing with distance from the trunk axis, ~4 Hz flutter on
      cooked leaves weighted by the canopy shell (uv1). Same wind field as the grass, coarsened so crowns
      move as one. Fades out 60-100 m; shadow casters static (as grass). On for all woodland prefabs (oaks,
      conifers 0.1; pack bushes 0.15) and in `generate_conifers.py` / `woodland_assets.write_prefab`
      (`tree_wind`). `ENGINE_TREE_WIND=0` disables it. foliage_test: crowns move between frames, ~+0.5 ms
      GPU. Note: wind meshes are left out of the CPU mesh occluders (opaque coarsest bands only; trees
      hardly qualified).
- [ ] **Octahedral impostors** (normals + depth + occlusion atlases) replacing the 6-view + top bake, lit
      with the Phase 1 leaf model so far trees keep translucency and dark interiors.
- [ ] Tropical set if the game needs it (palms, mangroves, broadleaf shrubs): palm generator with frond
      cards; mangrove prop roots. Decide with the biome plan.

### Foliage Phase 6: Air and camera
- [ ] Froxel volumetric fog (sun shafts through canopy, RDR2/pine shots). Re-use the cascade shadow; ~160x90x64
      froxels, temporally reprojected; budget ≤ 1 ms. Replaces the per-pixel shaft march in the forward shaders.
- [ ] Depth of field (shots 2, 4, 5): half-res gather bokeh, off by default, for cinematic/photo mode.
- [x] **AgX** tone mapping (`ToneMappingOperator::AgX` = scene 3 → PostProcess.hlsl operator 6; polynomial
      fit of Troy Sobotka's AgX; editor Output Tone Mapping combo). Lifted shadows, softer highlights and less
      saturated sky/greens than ACES (`.local/foliage/agx_*.png` vs `aces_*.png`). ACES stays the default;
      a per-scene choice.

---

## Grass

Notes from a review of the grass system (2026-11-19). Three problems to fix:

1. **Pop-in.** Grass appears in visible squares at the edge of its range as the camera moves.
2. **Weak coverage.** The flat terrain shows clearly between grass clumps.
3. **Flat distance.** Past the grass radius the terrain looks like a painted floor, and the
   grass edge reads as a change in colour, pattern and lighting.

Line numbers below were correct at commit `0206428` and will drift.

### How grass works

| Piece | Where | Notes |
|---|---|---|
| Streaming / tiles | [GrassField.cpp](../Engine/Core/Assets/Grass/GrassField.cpp) | 4 m tiles around the viewer. Each tile is one instanced Mesh with all species and all 3 LODs. CPU prepares placements only; blade transforms run on the GPU. The tile object sits at ground height (`TileState::originHeight`). |
| Procedural placement | `GrassField::BeginBuild` (≈L582-705) | Short + tall tussocks, accent drifts (flowers), shrub thickets (value noise). Masked by terrain paint layer. |
| Meadow preset | [MeadowGrass.cpp](../Engine/Core/Assets/Scripts/Grass/MeadowGrass.cpp) | Defaults for new fields; procedural layers use `meadow_tussock` / `field_tussock`. |
| Landscape demo tuning | [LandscapeDriver.cpp:799-834](../Engine/Core/Assets/Scripts/Landscape/LandscapeDriver.cpp#L799-L834) | `grassRadius = 26` → layer `viewDistance = 36`, LOD1 7 m, LOD2 13 m, 2.6 short + 0.4 tall tussocks/m², accents 0.18/m², shrubs 0.7/m². |
| Meshes | [generate_meadow_grass.py](../tools/generate_meadow_grass.py), [generate_meadow_accents.py](../tools/generate_meadow_accents.py) | Ribbon blades ~1.7 cm wide. Tussock = crown of blades + low sward (84 blades, r 0.6 m, h 0.22 m). LOD2 keeps 35% of sward, widened 1.9x. |
| Wind | [GrassWind.hlsli](../Engine/Core/Shaders/GrassWind.hlsli) | Root-pinned rotation bend, advected Perlin gust fronts + per-blade flutter. Good; keep. |
| Interaction | [GrassFootBend.hlsli](../Engine/Core/Shaders/GrassFootBend.hlsli) | Capsule push-aside. Keep. |
| Surface shading | [Object.hlsl:266-290](../Engine/Core/Shaders/Object/Object.hlsl#L266-L290) `GrassColorVariation` | Dry/lush Perlin patches (period 11 m) + per-clump brightness/hue jitter, keyed to placement root. |
| Fades | [Object.hlsl](../Engine/Core/Shaders/Object/Object.hlsl) `ApplyGeometryFade` | Dithered clip for LOD crossfades and tile visibility. |
| Terrain under grass | [grass.material](../Engine/Core/Assets/Materials/Terrain/PBR/grass.material) | Photo texture `Grass004`, base colour [0.11, 0.145, 0.065] (darkened with the plant palette, Foliage Phase 1d). Tinted toward the grass by the ground cover (Grass Phase 2). |
| Range scaling | `GrassField::DetailRadiusScale` + [ProjectLayers.h](../Engine/Core/Model/ProjectLayers.h) | Adaptive detail scales grass reach 0.5x–2x at runtime; "Grass" layer budget weight 1.25 (shed first). |

### Diagnosis (review of 2026-11-19, before the phases below)

**Pop-in**
- **The fade is per tile, not per plant.** [GrassField.cpp:917-927](../Engine/Core/Assets/Grass/GrassField.cpp#L917-L927)
  takes the distance to the tile's nearest edge and dithers the whole 4 m tile over `distanceFadeWidth = 2 m`.
  A 2 m band at the edge of a ~36 m radius is too narrow, so tiles show as squares.
- **Tiles are built late.** One tile is built per refresh, within a shared 2 ms budget
  ([GrassField.cpp:1024-1026](../Engine/Core/Assets/Grass/GrassField.cpp#L1024-L1026)). Tiles only exist once
  they are inside the radius, so a fast-moving camera gets late tiles, which then also wait out
  `fadeDuration = 0.35 s`.
- **The radius moves.** Adaptive detail can shrink grass reach to 0.5x (≈18 m) on a low-spec PC. The scale is
  smoothed and quantised (12% steps), but the edge still visibly slides when the budget changes.

**Coverage**
- Clumps at 2.6/m² with thin blades leave ground visible between them, especially from above.
- Thinning starts very close (LOD1 7 m, LOD2 13 m), and LOD2 widening (1.9x on 35% of the sward) does not
  keep coverage constant.
- Gaps show the `Grass004` photo texture, whose colour and pattern differ from the vertex-coloured grass,
  so they read as "ground showing through" rather than shadowed undergrowth.
- Grass shadows are off (`castShadows = false`), so nothing darkens the base of the field.

**Flat at a distance**
- Past the radius there is only the tiled terrain texture. The grass's colour pattern (dry/lush, clump jitter)
  stops where the blades stop, and so does the soft lighting of a blade field.

### Research references (grass)

| Source | Takeaway |
|---|---|
| [Procedural Grass in 'Ghost of Tsushima' (GDC 2021, Eric Wohllaib)](https://gdcvault.com/play/1027214/contactUs) | Fully procedural GPU grass. Reported (second-hand): far LOD draws ~half the blades at ~2x width. Clumps drive height/colour/direction. Vault page is members-only; talk is also on YouTube. |
| [GodotGrass](https://github.com/2Retr0/GodotGrass) | Reimplementation of the Tsushima techniques: view-space widening of blades seen edge-on, height-based procedural bend. Practical code reference. |
| [Unity Grass (Cain Rademan)](https://cainrademan.github.io/Unity-Grass/) | Voronoi clumping: each cell is a clump with shared height/colour/facing. |
| [Godot shader: screen-space displacement grass](https://godotshaders.com/?p=4133) | Fade by shrinking blades over a start→end distance band. |
| [Codrops: fluffy grass in three.js](https://tympanus.net/codrops/2025/02/04/how-to-make-the-fluffiest-grass-with-three-js/) | Match terrain colour to grass base/tip colour (plus fog) so the cutoff disappears. |
| [Unreal forums: grass only in a round area](https://forums.unrealengine.com/t/i-see-the-grass-only-in-a-round-area-from-the-player/137539) | Per-instance fade works best with a landscape-colour texture projected by world position. |
| [Unreal forums: fading foliage when culling](https://forums.unrealengine.com/t/how-do-you-fade-foliage-out-when-culling-in-the-distance/14364) | Sinking vertices into the ground looked wrong on slopes → scale along the plant's own up axis instead. |
| [Unity discussions: grass rendered as chunks](https://discussions.unity.com/t/unity-terrain-grass-objects-is-being-rendered-as-chunks/1543542) | Dithered alpha-test fade, often blended toward terrain colour. Chunk-visible fades are a common failure. |
| [Unreal forums: distance grass darkness](https://forums.unrealengine.com/t/distance-grass-darkness/563348) | Games rarely draw grass beyond ~100–150 m; the terrain has to carry the look past that. |

Common pattern across all of them: **per-instance fade over a wide band + terrain coloured like the grass**,
with density thinning compensated by wider blades.

Phases in the order they were planned. Keep wind, foot bend and placement determinism unchanged throughout.

### Grass Phase 1: Per-plant distance fade (pop-in)
- [x] Vertex shader (Object.hlsl `VSMain`, after the world position): per placement,
      `s = 1 - smoothstep(start, end, distance(root, camera) + advance)`. `advance` is a per-placement hash
      (`GrassWindHash` of the root, 6 cm cells) × 0.35 × band. It only brings the fade forward, so every
      plant is gone by `end` and the edge never forms a ring.
- [x] The plant shrinks toward its root along its own up axis (correct on slopes): height × `s`, width ×
      `lerp(0.5, 1, s)`, collapsed to the root at `s = 0`. Wind bend height is scaled by `s` too.
- [x] New `ObjectGPUData::placementFade` (x start, y end), filled from new transient
      `Mesh::placementFadeStart/End` for both color and shadow draws, so shadows/depth match.
      Object buffer stride 1792 → 1808 (static_asserts and both shader structs updated).
- [x] GrassField: new `distanceFadeFraction` (default 0.3 of the radius). When > 0 the tile `visibility`
      range dither is off (the `appearance` fade for newly built tiles stays); 0 falls back to the old
      `distanceFadeWidth` tile fade. Distances are converted to world units with the terrain's scale.
- [x] Check: grass_test.scene capture shows no tile squares or ring at the edge.
- [ ] The edge still reads as a band at ~25–35 m; try a wider `distanceFadeFraction`.

### Grass Phase 2: Terrain matches the grass (coverage + distance)
- [x] `GrassLeafiness` moved out of the grass-only `#ifdef`; new `GrassPatchVariation` (the broad 11 m
      dry/lush pattern only) is shared with the terrain. The per-clump jitter stays grass-only because its
      25 cm cells would show as a checkerboard on the ground. Kept in Object.hlsl rather than a new include:
      GrassWind.hlsli is already included there unconditionally.
- [x] New transient `Terrain::GroundCover` (colour, strength, root shade, fade start/end, paint layer mask)
      → `ObjectGPUData::groundCoverColor/groundCoverRange` in `PrepareTerrainMaterial`.
      Object buffer stride 1808 → 1840.
- [x] Terrain shader (after layer blending, before the macro-colour fade), weighted by the cover layers'
      share of the paint weight:
  - inside the grass range: root colour (`color × rootShade`) so gaps read as undergrowth;
  - across and past the grass fade: the mean blade colour;
  - both with `GrassPatchVariation`; the layer texture survives as brightness detail (60% near, 30% far).
- [x] GrassField publishes the cover (`PublishGroundCover`, from `Start`, `Refresh` and
      `DescribeSpatialRegion` so cells outside the spatial region are tinted too). Colour = mean vertex
      colour of the short + tall grass meshes (LOD0), measured once per mesh pair from the shared CPU
      source cache. Only procedural fields with a `proceduralLayerMask` publish. New properties
      `groundTintStrength` (0.75 at first, 0.5 since the tint tuning) and `groundRootShade` (0.55), category
      "Grass | Ground".
- [x] Tint tuned on the landscape: the first pass made the meadow a flat saturated lime; now cover colour
      × 0.8 with 25 % desaturation, strength 0.5 and more texture detail kept (80 % near / 50 % far).
- [ ] Far tiles beyond the loaded cells use the macro colour palette (`LandscapeDriver` `farColors`,
      `mean[Grass] × .85`), not this tint. Match that palette's grass entry to the cover colour.
- [ ] Check: top-down and grazing captures near the grass edge; the edge should be hard to locate.
      Tune `groundTintStrength` / `groundRootShade` from captures. Related: the ground between blades in
      the foliage_test Meadow still reads paler than the grass (Foliage Phase 1d open item).

### Grass Phase 3: Density falloff with width compensation (coverage at mid range)
- [x] Shader-side thinning (Object.hlsl VSMain, `ObjectGPUData::placementThin`, `Mesh::placementThin*`):
      from a fifth of the radius to the fade start the share of plants kept falls to
      `GrassField::distanceThinning` (default 0.45; 1 = off). Each plant has a fixed random rank and
      shrinks away as the share passes it (no popping); survivors widen sideways by `1 / sqrt(share)`.
      Object buffer stride 1840 → 1856. Grass GPU time on grass_test.scene (DX12, raised pose):
      3.52 → 1.97 ms with the same plant count (collapsed plants skip rasterisation).
- [x] Thinning also in InstanceCull.hlsl: the thin range rides in the tile record's spare
      `reserved` uvec4 (`InstanceTileRecord`, filled from `placementThin` in SceneInstanceBatching);
      compute drops placements the vertex shader would shrink to nothing (same hash and share formula),
      so they cost no vertex work either.
- [ ] Re-evaluate the fixed LOD distances (7 / 13 m in LandscapeDriver). With smooth thinning, push LOD
      switches further out, or regenerate LOD1/LOD2 meshes with less aggressive sward removal.
- [ ] Consider Tsushima-style view-space widening of blades seen edge-on (GodotGrass reference).
- [ ] Use the saved cost to push the grass radius out, or hold it steady under the detail budget.

### Grass Phase 4: Lighting as one surface
- [x] Pelt shading in `PSGrassMain` (Object.hlsl): normals lean toward the sky so a field lights as one soft
      surface, wrapped diffuse, root occlusion along the blade (uv.y), transmission when the sun is behind
      the blades, and a Kajiya-Kay sheen along each blade.
- [ ] The plan had the lean grow with distance (≈0.3 near → 0.8 far); not checked whether the shader does
      that or whether the far field needs it.
- [ ] Sky (SH) ambient instead of the flat `ambientUnlit` for grass and meadow plants (Foliage Phase 1 open
      item); canopy shade already scales the ambient (Foliage Phase 3).

### Grass Phase 5: Build tiles ahead
- [ ] Create and build tiles in a ring beyond the radius (`retentionRadius` exists) so they are resident
      before they reach the fade band.
- [ ] Prioritise tiles in the direction of travel (dot of tile offset with viewer velocity) over pure distance.
- [ ] Consider damping how fast `DetailRadiusScale` may move the grass radius, or excluding the fade band
      from it, so the edge does not slide visibly.

### Grass Phase 6: Grass texture past the radius (flat distance)
- [x] **6a:** terrain shader (ground-cover block, weighted by `beyond`): two octaves of fine
      brightness grain (1.7 and 6.3 /m, each faded as it nears the pixel footprint so it cannot
      flicker) and a grazing-angle sheen, so open ground past the blades reads as short grass.
- [x] **6b → far grass ring** (geometry instead of cards; no alpha textures needed):
      `Procedural far grass` layer per terrain cell (`MeadowGrass::ApplyFar`, LandscapeDriver,
      landscape_demo layer list, test scene). A second GrassField with `innerRadius`: 16 m tiles of
      `far_grass_tuft` (12 wide two-segment blades, 36 tris, same mesh for every LOD) at 1.2/m² out to
      90 m. Tiles wholly inside `innerRadius` are skipped; it fades in by running the thinning backwards
      (`placementThin` start > end, minimum 0, `w = 1` keeps width). Its fade-in must finish before the
      detailed grass starts fading (inner = 0.5 R, width 0.2 R), or a band of bare ground shows between
      the two. It does not publish the ground tint. Placement caps scale with tile area.
      First test (fades not yet overlapped): far-band terrain 86 % → 59 %, eye-level overall 22 % →
      17 %; grass GPU 2.77 → 3.27 ms, 5.9k → 13k plants, 100 → 106 tiles.
- [ ] Shell texturing is likely too expensive on the current low-spec target; revisit only if 6a/6b fall short.

### Grass Phase 6½: Antialiasing (grass edges stair-step and shimmer)
At the review the live renderer had no antialiasing (MSAA only in the DX11 video-export renderer), and grass
blades are 1–2 px wide at a few metres, so their edges aliased and shimmered under wind. Now: FXAA, MSAA
(DX11, DX12) and TAA (DX11, DX12; on by default). `SceneSettings::temporalUpscaling` is still serialized but
unimplemented (Foliage Phase 2, optional headroom).
- [x] **FXAA** inside the compose pass (`PostProcess.hlsl` `ApplyFxaa`): console-style edge blend plus
      a sub-pixel pass, on HDR before bloom/exposure/tone mapping, with edges found on exposed,
      compressed luma. No new targets or passes, so DX11, DX12 and Vulkan all get it; settings travel in
      compose `params0.w` (negative = FXAA), which only video accumulation used before.
- [x] **Grass minimum width** (`Object.hlsl` VSMain, grass shader only): blades narrower than N pixels
      widen across their UV u axis; centre-line vertices (midribs, tips) stay. `placementFade.zw` carry
      N and the world size of a pixel per unit depth (`2 / (proj[1][1] · viewport height)`).
- [x] **Project settings** `ProjectSettings::renderingEffects` (`Model/RenderingEffectsSettings.h`),
      saved as `<RenderingEffects .../>` in the .proj, edited in Preferences > **Effects** (presets
      Performance / Balanced / Quality; dependent controls grey out). Applied through the process-wide
      `Rendering::RenderingEffects` policy (editor load, Preferences changes, game start). Also project
      switches/scales over scene look: bloom (+scale), auto exposure, exposure bias (EV), colour grading
      (+strength toward neutral), ambient occlusion.
- [x] Checked on grass_test.scene (DX11): stair steps and single-pixel sparkles gone; distant blades read
      as solid strokes. Image slightly softer at the default 0.75 subpixel.
- [x] **MSAA** (`AntiAliasingSettings::msaaSamples` 1/2/4/8, Preferences > Effects > MSAA; default off).
      Renderers lower an unsupported count to the nearest supported one.
  - **DX11:** game renderer follows the setting live (`ApplyLiveSampleCount`, video export keeps its
    own count); editor views (`D3D11View`) follow it too (they were fixed at 4x before, so they now
    default to 1x + FXAA). Multisampled depth is now readable: typeless + `Texture2DMS` SRV, and the
    depth copy has a `Texture2DMS` variant, so AO and the depth-aware sky/haze keep working.
  - **DX12:** `D3D12PipelineState` keeps its description and builds a variant per sample count on
    first use (`ForSamples`); `D3D12GraphicsContext::SetPipeline` picks the bound scene target's count
    (`DX12PostProcess::BoundSceneSamples`, set by BindScene/RebindScene, 1 after Compose; always 1 in
    depth-only passes). `DX12PostProcess` renders into a multisampled target and resolves at compose.
    Depth is multisampled; `CopySceneDepth` copies sample 0 with its own compute root signature/PSO
    (`CopyTextureRegion` cannot read MSAA depth), then restores the shared texture descriptor heap.
    Game renderer flushes and recreates on a change; editor views (`D3D12View`) retire old targets
    for 4 renders instead (frames in flight).
  - [ ] **Vulkan:** not yet (renders without MSAA).
  - Checked on grass_test.scene: DX11 and DX12 4x give clean blade edges; AO runs under MSAA on both;
    no DX12 device errors. With auto exposure on, DX12 MSAA captures at 120 warm-up frames looked
    darker: a start-up adaptation transient (converges to the same level by 600 frames; identical
    with `ENGINE_AUTO_EXPOSURE=0`).
  - [ ] Not yet exercised: live MSAA switching in the editor's DX12/DX11 views (captures use the game
    renderer).
  - Diagnostics for captures: `ENGINE_MSAA`, `ENGINE_FXAA`, `ENGINE_GRASS_MIN_WIDTH`,
    `ENGINE_AUTO_EXPOSURE` override the project settings in the game/capture tool.
- [x] **TAA** (`AntiAliasingSettings::taa`, `taaFeedback` 0.75–0.97, `taaSharpen` 0–1; Preferences >
      Effects; on by default since Foliage Phase 1b, with the Balanced preset (Performance: off, Quality:
      MSAA 4x); mutually exclusive with MSAA in the UI, MSAA wins if both are set).
  - **Jitter:** SceneRendering offsets the main view's projection by a Halton(2,3) 8-sample sub-pixel
    sequence, per camera (`SceneRenderer::m_temporalViews`), only when the backend reports
    `IGraphicsContextFactory::SupportsTemporalAntiAliasing()` (DX11, DX12), single-sample, perspective.
  - **Reprojection:** one matrix, previous unjittered view-projection × inverse(current), in double,
    carried in `PostProcessSettings` (so it also travels in the game's render packets). History is
    invalid after a resize or on a camera's first frame.
  - **Resolve** (`PostProcess.hlsl` `TemporalResolvePS`, before bloom/exposure): reprojects the
    nearest depth in the 3×3 neighbourhood, clips history to the neighbourhood's YCoCg spread (no
    per-object motion vectors, so this is what stops animated grass ghosting), blends on
    luminance-compressed colour, and lowers the history weight with motion. Result goes to one of two
    history targets and is copied into the scene colour, so bloom/exposure/compose are unchanged.
  - **Sharpening:** compose applies a 4-tap unsharp mask after TAA (`temporal.z`).
  - `PostProcessConstants` grew 64 → 80 bytes (`temporal` float4); all backends size it with
    `sizeof` (Vulkan push constants stay well under the 128-byte minimum).
  - DX11: history in `DX11PostProcess`; game renderer and editor views (`D3D11View`, depth now
    typeless + SRV) hand it their depth. DX12: two history targets/RTVs and two descriptor tables
    (scene, history, depth) in `DX12PostProcess`; depth goes DEPTH_WRITE → PIXEL_SHADER_RESOURCE
    around the pass.
  - Checked on grass_test.scene: static camera converges to clean edges on DX11 and DX12 (DX11
    high-frequency energy 14.4 → 8.3); a turning camera (`--look-yaw 6`) shows no ghost copies or
    trails, sky and clouds crisp, grass softer while moving (sharpening helps). Cost on the iGPU:
    ~+1 ms DX11, ~+0.05 ms DX12 at 960×540. No DX12 device errors.
  - [ ] Vulkan: not supported yet (no jitter is applied there).
  - Override for captures: `ENGINE_TAA=0|1`.

### Coverage test (render mode)
- [x] `SceneRenderMode::Coverage` (6): terrain red, everything drawn on it green (reuses the Performance
      heat colours via `morphParams.z`). Editor: Scene view render-mode list > Coverage. Captures:
      `GameBackendCapture --render-mode 6`. Measure with a red/green pixel count (terrain share of the
      ground pixels, split into near/mid/far screen bands).

### Grass Phase 8: Larger, thicker foliage
- [x] `tools/generate_meadow_grass.py`: `meadow_bush` (~1.2 m rounded broadleaf), `meadow_bush_tall`
      (~1.7 m upright), `tall_grass_clump` (~1.1 m dense grass with seed stalks). Bushes have a solid
      **crown core** (closed lumpy ellipsoid per lobe, dark foliage colour) under the leaf cards, so they
      block the view at every LOD; `meadow_shrub` gets the same core.
- [x] `GrassField` **Bushes** category: `bushMeshes`, `bushesPerSquareMeter`, `bushClusterSize`,
      `bushCoverage`; clusters from the generalised thicket noise plus a few singles; grass thins under
      clusters. Meadow preset / LandscapeDriver / test scene: bushes 0.06/m², clusters 16 m, coverage
      0.25; `tall_grass_clump` added to the shrub-thicket species.
- [x] Thicker base cover: `meadow_tussock` sward 84 → 150 blades, 2.0 → 2.6 cm wide, 0.22 → 0.26 m;
      LandscapeDriver/test scene: tussocks 2.6 → 3.5/m², bushes 0.12/m², bush coverage 0.35.
- Coverage results on grass_test.scene (DX12 960×540, terrain share of ground pixels, near / mid / far
  screen bands; far is mostly open terrain past the grass radius):

  | Pose | Before (grass only) | + bushes | + thicker cover | Grass GPU |
  |---|---|---|---|---|
  | Raised (4 m up) | 42 / 71 / – % | 47 / 34 / 90 % | **23 / 16 / 87 %** | 2.4 → 3.4 ms |
  | Eye level | 36 / 34 / – % | 53 / 25 / 47 % | **21 / 25 / –** % (22 % overall) | 2.6 → 3.7 ms |

  Near and mid gaps roughly halved. Remaining terrain is (1) past the ~36 m grass radius (Phase 6
  impostors / longer range) and (2) small gaps between tussocks up close.
- [x] Bushes rebuilt as **grown plants** (`grow_bush`): 6–7 main stems → branches → twigs (four
      levels, tapered round tubes, curving outward from the trunk axis and up, each growing a share of
      the way to an ellipsoid envelope so twigs reach it), clusters of 8 leaves (6.5–8 cm on short
      stalks) at twig tips and along twigs, and a small dark core fitted to the foliage (35 % of its
      spread at LOD0, 80 % at LOD2) as the shadowed interior. One layout for every LOD; coarse LODs
      drop fine wood and keep fewer, larger leaves. meadow_bush 8780/4292/1898 tris,
      meadow_bush_tall 7544/3730/1794. (A first version centred the core on the envelope and aimed
      child branches outward from the centre, so low branches pointed down: the crown stayed empty
      and the core read as a smooth egg.)
- [x] Shrub/bush leaves (`generate_meadow_grass.py`): `leaf_card` is now an ovate leaf per detail
      level (10 / 6 / 2 triangles; raised midrib, back-curl) instead of a 4-vertex diamond; `grow_bush`
      leaves get a random roll (they lay in parallel tiers), ±25 % value jitter and occasional yellowing;
      coarse LODs keep more, smaller leaves (`LEAF_KEEP` 1/.6/.35, `LEAF_GROW` 1/1.25/1.6, was 1/.46/.2 and
      1/1.45/2.15: 26 cm diamonds at LOD2); no crown core at LOD0 (it read as an egg), smaller at LOD1/2.
      meadow_bush 8780/4292/1898 → 32844/11954/2718 tris, meadow_bush_tall 7544/3730/1794 → 28152/10562/2438.
- [ ] Bushes share the grass radius (~36 m); larger bushes should reach further (their own range or a
      props/foliage layer).
- Coverage now (grass_test.scene, DX12 960×540; terrain share of ground pixels, overall | near /
  mid / far): raised pose **26.5 %** | 23 / 15 / 44 %, eye level **14.8 %** | 22 / 8 / 13 %
  (started at ~42–57 %). Grass GPU 3.6–3.8 ms with 17k plants (detailed grass, bushes, far ring).

### Grass Phase 7: Mesh/coverage tuning (optional)
- [x] 2026-11-19: blade meshes reworked (`tools/generate_meadow_grass.py`): leaf outline (narrow sheath,
      widest ~20% up, parallel margins, long tapered tip) instead of a root-to-tip triangle; V-folded
      cross-section with a sunken, paler midrib (3 columns at LOD0/1); 5/4/3 segments (was 4/3/2);
      ±12% brightness per blade; seed stalks are 3-sided tubes at LOD0/1. Triangles: meadow_tussock
      798/340/88 → 1824/709/126, field_tussock 580/290/101 → 1390/671/149.
- [ ] Clumping (Voronoi cells with shared height/colour/facing) to make the field read as real grass while
      gaps look natural.

### Grass notes (testing)

- **Testing pitfall:** `GameBackendCapture` is `EXCLUDE_FROM_ALL`, so `cmake --build --preset
  devoptimized` does not rebuild it. Build it with `--target GameBackendCapture` after engine changes.
  The first captures of this work ran a 2026-11-18 binary (1792-byte object records) against the new
  shaders (1840), which garbled every object (sky only, giant stretched triangles) and froze the PC.
  After rebuilding it, lighting_showcase renders correctly on DX11 with these changes.
- The DX12 access violation on landscape_demo (also seen on the stashed baseline) was with that stale
  binary too; re-test before treating it as a separate bug. The machine was on the Intel UHD iGPU,
  where DX11 needs ~5 min to start landscape_demo. Not seen in the 2026-11-19 DX12 landscape runs on the
  RTX 4090.
- The `debug` preset fails on `bc7enc` (`/O2` with `/RTC1`); use `devoptimized`.
- [ ] Not re-tested: `--camera-position 0,-38,40` landed under the terrain (capture overrides
  skip the flight controller's ground clearance), and a pose over the lake near the shore
  (220,-38,680 → 190,-39,600) froze the PC during warm-up, with no crash dump. Cause unknown; the same
  build completed the start pose and drew 5474 grass instances from under the terrain. That was DX11 on
  the Intel UHD iGPU; use the test scenes (below, and foliage_test) for grass checks.
- **Test scene:** `tools/build_grass_test_scene.py [--cell X Z]` writes
  `Engine/Core/Assets/Scenes/terrain/grass_test.scene`: one landscape cell (heights from
  world.heights, painted with the meadow layer) with the demo's procedural GrassField settings, plus
  the demo's sun, atmosphere and explorer. Loads in ~2 s on the iGPU. Capture:
  `GameBackendCapture --backend dx11 --scene Engine/Core/Assets/Scenes/terrain/grass_test.scene
  --width 960 --height 540 --warmup 120 --frames 5 --camera-position 64,-28.1,6
  --camera-target 64,-29.5,120 --out .local/grass/test`.
  First capture: no tile squares at the range edge (Phase 1 OK); ground past the grass is olive with
  soft patches matching the blades (Phase 2 OK). The edge still reads as a band at ~25–35 m; try a
  wider `distanceFadeFraction`. Gaps between near tussocks still show ground (Phases 3 and 7).

---

## Landscape look (large-scale vista with layered haze)

Where a target here differs from "Landscape demo expectations" at the top, that section wins.

Every landscape feature below must be usable from the editor, not only from
Python scripts or hand-edited scene JSON: settings live in SceneSettings or a
component, show in the Properties view with tooltips, serialize with the scene,
and update live. Offline generators (erosion, asset generators) get an editor
entry point (button/tool window) that runs them and reloads the result.

Order is by dependency and visual payoff, not by section number in older notes.

### 0. Baseline + test shot
- [x] Fixed-pose capture: `GameBackendCapture --camera-position x,y,z --camera-target x,y,z` (recorded in the session header)
- [x] GPU `Sky` timing stage (`gpu_sky_ms` in captures and the editor frame profiler)
- [x] `ENGINE_ATMOSPHERE_FOG=0|1` env override for fog A/B captures
- [x] Rough vista pose (peak over a valley, see command below)
- [x] Vista pose is a scene viewpoint: inactive Camera object "Vista" in landscape_demo.scene (emitted by
  `tools/build_landscape_demo.py`, `VISTA_EYE`/`VISTA_TARGET`). Scene View toolbar "Viewpoints" menu jumps the
  editor camera to any scene camera and sets the selected camera from the current view
  (`Camera::GetWorldPose/SetWorldPose`); captures take `--viewpoint Vista` (recorded as `viewpoint` in the header)
- [x] Vertical dashed line in the sky: the equirect panorama's u wrap (atan2 jump at world -X) gave implicit
  derivatives a full-texture step, so 2x2 quads straddling it sampled the smallest mip. Skybox.hlsl now uses
  `SampleGrad` with wrap-aware u derivatives (checked on DX12, DX11, Vulkan)
- [x] Black sky after the 2026-11-17 architecture merge: the D3D `SkyboxCB` kept `float4x4 invVP` ahead of the new
  ray-basis fields while C++ `SkyboxConstants` dropped it, so every sky/fog/sun constant read 64 bytes off. Removed
- [x] DX11 vista differed from DX12/Vulkan (weaker sun glow, sparser far forest): not a rendering difference.
  DX11 captured the composed back buffer, DX12/Vulkan the HDR scene color before post-processing with a plain
  2.2 gamma, so their captures had no bloom, tone mapping or grading. DX12 now copies the back buffer after
  compose, Vulkan the swap-chain image after composition (swap chain gets `TRANSFER_SRC` usage; BGRA swizzled).
  Second cause: benchmark runs passed the host's real input to the game, so a capture window that ended up in
  the foreground let the user's mouse turn the camera during warmup (runs of one viewpoint diverged; Vulkan was
  off by ~40 degrees once). Benchmarks now get an unfocused, key-less snapshot; only synthetic input drives them.
  Vista now: DX12 == DX11 pixel for pixel, Vulkan differs on 0.22% of pixels, all three at the exact viewpoint.
  Captures taken before 2026-11-18 on DX12/Vulkan show pre-post-processing images
- [x] Vista pose re-picked for the 24 km map: eye (2304, 463, -3072) on a 410 m foothill at the valley's south-east
  rim, target (283, 254, -865): across the valley and a lake at the backlit north-west ranges, dark canopy in the
  foreground. Chosen from candidates found by searching the far grid for rim peaks with a deep drop toward the
  sun's azimuth and high ranges behind (A2 at (2688, 972, -4352) is a wider, flatter alternative). Set in
  `VISTA_EYE`/`VISTA_TARGET` and the scene's "Vista" camera. Numbers above this line used the old pose
- [x] Depth precision: forward Z with D32F and near 0.06 gave ~25 m depth steps at 5 km and ~100 m at 10 km. Every
  depth buffer (camera, portal views, shadow atlases) is now reverse-Z; `Graphics/DepthConvention.h` holds the
  convention (clear 0, `CompareNearer` = GREATER as the builder default, `CompareNearerOrEqual`). Projections are
  built with near/far swapped in double (`Camera::GetRenderProjectionMatrix`, `BuildStableShadowCascade`);
  `Camera::GetProjectionMatrix` stays forward-Z for editor picking/overlay math. Shader sites: shadow compares
  (`receiver + bias >= stored`), skybox/grid far-plane triangles and grid plane depth, fluid front depth, parallax
  `SV_DepthLessEqual`, portal aperture near clamp (`z = min(z, w)`) and depth-reset far plane (`z = 0`),
  sky ray unprojection, `ProjectedTextureFootprint` near test. The software occlusion buffer and GPU cull work in
  clip w, and frustum tests are symmetric in z, so neither changed. Verified: DX12 vista vs HEAD 0.10% of pixels
  differ (noise floor 0.01%, all in distant canopy), Vulkan matches, DX11 unchanged apart from its known sky
  difference; portal_traversal_demo, room_larger_inside (DX12/DX11/Vulkan), particle_fluid_river and
  surface_detail_materials match HEAD up to animation/streaming timing; `.local/tests` pass. Vista frame time
  unchanged (A/B on the same machine: HEAD 26.3/27.0 ms, reverse-Z 25.9/26.5 ms). Not checked: editor grid
  (editor-only pass, math changed in `Grid.hlsl`)
- [x] Vista "regressed" from ~10 ms (2026-11-16) to ~26.5 ms at `ec249b3d`. Two causes, neither a code regression:
  - Unfocused captures were throttled. Since background launches (`ec249b3d`), the capture window never has focus
    and Windows 11 power throttling (EcoQoS) ran every CPU stage 2-3x slower on this hybrid CPU. The old 5.2 km
    world at the old pose, current code: 25.2 ms throttled, 10.4 ms with `Engine::Core::DisablePowerThrottling()`
    (`Startup/LaunchActivation`), i.e. the 11-16 number. GameBackendCapture always opts out; Game/Editor opt out
    when launched in the background. Captures from before 11-17 18:00 ran focused; later ones before this fix were
    throttled, so neither set compares with new numbers
  - The 24 km world is heavier at the same pose: 16k far-object cells (radius 64 x 128 m) instead of ~1.6k
  - [x] `TerrainManager::UpdateFarObjects` rescanned the whole far window each frame (map lookups per cell, a sort
    of every uncomputed cell) and rebuilt a block after every 8-cell batch: 4.4 ms/frame plus constant prefab
    re-preparation. Now incremental: the nearest-block-first queue is rebuilt only when the center cell changes,
    only loaded/latched cells are checked for near coverage, and a block rebuilds once its data is complete
    (coverage/radius changes still apply at once). 2-6 far jobs instead of 2. 4.4 -> 0.02 ms; drive-forward
    A/B: median 29.0 -> 21.4 ms, far time over the drive 7.7 s -> 0.46 s
  - [x] Far blocks re-hashed every placement once per second per block (`GetPlacementSignature`) to catch
    in-place edits they never get: `TerrainObjectLayer::placementsChangeOnlyWhenMarked`. Prefabs 3.0 -> 1.4 ms
  - [x] Texture footprints: shadow cascades are orthographic, so each instance chunk's world box with its
    largest scale bounds all its shadow demand in one projection; chunks 4x their diagonal away also use the box
    for the camera. Per-instance work is now camera-only for near chunks. 5.2 -> 2.5-2.9 ms, image identical
  - Result (DX12, 1920x1080, settled after 5000 warmup frames): 24.8 -> 16.8-20.7 ms. CPU path now update 1.7 +
    prepare 5.0 + record 10.5 ms; the GPU varies 11-20 ms between runs (other apps share the GPU)
- [ ] Far blocks are outside the detail budget. With all far blocks loaded the vista was GPU-bound on far forest
  (foliage depth 6-11 ms + leaves 3.5-7 ms at detail_scale 0.82-0.94). 2026-11-19: after the depth-pyramid
  occlusion (Foliage Phase 2) the vista's GPU opaque stage is ~8 ms and the vista is CPU-bound, so this is no
  longer the blocker; letting far-block density follow the budget is still open
- [x] Benchmarks measured while far objects still streamed (600 warmup frames was ~half of the 16k cells).
  `--wait-for-residency` now also waits for `TerrainManager::GetFarObjectStatus().settled` (window computed, no
  jobs or dirty blocks, every block's instances prepared, latched per block since its last rebuild); frame logs
  carry `far_object_cells`, `far_object_cells_queued`, `far_object_blocks`, `far_object_blocks_preparing`,
  `far_objects_settled`. The timeout message prints which condition is missing. Vista settles after ~109 s.
  - [x] Found on the way: the prefab source stat cache (`PreparePrefabInstances`) dropped each entry when it went
    1 s stale, so a layer visited less than once a second (round-robin under the 3 ms budget, ~300 layers) found its
    sources pending on every visit; a far block whose prefabs no other layer kept fresh never built. Stale entries
    now keep serving until the refresh lands
  - [x] Fully settled vista measured 40.1 ms median / 47.5 p95 (DX12, one run), far above the 16.8-20.7 ms
    recorded with half the far cells streamed. Stage breakdown (Foliage Phase 0, 42.8 ms): CPU-bound, update 5.1 +
    prepare 10.8 + record 25.4 back to back, GPU ~13.4. After the foliage CPU rounds and the GPU occlusion
    pyramid: ~25.7 ms (2026-11-19)
- [ ] Cell-crossing spikes while moving (old and new far code alike): ~35 ms far-block membership rebuilds plus
  ~22 ms near-cell streaming on the same frames; spread block rebuilds over frames. 2026-11-19: part of this was
  `Scene::MoveObject` invalidating the whole scene for each parented object (fixed); walking still shows
  80-120 ms clusters at cell crossings (Foliage Phase 2, "Moving camera, remaining hitches")
- [ ] Camera-relative precision check at 16 km from origin (transforms are double, check vertex/shader paths)

Vista capture (RTX 4090, DX12, 1920x1080, RelWithDebInfo, uncapped):

```
ENGINE_ATMOSPHERE_FOG=0|1 build/RelWithDebInfo/GameBackendCapture.exe --backend dx12 \
  --scene Engine/Core/Assets/Scenes/terrain/landscape_demo.scene --width 1920 --height 1080 \
  --warmup 600 --frames 300 --max-fps 0 --require-terrain --wait-for-residency 240 \
  --viewpoint Vista --out .local/vista/<name>
```

(`--viewpoint Vista` was `--camera-position 1272,123,-12 --camera-target -893,28,1238` until 2026-11-18; it is now
`--camera-position 2304,463,-3072 --camera-target 283,254,-865`, so the tables below are from the old pose.)

Baseline 2026-11-16 (medians, ms):

| metric | fog off | fog on |
|---|---|---|
| frame interval | 20.00 | 19.87 |
| simulation update | 3.17 | 3.12 |
| scene prepare | 5.08 | 4.75 |
| command record (camera 8.5, shadow 1.9) | 11.52 | 11.55 |
| GPU total of stages | 5.93 | 5.89 |
| GPU opaque (other 1.6, leaves 1.5, foliage depth 1.8, bark 0.2) | 5.28 | 5.22 |
| GPU terrain / shadow / sky | 0.53 / 0.10 / 0.02 | 0.55 / 0.10 / 0.02 |

Finding: the vista is CPU-bound at ~50 fps. GPU work is ~6 ms; update + prepare + record (~20 ms) add up to the frame
interval, so they appear to run serially. 100 fps needs the CPU critical path under 10 ms: camera command recording
(8.5 ms) is the largest item, then scene prepare. Height fog costs nothing measurable.

- [ ] Profile and cut CPU frame cost for the vista (camera command recording, scene prepare)
  - Why serial: `SimulationLoop` (Game/Application/GameApplication.cpp) runs Update, then PrepareRenderFrame, then
    Render (shadow + camera recording) on one scene-owner thread while the window thread waits; only replay is on the
    render thread. Overlapping update N+1 with recording N needs a scene snapshot boundary; bigger change, do after the
    per-stage cuts below.
  - [x] `SceneTextureFootprints::Request` projected every instance into the camera and all shadow cascades on one
    thread (3.8 ms). Now split into 512-instance chunks on RenderJobs: 1.0 ms. New capture fields
    `render_material_draw_loop_ms`, `render_material_footprint_ms`, `render_material_record_upload_ms`.
  - [x] Frame item preparation did two `dynamic_cast` component scans per object per frame (~3.8k objects); terrain
    components now come from `FrameComponentLookup`: `prepare_items_ms` 4.3 -> 3.3.
  - [x] Sampling profiler without admin rights: `GameBackendCapture --cpu-sample` samples every thread during the
    measured frames and writes `<out>/cpu-profile.txt` (per-thread inclusive/exclusive + caller chains;
    `.local/tests/CpuSampler.h`). Adds sampling overhead: use it to find hotspots, not for timings.
  - [x] Biggest finding: no shader cache. Object.hlsl entry points take ~21–23 s each in fxc (Water.hlsl PSMain 25 s),
    so every launch spent 60–100 s compiling and the compiles were still running on the job workers during
    measurement, starving the frame's parallel work. Added a persistent DX11/DX12 bytecode cache
    (`Renderers/D3DShaderCache`, key = preprocessed source incl. all includes + entry/target/flags/compiler version;
    `%TEMP%/EngineShaderCache`, `ENGINE_SHADER_CACHE_DIR`, `ENGINE_SHADER_CACHE=0`). Warm startup 65.6 s -> 1.8 s.
  - [x] Particle rendering scanned every object with `GetComponent<ParticleEmitter>` four times per frame (~15% of
    the simulation thread) in a scene without emitters; now uses the component registry.
  - [x] Remaining per-draw `GetComponent<Terrain/TerrainBatchAppearance>` in Render/PrepareTerrainMaterial use the
    frame item's `terrainSurface` flag.
  - [x] Standalone game publishes scene snapshots without per-object contents (only the editor reads them):
    simulation update 3.6 -> 2.3 ms.
  - [ ] Shader compile time itself: Object.hlsl ~21 s per entry in fxc (the fog include added ~2 s). Look at what
    fxc unrolls/flattens (lighting loops, terrain layers); cold-cache startup is still ~65 s
  - [x] `Material::Validate` (0.75 ms, cache misses over ~40 fields of each large per-object Material) now runs as a
    parallel pre-pass over the frame lookups before the serial item loop: 0.15 ms. Prefab LOD materials still
    validate inline. Shader prewarm dedupe no longer builds a `tuple<string,string,bool>` per item (transparent set).
  - [x] Texture footprints re-project each 512-instance chunk every 4 frames (staggered, per view; last result is
    re-submitted in between) via `Rendering/TextureFootprintCache` owned by the Scene: 2.55 -> 1.5 ms, same
    residency. Remaining: the parallel join (~60%), serial gather, `RequestGraphicsFootprint` mutex per texture slot
  - [x] `TerrainLayerArrays::Acquire` built a string key (paths + formatted tints) per terrain draw per frame; now
    an identity fast path (texture pointers + tints, weak refs guard address reuse). `PrepareTerrainMaterial`
    looked up the parent TerrainManager twice. Material draw loop 1.3 -> 0.9 ms
  - [x] GrassField visibility loop: per-frame `std::set` of desired tiles replaced by a refresh serial, tile Material
    cached (was a component scan per tile): grass 0.94 -> 0.42 ms with ~1000 active tiles. Captures now carry
    `grass_*_ms` phases
  - [ ] GrassField::LoadSources (0.18 ms) re-derives species LOD paths from string maps every frame, including a
    by-value copy of `ObjectLodProvider::GetObjectLodMeshes()`; needs revisions on its inputs to memoize
  - [x] Hotspots listed here on 2026-11-16 (item preparation, TerrainObjectLayer::PreparePrefabInstances,
    Material::PrepareTextures, BuildOcclusionSignature, TransformHierarchy::Prepare, TerrainManager::
    UpdateFarObjects, heap allocation, `_NLG_Return2`) have each been cut: texture preparation and far objects
    by the items in this section, the rest in the foliage CPU rounds (Foliage Phase 2). The current list is
    "Next CPU steps" there

Looking around (`GameBackendCapture --look-yaw <pixels/frame>` feeds synthetic mouse-look through the normal input
path; pin detail with `ENGINE_DETAIL_SCALE=2.5` for comparable runs):

- [x] Turning stuttered: `PersistentInstanceBatches` keys include bound texture and mesh-buffer pointers, which
  texture streaming replaces constantly while the view turns, so every new key created four D3D12 buffers (16% of
  the simulation thread, upload p95 8.7 ms). Batches unused for 8 frames now park their buffers per frame slot and
  new keys reuse them. Look (adaptive detail): median 15.2 -> 13.9 ms, p95 24.8 -> 19.2, max 38 -> 26
- [x] Software occlusion buffer (rebuilt every frame while turning) now starts in the camera's shadow pass, which
  runs first and computes the same view; the color pass adopts it when camera, frame serial, viewport and
  view-projection match bit for bit (`EarlyCameraOcclusion`, joined by every color pass, frame preparation and
  `FenceBackgroundWork`; `ENGINE_EARLY_CAMERA_OCCLUSION=0` disables). Interleaved A/B while turning: occlusion
  wait 0.78 -> 0, command record 7.0 -> 6.5 ms, frame 13.1 -> 12.6 ms
- [x] `Material::PrepareTextures` residency check moved into the parallel validation pre-pass
  (`NeedsTexturePreparation`, read-only); serial texture preparation 0.37 -> 0 ms, items 2.33 -> 2.11 ms
- [x] Shader prewarm skips materials whose shader, asset path and terrain use are unchanged since their last
  prewarm under the same epoch (`ShaderSourceCache::Generation()` + `m_materialShaderBytecodeEpoch`); the check
  runs in the parallel pre-pass. Prewarm 0.29 -> 0 ms, items 2.0 -> 1.5 ms, prepare 2.9 -> 2.46 ms
- [x] Tried: texture pointers out of the instance batch key (per-frame occurrence ordinal instead). No
  measurable change (upload 0.40 vs 0.38 ms while turning): the remaining upload is visible tile membership
  changing with the view, not texture churn. Reverted
- [ ] Shadow record +0.2 ms from sharing workers with the early occlusion job

Vista 2026-11-16 end of day (adaptive detail, median/p95/p99 ms): static 10.0/12.5/14.6 (was 12.0/14.8/19.1);
fast turn `--look-yaw 20` 10.3/14.2/16.8 (was 15.2/24.8/30.3); slow turn `--look-yaw 6` 10.9/14.7/17.7
(was 12.0/17.8/20.6). Pinned detail 2.5 static: 10.2-10.35. Single runs vary by up to ~1 ms on this machine:
compare stage timers or interleaved A/B pairs, not single frame medians
- [x] Detail budget oscillated: the predictive mode grew its cost budget whenever the GPU was under budget, but the
  GPU measures the *applied* scale, which glides far behind the target, so the budget wound up (0.49M -> 1.37M,
  target at the 8x maximum) and geometry overshot to 10-14 ms. Now the budget comes straight from a calibration
  (smoothed GPU ms / smoothed predicted cost of the applied scale), cuts in 0.2 s and grows over the response time;
  the controller follows a lower target in 0.1 s; and when *measured* GPU time is >1.1x the budget, detail is shed
  up to 5x faster than the anti-popping glide. `.local/tests/DetailBudgetTests.cpp` runs the closed loop in
  simulated time (static, fast view changes, and a model whose predictions run opposite to real cost).
  Captures (adaptive): static 12.0/14.8/19.1 -> 10.6/12.5/13.9 ms (median/p95/p99), geometry max 10.2 -> 7.0;
  fast turn 13.9/19.2/23.0 -> 12.6/16.5/19.1, geometry max 14.2 -> 7.4
- [ ] The cost model's per-view predictions do not track GPU cost while turning: views it predicts as densest
  (~300k) measure cheapest (~2.4 ms) and vice versa (likely occlusion the model's coarse grid misses). The budget
  calibration swings 150k..350k with the view. Compare predicted vs measured per view direction
- [ ] Slow turn (`--look-yaw 6`) p95 ~17 ms: frames facing dense content are CPU-bound (camera record ~7 ms, prepare
  ~5 ms, CPU render ~8.8 ms vs the 4 ms CPU budget, which model mode ignores). Either let the CPU render budget
  limit the model target or take the serial update/prepare/record pipeline apart (see "Why serial" above)
- [ ] One 1.77 s hitch in one slow-turn capture (frame 299: `prepare_assets_ms` 8.5, 5 prefab layers started
  streaming; no recorded stage accounts for the time). Not reproduced in three more runs; watch for it
  - [ ] Detail model: between the round-3 and round-4 captures (other edits to shadows/terrain object layers/
    Object.hlsl landed) the vista's `detail_scale` rose 2.03 -> 2.48 and footprints 0.95 -> 2.55 ms. Check whether
    the detail budget should count the CPU footprint pass, since it scales with placements

Vista after CPU work (warm shader cache, same command, two runs): frame 12.6 / 12.8 ms (~79 fps, was 20.0);
update 2.3, prepare 4.3, record 5.7 (camera 4.0, shadow 1.4); GPU ~5.8 ms.

Vista 2026-11-16 round 5 (`--frames 900`; detail_scale 2.48 vs 2.03 above, so not directly comparable):
before validation/footprint changes 13.2 ms; after 11.8 ms (~85 fps), p95 14.1. Update 2.2, prepare 3.3,
record 5.9 (camera 4.5, shadow 1.1); GPU opaque 5.4.

### 1. Aerial perspective + height fog
- [x] Per-pixel analytic height fog in the forward shaders via one shared include (`Shaders/AtmosphereFog.hlsli`); no per-material fog code
- [x] Extinction/inscatter blend: `color * T + inscatter * (1 - T)` (Beer–Lambert), exponential height falloff so haze pools in valleys
- [x] Henyey-Greenstein sun-glow term for in-scatter toward the sun, using the scene's main directional light
- [x] Sky horizon haze from the same parameters (skybox shader)
- [x] Scene settings + editor UI ("Atmosphere" in scene Properties), serialized with the scene
- [x] Shadowed in-scatter (light shafts): `ObjectFogSunVisibility` (Object.hlsl) marches the cascade shadow map
  along the view ray in quadratically spaced segments (finer near the camera); each segment's analytic fog opacity
  is weighted by one bilinear shadow tap, dithered per pixel with interleaved gradient noise (static: there is no
  temporal pass yet). Only the sun-glow share of the in-scatter is shadowed (`ApplyAtmosphereFogShadowed`); fog past
  the last cascade split is sunlit. The range uses the view-wide splits, so lighting bands do not cut shafts at
  object edges. Settings: Light Shafts / Samples (1-32, default 8) / Strength, packed into `fog0.w` (no new
  `ObjectGPUData` field). Opt-in: 8 samples cost ~2 ms GPU on the Vulkan vista (terrain 1.03 -> 1.97, leaves
  4.26 -> 5.19 ms). `ENGINE_LIGHT_SHAFTS=0|N` overrides for A/B captures. `AtmosphereFogSettingsTests` covers
  round trip, clamps and the `fog0.w` encoding. Checked: FXC ps_5_0 + DXC SPIR-V compile; on the vista the change
  only darkens (forest within shadow range), no seams
  - [ ] Sky pixels are not shadowed (the skybox has no shadow map), so shafts never cross open sky
  - [ ] Animate the shaft dither now that TAA exists (on by default since 2026-11-19; the LOD dissolve dither
    already rotates per frame, Foliage Phase 1b); 4-6 samples may then suffice
  - [ ] Not yet seen as distinct shafts: needs a low sun behind trees/ridges with moderate fog. Judge with the step 0
    shot when tuning defaults (below)
- [x] `GameBackendCapture` with `--backend dx12` failed "renderer did not return the requested scene capture" when
  the window came out at 1520x873 instead of 1920x1080. Two bugs: (1) startup placement centred the benchmark
  window on the splash's monitor and shrank it to that monitor's work area (smaller or DPI-scaled monitor);
  benchmarks now skip `StartupSplash::PlaceWindow`. (2) DX12 mapped `rowPitch * height` of the readback, but the
  copy's last row is not pitch-padded, so widths not a multiple of 32 px (64 px for the RGBA8 back buffer copied now) overran the buffer and `Map` failed; it
  maps the footprint size now. `IGameRenderer::ReadSceneCaptureRGBA` returns the size actually captured (DX12 and
  Vulkan read the window size; Vulkan also recreates its readback buffer after a resize), and the benchmark writes
  the image at that size and says when it differs from the request. Checked DX12 at 1520x873 and 1920x1080, and an
  oversized 3840x2400 request (Windows caps the window at 2564 px wide here) on DX12/Vulkan/DX11. The capture tool's
  default scene (`oak_meadow.scene`, removed) is now `terrain/landscape_demo.scene`
- [x] Fog particles (CPU, GPU, fluid surface) and MMD outlines. Particles read a per-pass `particleFog` buffer
  (t7 / Vulkan binding 8: camera position + fog0..3, since particle constants already fill the 128-byte push
  constant); alpha particles blend to the in-scatter, additive ones fade by transmittance
  (`AtmosphereFogTransmittance`); the fluid surface fogs at its density-weighted particle center. Checked on DX12,
  DX11, Vulkan with a dense-fog copy of particle_showcase; fluid river runs on DX12/Vulkan. MMD outline fog
  compiles but was not seen in a capture
- [x] Fog the skybox seen through portals: `ViewLighting::SetSkyboxFog` now sets the sky fog terms for the main sky
  and portal skies alike (portal skies use the connected view's eye height). With fog forced on in
  portal_traversal_demo the sky inside the aperture matches the main sky exactly (213, 219, 226)
- [x] Tune defaults against the step 0 test shot: on the new vista the defaults (density 2e-4 at height 0, falloff
  0.004 = 250 m scale height, sky distance 30 km) give the layered look: haze pools on the far valley floor and
  around the ridge bases while the peaks and the foreground stay crisp. Thinner, deeper fog (8e-5..1.2e-4 with
  830..500 m scale heights) washed the whole range to one flat tone. The landscape scene never enabled fog; it
  does now, with those values written out (the generator carries `<settings>` over)

### 2. Terrain scale + erosion (Tools/build_landscape_demo.py)
- [x] Grow world to 16–32 km with 800–2000 m relief (now 24.2 km, -56..1888 m; archive v3 stores its own scale, 1/16 m)
- [x] Coarser spacing outside the play area (16 m samples, upsampled on read); 4 m within 3 km of the start
- [x] Verify far-terrain cache/LOD in-engine at 24 km: confirmed faceted. Far tiles take vertices only at 128 m cell
  corners (the archive is read just for seam cells), so distant peaks were a few flat-shaded triangles. The macro
  color map now carries a 16 m world-space normal map (same grid and mapping, texture slot 2,
  `terrainMacroParams.w`), and distant tiles are lit per pixel with it whenever a macro or long-range shadow map
  is bound: the ranges shade with eroded ridges, gullies and spurs. Same on DX12/DX11/Vulkan
- [ ] Far-terrain silhouettes are still the 128 m polygon outline (visible on peaks against the sky). A finer far
  grid (e.g. 32 m from the archive's 16 m samples) needs sub-cell vertices in the far-tile mesher
- [x] Hydraulic erosion pass (particle-based) before writing `world.heights`
- [x] Export flow and deposition maps alongside heights (`world.flow`, `world.deposition`; format in the script docstring)
- [ ] Editor tool: terrain generation window (size, relief, seed, erosion iterations) that regenerates and reloads the landscape

### 2b. Terrain editor tools (done; needs an artist pass in the editor)
- [x] Realtime brushes: Erode (hydraulic), Weather (thermal), Noise, Terrace
- [x] Select tool: rectangle / ellipse / lasso / polygon with feather, draped highlight, Apply commits one undo step
- [x] Region operations: hydraulic + thermal erosion, smooth, noise, terrace, flatten (Ctrl+click picks height), raise/lower, paint layer

### 3. Distant terrain color + forest coverage
- [x] Macro color map at 16 m (1513^2, the erosion grid) instead of the far palette's one color per 128 m cell
  corner. `LandscapeDriver` composes it on a worker from the near paint rules (`PaintSample`), slope, and the
  generator's `world.flow`/`world.deposition` (`TerrainHydrologyMap.h`, validated against the archive signature);
  `TerrainManager` uploads it as an sRGB mip chain and binds it in texture slot 5 (`ObjectGPUData::terrainMacroMap`,
  `terrainMacroParams`). Distant tiles take their albedo from it; loaded cells blend to it between
  `macroColorNear`/`macroColorFar` (256/640 m) so both sides of the loaded-cell border match. Alpha holds forest
  cover. Editor: TerrainManager "Terrain appearance" (Macro color map, start/full distance, tooltips) and
  LandscapeDriver "Macro color" group (map paths, lushness, flow threshold, scree, scree depth, gully rock,
  brightness, with tooltips via the new `Component::RegisterFieldTooltip`). Draw preparation polls the composer's
  settings every 100 ms, so edits rebuild it live in the editor and in play (0.8 s per build). Verified on DX12,
  DX11 and Vulkan: slopes gain gully and scree detail, mid-distance ground fades into the far colors
- [x] Use flow for lush valley floors / streams, deposition for scree, steep + high flow for bare rock (in the macro
  map, i.e. at distance; the near paint does not read the erosion maps yet)
- [ ] Macro colors use the materials' authored means while the near cells' distant blend uses measured layer-array
  means; at 0.85 brightness distant meadows read a little duller than textured near meadows. Tune, or feed the
  measured means into the composer
- [x] Forest-density map baked with the macro map (macro alpha = canopy cover; the color takes the canopy tint), and
  distant tiles shade forest as a bumpy canopy: `ApplyDistantCanopy` (Object.hlsl) tilts the 16 m macro normal with
  world-space crown/clump/stand noise (14/42/130 m) and darkens its hollows, each octave fading out once a pixel spans
  half its period. TerrainManager "Distant canopy relief" (0..1, default 0.6, tooltip), packed as
  `terrainMacroParams.w = 1 + relief`. Subtle in practice: the canopy albedo is very dark, and within
  `farObjectRadius` (8 km) the far tree impostors cover it; with impostors off it changes 2.7% of the vista's
  pixels, all on the far forest. No specular on distant tiles, so no canopy roughness

### 4. Lighting + post
- [x] GTAO (half-res, temporally filtered) for contact darkening under trees and in valleys:
  `Shaders/AmbientOcclusion/AmbientOcclusion.hlsl` (compute) after the main view's draws
  (`SceneRendererLighting::ComputeAmbientOcclusion`). New context call `CopySceneDepth` copies the scene depth into
  a buffer (DX12 `CopyTextureRegion` of the depth plane; Vulkan `vkCmdCopyImageToBuffer` while the scene pass is
  paused; DX11 a small compute copy from a now-typeless depth; recorded in command packets, gated by
  `SupportsSceneDepthCopy`). GTAO per half-res pixel: view-space position/normal from depth, 2 slices x 2 sides x
  6 steps (rotated per pixel and frame), cosine-weighted arc integration, then blended with last frame's AO
  reprojected through the camera stored in the output's header (depth-checked). Applied next frame through a new
  structured slot 47 (DX12 root SRV t47, Vulkan binding 48) to ambient + environment (diffuse and specular) only,
  never sunlight: object surfaces, foliage cards, grass, fast terrain and per-pixel distant tiles
  (`ScreenAmbientOcclusion` in Object.hlsl: reprojects, 4-tap depth-matched filter, 1 off screen / disoccluded).
  Scene `ambientOcclusion` (`enabled`, `radius` 1.5 m, `intensity`, `temporalBlend` 0.15); editor block. Landscape
  demo: on. No measurable frame cost on DX12 or Vulkan (vista and near view, 4090). Notes:
  - fxc compiled `continue`/`break` inside the unrolled GTAO loops into a GPU hang (TDR); the loops are now
    branch-free. One later DX12 run still froze GPU timings once and has not reproduced in 12 runs since; the
    debug layer is clean. `ENGINE_GPU_DEBUG=1` now prints DX12 debug-layer errors/warnings to stderr
  - Editor viewports (D3D11View / D3D12View / VulkanView) don't register their depth yet, so AO is game-view only
  - Dense foliage shows some half-res blotchiness; a depth-aware spatial filter pass would smooth it. 2026-11-19:
    the lookup in Object.hlsl is now a depth-matched 3x3 tent (was 4 taps), which removed the diagonal noise
    stripes on plants; a filter pass in the AO shader itself is still open
  - Vulkan's AO GPU timer includes the barrier wait for the opaque pass to drain (reads ~1.3 ms; frame time
    shows no cost)
- [x] Auto exposure (luminance histogram, eye adaptation) with editor min/max EV and speed: three small passes
  in each backend's post-process before compose (`Shaders/PostProcess/PostProcess.hlsl`
  `ExposureLuminancePS` / `ExposureHistogramPS` / `ExposureAdaptPS`, shared plumbing in `Graphics/PostProcess.*`).
  64x32 log-luminance map (centre-weighted metering) -> 64-bin histogram over -14..+10 EV -> 1x1 adapt pass that
  trims the histogram to [lowPercent, highPercent], clamps to [minEv, maxEv] and eases from the previous frame
  (two ping-pong RGBA32F targets, separate up/down speeds, snaps after resize or re-enable). Compose maps the
  adapted average to mid grey; `outputExposure` stays as compensation. GPU-only, no readback. Lit views only.
  Scene `autoExposure` (`enabled`, `minEv`, `maxEv`, `speedUp`, `speedDown`, `lowPercent`, `highPercent`);
  editor block under Output Tone Mapping. Landscape demo: on, compensation -0.4 (vista meters at -2.7 EV, about
  where the fixed exposure was). DX11/DX12/Vulkan meter identically. Note: fxc mis-compiled a nested,
  partially unrolled histogram loop (DX counted ~1/10 of the map); it's one flat `[loop]` now. Not separately
  GPU-timed (64 + 1 pixels reading a 16 KB map)
- [x] Long-range shadow: terrain sun-visibility map ray-marched over the far height grid on a worker thread when the sun moves (`TerrainManagerSunShadow.cpp`); near terrain multiplies it into the cascade shadow, distant tiles are lit per pixel with it. TerrainManager `longRangeShadows`, `longRangeShadowSoftness`
- [ ] Long-range shadow for non-terrain objects (far trees/impostors stay sunlit inside a ridge shadow; needs a global texture slot on all three backends)
- [x] Bloom: half-res 13-tap downsample / tent upsample mip chain in each backend's compose pass (`Shaders/PostProcess/PostProcess.hlsl`, shared by DX11/DX12/Vulkan). Scene `bloomEnabled`, `bloomIntensity`
- [x] Color grading (warm highlights, cool shadows): analytic split-tone + contrast + saturation after tone mapping. Scene `colorGrading*`, `grading*`
- [ ] Optional authored 3D LUT input for grading (the split tone is analytic today)
- [x] Sun disc / glare: limb-darkened disc + Henyey-Greenstein glow in `Skybox.hlsl` toward the brightest directional light; bloom provides the glare. Scene `sunDisc*`, `sunGlowIntensity`

### 5. Sky computed from the sun
- [x] Hillaire-2020-style sky: `Rendering/Atmosphere/PhysicalAtmosphere.*` (Rayleigh/Mie/ozone media, Bruneton
  transmittance LUT 256x64, Hillaire multiple-scattering LUT 32x32, planet geometry in double) and
  `PhysicalSky.*`, which renders the sky on a worker into a 512x256 RGBA32F world-space panorama (with mips)
  whenever the sun moves >0.2°, its illuminance >1%, the camera altitude >100 m or the parameters change.
  The panorama replaces the sky-view LUT: it feeds the existing skybox draw and EnvironmentMap unchanged, so
  sky, reflections and ambient all come from one image. CPU-side because the generic graphics context has no
  offscreen color targets; the GPU only samples. Scene `physicalSky` (`enabled` is the HDRI toggle; HDRI is
  used until the first panorama is ready), `groundHeight`, `rayleighScale`, `mieScale` (haze), `mieScaleHeight`,
  `mieAnisotropy`, `ozoneScale`, `groundAlbedo`, `skyIntensity`; editor "Physical Sky" block. The skybox's
  analytic sun glow is off over the physical panorama (the aureole is in it); the sun disc stays.
  Landscape demo: enabled, `mieScale` 4. Tests: `.local/tests/PhysicalAtmosphereTests.cpp`
- [x] Derive sun light color and ambient SH from the same atmosphere model: the strongest directional light is
  multiplied by the transmittance toward the sun at the camera altitude (`attenuateSun`), and the environment
  map / SH are prefiltered from the physical panorama on the same worker
  (`SceneRendererLighting::ApplyPhysicalAtmosphere`)
  - Found while doing this: the split-sum BRDF LUT had lost its `/ sampleCount` in the 11-09 refactor
    (`RenderUtilities.h` `BrdfIntegration::Integrate`), making specular IBL ~256x too bright. It went unseen
    because PNG HDRI skyboxes drop their CPU pixels after upload, so `EnvironmentMap::Acquire` returns null and
    those scenes get no environment lighting at all (still true for the HDRI path; separate issue). Fixed, with
    a LUT range check in PhysicalAtmosphereTests
- [x] Replace step 1's analytic fog with the aerial-perspective volume behind the same shader include:
  `AtmosphereFog.hlsli` has a physical mode (fog0.w >= 100, packed by `ViewLighting::PackPhysicalAerialPerspective`)
  used by objects, particles and outlines with no call-site changes; light shafts keep working
  (`aerialPerspective` toggle, height fog otherwise). Deviation: it's analytic, not a froxel volume:
  per-channel Rayleigh + Mie extinction with their own exponential height profiles integrated along the
  ray, sun single scattering (Rayleigh + HG phases, sun illuminance at the camera) and Hillaire's
  multiple-scattering term, combined as color*T + S*(1-T)/tau. Exact for the lower atmosphere at
  terrain distances. A froxel volume needs 3D textures and a global texture slot, which the generic graphics
  context doesn't have yet
  - [ ] Froxel aerial-perspective volume (32x32x32, compute) once 3D textures exist; would add cloud/terrain
    shadowing of the in-scatter beyond the current shaft march
- [x] Volumetric clouds: `Shaders/Clouds/Clouds.hlsl`, a fullscreen pass blended (premultiplied) over the sky right
  after it, so terrain and objects drawn later occlude it. Flat layer (base 2600 m, 1400 m thick, above the
  1.9 km peaks), ray-marched per pixel (48 steps, jittered): 3D value-noise FBM shaped by a 2D weather/coverage
  noise, cumulus profile (flat bases, threshold rising with height for domed tops), Worley billows and value-noise
  edge erosion; 5-step light march toward the sun (Beer-Lambert with a multiple-scattering floor), two-lobe
  Henyey-Greenstein phase, sky light brighter toward the tops; fades out toward max distance into the hazed sky.
  Wind drifts the field. `SceneSettings::volumetricClouds` (coverage, density, base, thickness, shape scale, edge
  detail, wind speed/direction, sky light colour/intensity, sun intensity, steps, max distance), serialized, in
  scene Properties > Atmosphere with tooltips, read every frame (live in the editor). Constants: 128 bytes in an
  extra slot of the sky constant ring (`SceneRenderer::kCloudConstantSlot`), Vulkan SPIR-V built by CMake.
  Enabled in landscape_demo. Sky pass (sky + clouds) 1.0-1.3 ms GPU on the vista, same on DX12/DX11/Vulkan
  - [x] Cloud shadows on terrain and objects: a scrolling cloud-shadow cookie on the sun (Foliage Phase 3),
    statistically like the clouds
  - [ ] Cloud shadows that sit under the actual volumetric clouds (project the coverage/density toward the sun)
  - [x] Half-resolution march: `Clouds.hlsl` `CSMain` ray-marches one thread per half-res pixel into a structured
    buffer (`SceneRenderer::m_cloudSamples`, retired on resize) in a compute pass before the sky
    (`SceneRendererLighting::MarchVolumetricClouds`); `PSMain` upsamples it bilinearly (graphics slot 6, the lights
    are rebound after). The grid size and step count are bit-packed into `shape.w` to stay within 128 bytes of
    constants. New GPU stage `Clouds` (`gpu_clouds_ms`). Vista cost 1.33 ms -> 0.61 ms on DX12 (march 0.58 +
    sky/composite 0.03), DX11 0.46, Vulkan 0.82; image nearly identical (edges marginally softer). Vulkan's
    `StorageBarrier` now also covers fragment-shader reads (it only covered compute/vertex/indirect)
  - [ ] Temporal reprojection (march a quarter of the pixels per frame) if the march needs to get cheaper still
  - [ ] Clouds in portal views and fog/aerial perspective between camera and cloud

### 6. Generated landscape assets (Python, same conventions as Tools/generate_meadow_accents.py)
- [x] Conifer generator (`Tools/generate_conifers.py`): trunk + layered branch whorls with needle cards; ~4 species/size variants; LOD chain; mid/far quality
  Spruce_tall_01 (24 m), Spruce_young_01, Pine_scots_01, Fir_subalpine_01 under `Prefabs/Woodland/Conifers`. LOD0 sprays
  (3-14k tris) / LOD1 fewer larger sprays with card coverage matched to LOD0 (side coverage within ~5-20%) / LOD2
  whorl stars / view-baked impostor (6 views + top, 38 tris, `impostorBillboard`, same card layout as the oak pack).
  Shared procedural needle atlas (spruce, fir, pine, clump cells) and bark; species colour is muted linear vertex colour
  (inner/outer/tip, per-branch jitter). Instanced chain resolves to LOD0 -> LOD1 -> impostor. Shared helpers:
  `tools/woodland_assets.py` (native .mesh writer, numpy rasterizer/impostor baker, PNG I/O, prefab/material writers)
- [x] Boulder / cliff generator: noise-displaced, faceted meshes, ground pivot, LODs, triplanar rock material
  `tools/generate_boulders.py`: Boulder_01-03, Boulder_flat_01, Cliff_block_01/02 under `Prefabs/Woodland/Boulders`.
  Superellipsoid cut by random planes (smooth-min) + FBM, cliffs with vertical cuts and stepped strata; one radius
  function sampled on icospheres per LOD; normals split at 38 degree creases. New `Material::triplanarScale`
  (world-space 3-projection base/ORM/occlusion/normal with UDN blend, projections < 4% skipped; packed in
  `textureUvSets1.z`, a terrain-only lane) with the Rock030 set; meshes keep box UVs at the same scale
- [x] Far-forest clusters / impostor cards baked from conifer + oak meshes; hook into TerrainManagerFarObjects
  `tools/generate_forest_clusters.py` bakes 4 stands (2 oak from the pack's LOD2 meshes + material textures, 2 conifer)
  of 14 trees on 36 m. `TerrainManager::farClusters` (asset, match text, baked tree count) + `farClusterDistance` /
  `farClusterSize` / `farClusterMinimumTrees`: far blocks wholly beyond the distance bucket their trees on the grid and
  replace squares where one family is >= 70% with one cluster (centroid, scale sqrt(count / baked)). Landscape demo:
  2.2 km, 40 m, 5 (scene + `tools/build_landscape_demo.py`)
- [x] Check alpha-to-coverage / coverage-preserving alpha mips so distant canopies do not thin out or shimmer
  Real-time rendering is single-sample (MSAA only in DX11 video export), so A2C does not apply. Measured: PNG foliage
  gets runtime box-filtered mips, which at the 0.32 cutoff grow oak canopy/impostor coverage 14-60% by mip 4-7 (crowns
  bloat and pulse) and drop sapling impostors to 25-48%. `tools/cook_foliage_textures.py` cooks every Mask material's
  PNG base colour with `TextureCook --alpha-cutoff` (push-pull fill + per-mip coverage rescale, BC7) and repoints the
  19 materials; generated foliage is cooked the same way. `--check` prints the per-mip table
- [x] Cloud and valley-mist sprite textures (noise-based) + soft particle/card prefabs
  `tools/generate_sky_sprites.py`: Cloud_Puff_A/B, Mist_Wisp_A/B, Mist_Sheet (`Textures/Atmosphere`), prefabs
  ValleyMist / LowClouds (ParticleEmitter) and MistCard (Blend mesh) under `Prefabs/Woodland/Atmosphere`. Soft
  particles: `ParticleEmitter::softness` fades against the main view's depth copy (reused from AO, or copied when AO is
  off; `SceneRendererLighting::SceneDepthForPixelShaders`, particle t8), plus `nearFadeDistance`, `fadeInTime`,
  `sizeVariation`, `randomRotation`. Portal views draw hard. Verified on DX12/DX11/Vulkan
- [x] Prefabs for all of the above under `Assets/Prefabs/Woodland/`, placeable by TerrainObjectLayer
  Conifers/, Boulders/, Clusters/, Atmosphere/. Landscape demo uses them through its layers: `LandscapeDriver`
  grows conifers by altitude (`ConiferShare`: oaks below 350 m, mixed foothills, conifers from 710 m to the tree
  line; picked from existing draws so the lowland oak layout is unchanged) and puts the generated boulders in rock
  fields; far impostors and clusters follow automatically. `Scenes/terrain/woodland_assets_preview.scene` shows every
  asset at its LODs; captured on DX12, DX11 and Vulkan
  Demo update: cliff blocks join the forest layer on steep rock below the snow (<= 2 per cell, upright, sunk by the
  slope; 900 m reach). They are authored `cullAfterLastLod`, which `GetObjectLodProfile` now passes through and
  `FarPlacements` skips, so near-only props never become far objects (~30% of cells are steep). The scene gets an
  "Atmosphere" group (`build_landscape_demo.atmosphere_items`): LakeMist (new wide prefab) over all five lakes,
  ValleyMist + three mist cards on the first lake by the start, LowClouds on four summits in the vista
- [x] Editor: run generators from an editor tool and refresh the asset browser
  Tools > Asset generators lists `tools/generate_*.py` / `cook_*.py` (docstring as tooltip), plus "Run all woodland
  generators" (ordered queue). `AssetGeneratorRunner` runs one at a time without a console window, streams output to
  the Console, then reloads rewritten textures (`Texture::Invalidate`) and refreshes open scenes
  (`OnAssetContentsChanged`); the asset browser lists new files through its directory watch. Builds; not yet exercised
  in a running editor
- [x] Wider, less bright tree and foliage colours: `Material::foliageColorVariation` (masked foliage; world-space noise
  at ~9 m crown and ~70 m stand scale trades yellow-green against blue-green and +-~20% value; `textureUvSets1.w`) on
  oak leaves/impostors (0.7), conifers and clusters (0.55/0.7), identical across LODs. Oak leaf + impostor tint
  [0.86, 0.84, 0.77] (darker, less saturated); terrain meadow layer `grass.material` [0.24, 0.32, 0.12] ->
  [0.20, 0.26, 0.11]

### 7. Demo composition
- [ ] Final landscape demo vantage points: the MeadowSlope and Escarpment viewpoints of the expectations section
  (sunlit meadow, forest edge, forested hills behind). The earlier idea (elevated camera over a valley, backlit
  sun, dark foreground silhouette) stays as the Vista viewpoint, not the main shot
- [ ] Second reference set (`.local/reference-images/`): still water with reeds, lily pads and reflections (01-03);
  tall flowering meadow to the horizon (04, 05); sun haze between conifer trunks and a foggy broadleaf forest
  (01, 06, 07); layered ridge haze (08). Today the far forest reads as a flat light-green wall and the meadow
  foreground is dark (`.local/foliage/mv2_walkturn.png`)
- [ ] Re-run the step 0 benchmark, standing and moving (`--drive-forward`, `--look-yaw`): 60+ fps floor while
  walking and turning, 100+ on typical poses (RTX 4090). Now ~40 fps standing, ~28-32 fps walking (see Status)
