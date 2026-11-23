# Engine TODO

One plan for the landscape demo: look, foliage, grass and performance. `grass.md` and `foliage.md` were merged into
this file on 2026-11-19; their phases are the "Foliage" and "Grass" sections below, and code comments that say
`.md/todo.md Foliage Phase N` point there. Finished work is in [todo_history.md](todo_history.md), oldest first; completed items were last moved there on 2026-11-22.

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
| **Forest edge** | a closed wall of crowns from the ground up: shrubs and saplings fill below the crowns, no trunk line, no soil, almost no sky through the canopy; heights vary; single trees stand out in the meadow | 2026-11-20 evening, ForestWall pose (edge 75 m ahead): a closed wall from the grass up, no trunk line, soil-coloured pixels along its base 1.5 % → 0.2 %; one height and one species (small and medium oaks), no single trees in the meadow in that view; sky through the silhouette not measured | from 50-150 m, no bare soil or trunk line under the edge and sky through the forest silhouette under 5 % of its area; a shrub belt at the edge; single trees in the meadow | soil and trunk line yes; sky share and variety open |
| **Crown tone** | near crowns are darker than the meadow: 0.03-0.05 against 0.07-0.12 (about half), with lit tops | shaded crown undersides 0.024 against a 0.077 meadow (a third); were 0.015, nearly black | sunlit meadow about 2x the mean luminance of the adjoining crowns | close |
| **Far forest** | hills covered to the top in canopy with visible crown texture and pale field patches; luminance 0.08, no brighter than the sunlit meadow, shifting blue with distance (62 / 79 / 83) | front-lit far forest 0.082 against a 0.077 meadow (was 0.105, 1.5x the meadow); still flat: 10th to 90th percentile 0.066-0.098 where the reference spans 0.033-0.129; no blue shift at 300-600 m; against the light (Vista) it drops to 0.02 | far forest no brighter than the sunlit meadow; crown texture readable to ~2 km; hue moves toward blue with distance | brightness yes; texture and haze no |
| **Sky** | saturated blue (79 / 124 / 177, saturation 0.56) rising to a bright hazy horizon (luminance 0.6); white cumulus with soft shaded bases | 94 / 148 / 197, saturation 0.50-0.53; horizon luminance 0.66 on the test meadow | clear-sky saturation at least 0.5 away from the sun; horizon luminance at least 0.5 | yes |
| **Terrain in view** | rounded hills a few hundred metres high, forested to the top, with meadow clearings on the slopes; one sheer rock escarpment under a forested plateau (frame 04), vegetation running down its gullies | rolling forested hills with meadow slopes around the start (MeadowSlope pose: 140 m drop to the valley), alpine range behind; escarpment 66-95 m high under a forested plateau | around the start: rolling forested hills with meadow slopes; the alpine range as a distant backdrop; one escarpment at least 60 m high and 300 m long with forest above and below | hills and escarpment are in (captured 2026-11-20); the face is covered by scanned rock but hidden from the ground by the forest at its foot | partly |
| **Tree variety** | several silhouettes in one view: tall narrow crowns, broad round ones, a dark flat-topped lone tree, low shrubs | eight oak models and four conifers | at least three clearly different broadleaf silhouettes in any edge view, plus the shrub belt | no |
| **Frame rate** | reported by the developer, not verified: about 60 fps in the 10M-tree video's HUD; the earlier showcase 130 fps average, ~80 in dense forest, ~200 from the air, at 1440p on an RTX 3080, without upscaling, frame generation or hardware ray tracing | ~42 fps standing, ~35 fps walking (28.5 ms median, p95 42, 11 frames over 50 ms of 400) at 1080p on the RTX 4090; after the terrain and edge pass: standing 20.3-23.0 ms, walking 29.0 and 29.5 ms (11 and 22 frames over 50 ms), walking and turning 37.0 ms (50 over 50 ms) | unchanged: 60+ fps while walking and turning, no frame over 33 ms in a 400-frame walk, 100+ on typical poses; native resolution, no upscaling | no |

**Look pass 1 (2026-11-20).** Captures `.local/foliage/i1_MeadowSlope.png`, `i1_ForestEdge.png`, `i1_Vista.png`,
`i1_ForestFloor.png`; the test meadow matches on DX11, DX12 and Vulkan (`x_dx11.png`, `x_vulkan.png`, `x_dx12.png`).
- [ ] Grass follow-ups: the far ring still reads as rows of separate clumps at 20-60 m (dark ground between them);
      the shells have not been judged from a raised view; the +1.8 ms of grass GPU is not budgeted; the detailed
      grass still ends at ~20 m because the detail budget is at its floor; coverage numbers (render mode 6) not
      re-measured.
- [ ] **Exposure at forest edges**: with a dark forest filling the frame, auto exposure brightens the meadow to
      0.22 (ForestEdge), about a stop over the target. A higher floor (-3.5) brings it to 0.16 but turns the forest
      floor nearly black (whole frame 0.016); metering on the brighter 30 % makes MeadowSlope too dark (0.044).
      Needs metering that weights sunlit ground, or partial adaptation.
      - [x] 2026-11-22 (agents/look): `autoExposure groundWeight` (0-1): the top third of the frame meters a fifth
            as much, so the meadow meters alike whatever stands behind it; landscape: groundWeight 1,
            outputExposure -0.25 -> -1. 2560x1440, meadow near / mid: ForestEdge 0.216 / 0.249 -> 0.060 / 0.100,
            FieldWalk 0.101 / 0.104 -> 0.083 / 0.074, forest wall 0.065 -> ~0.04 (reference crowns 0.03-0.05).
      - [x] outputExposure -0.75 (2026-11-22): near / mid meadow ForestEdge 0.078 / 0.129, FieldWalk 0.095 / 0.087,
            MeadowSlope mid 0.065 (cloud shadow). Forest interior darker than before (ForestFloor whole frame 0.015):
            accepted by the user for now.
- [x] **Understory foliage glowed in canopy shade** (user 2026-11-22: yellow-orange blades by a rock at
      ForestFloor): the woodland pack's tall bush cards got FoliageTransmission's crown-scatter term (0.3 of the
      sun when shaded, meant for a crown's own underside). `Material::foliageCrownScatter` (default 1); the pack
      bush materials set 0. Blade area 99th percentile 0.204 -> 0.079 (`.local/foliage/ff_blades_fix3.png` in
      agents/look); other foliage materials unchanged.
- [ ] **Far forest texture and haze**: front-lit impostors are flat (no shadow between crowns) and there is no
      blue shift at 300-600 m. Needs occlusion baked into the impostor atlases (Foliage Phase 5) and a look at
      aerial perspective strength at short range.
- [ ] Forest interior is dark after this pass (ForestFloor whole frame 0.027, floor 0.012): check it against the
      first reference set before changing it.
- [ ] Not re-checked after this pass: the look in the editor, grass_test coverage numbers, the moving-camera runs
      other than one walk.

**Terrain and forest edge pass (2026-11-20, second machine: RTX A2000 laptop; captured and reworked the same
evening on the RTX 4090).** The first seven items were written on the laptop without a landscape capture, from
the generator's previews and counts on the archive; the capture and what it changed follow them.
Smoke test of the new build only: lighting_showcase on DX11 at 640x360 renders correctly.
- [ ] MeadowSlope foreground is still dark (`k5_MeadowSlope.png`: 0.030 against 0.124 for the sunlit valley
      meadow, a quarter). At 1440p (`h1_MeadowSlope_*.png`) it is a soft-edged band lying across the slope
      and the valley: the sun's scrolling cloud-shadow cookie (2400 m, 6 m/s), not the slope (the sun is 57
      degrees up; a 22 degree slope away from it loses a third, not three quarters). Which pose is shaded
      depends on run time, so the finder cannot avoid it. Open: whether cookie shade at a quarter of sunlit is
      wanted. The brightness rows of the table were measured at the old pose and are not re-measured.
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
- Adopted (2026-11-21 review, "Next" 1 and 2): vegetation instance data streamed into GPU buffers and culled,
  level-selected and counted on the GPU, so per-frame CPU cost does not grow with loaded cells or tiles. Here
  every batch and grass tile is a scene object that costs scene-thread time.
- Not adopted: virtual geometry, voxel far vegetation and voxel global illumination. Each is months of work;
  octahedral impostors ("Next" 5) are the far-vegetation step instead.
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
- [ ] **The same trees as scene objects (unbaked) look different, and that path is the odd one.** Scene objects
      keep LOD0-2 and never the impostor, so the far row (420-470 m) is whorl stars and nearly invisible; and
      the young spruce turns sparse between 34 and 38 m, where the authored distances say LOD1 until 85 m (65 m
      at that view scale). Not caused by the static work (`ENGINE_STATIC_BAKE=0` shows it); cause not found.
      It means ticking Static changes how a tree looks at mid range: fuller at 38-65 m, an impostor beyond.
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

## Status (2026-11-20, night)

Landscape demo, DX12, 1920x1080, RTX 4090, RelWithDebInfo, uncapped; moving runs are 400 frames. The scene now
loads 3 rings of full cells around the player (was 5); far-forest blocks draw everything beyond, cliffs included.

| Run | 2026-11-19 | Now (2026-11-21, 00:50) | Frames over 50 ms, now |
|---|---|---|---|
| Walking, forest (`--drive-forward 380`, ForestFloor) | 31.3 ms, p99 115 | 17.3 and 17.6 ms, p99 46-72, max 68-92 | 3-7 |
| Walking, meadow (MeadowSlope start) | not run | 18.2 and 18.9 ms, p99 53-56 | 4-6 |
| Walking, open field (`--viewpoint FieldWalk --drive-forward 380`; added 2026-11-21 for grass, eye 1-2 m over open meadow the whole way) | not run | 18.2 and 19.0 ms, p99 63-65, max 92; grass GPU 6.7 ms | 9 |
| Walking and turning (`--look-yaw 3`) | 35.2 ms, p99 115 | 21.0 and 21.7 ms, p99 64-65, max 71-74 | 8-10 |
| Standing, 2560x1440: Vista / Escarpment / MeadowSlope | 29.1 / 22.8 / 23.5 ms (5 rings) | 19.3 / 21.5 / 17.7 ms (before the block merge) | 0 |

- 2026-11-21 afternoon, after per-plant detail, the 80 m shrub reach, the tile skip and the area-scaled
  shrub cap: forest walk 17.1-17.2 ms, meadow walk 18.6-19.1 ms (the table's numbers are from the night before).
- Capture and benchmark windows reopen where they were last left (`window-GameCapture.txt` in
  %LOCALAPPDATA%\Engine; position only, never resized).
- The walk in these runs moves at ~23 m/s (173 m in 7.4 s), not walking pace: pop-in is far more visible at
  that speed, and every reach below is short for it.
- 60 fps (16.7 ms) while moving: the forest walk is ~1 ms short, the meadow walk ~2 ms, walking and turning
  ~4.5 ms. Before the ground-cover reach was restored (below) the forest walk measured 16.7-16.8 ms.
- What got it here on 2026-11-20/21 (details in todo_history.md, end of file): static objects skip the
  transform test for their cached render item; grass tile objects are pooled; one shared build per prefab
  prototype (the cell-crossing stutter); object removal looks ids up instead of walking the scene; 3 loaded
  rings; prefab batches merged per 2x2 cells.
- The frame is CPU-bound. Cost follows the number of prefab batch objects that exist (one per prefab part per
  cell): about 2 ms per 1,000, measured two ways. GPU is 9-11 ms.
- Plan direction since 2026-11-21: structural changes first (vegetation on the GPU, ground cover generated on
  the GPU, a retained render scene), settings tuning after; see "Next".
- Time until the world is fully ready: ~14.5 s (was ~29 s), most of it far-forest placement generation out to
  8 km (see "Next" 5).
- Checked on DX12 only. DX11 and Vulkan have not been run since look pass 1.
- Captures fail now and then with the device removed; rerun them (parked by decision on 2026-11-20). Moving
  drives on 2026-11-21 froze three times instead (memory past 18 GB, the user's screen froze): no drives until
  okayed.
- Cliffs (2026-11-21/22): stamps, then sculpted terrain on the test escarpment; see "Cliffs as sculpted
  terrain" under "Look and content".
- **Performance 2026-11-22** (1920x1080, 400-frame walks, single runs plus two interleaved on/off pairs;
  memory watchdog, peaks 5.5-8.8 GB, no runaways):

  | Run | Frame median / p99, frames over 50 ms | GPU (largest) | CPU serial (update + prepare + record) |
  |---|---|---|---|
  | FieldWalk standing | 20.2 / 23.1, 0 | ~19: grass 10.4, opaque 6.1 (foliage depth 3.1) | 1.2 + 3.0 + 12.7 (camera 9.4, shadow 3.1) |
  | Field walk | 22.1-22.9 / 70-72, 12-17 | ~20: grass 10.2-10.4, opaque 6.5 | 2.1 + 4.0 + 14.1 |
  | Forest walk | 21.0-21.7 / 57-67, 9-11 | ~17.5: opaque 9.0-9.5 (foliage depth 4.4), shadow 3.5 | 1.8 + 3.4 + 14.6 |
  | Meadow walk | 26.1 / 71, 19 | ~15: opaque 8.1 | 2.3 + 3.8 + 18.3 (camera 12.8, shadow 5.1) |
  | Forest walk + turning | 24.3 / 162, 23 | ~17.7 | 2.0 + 4.2 + 16.3 |

  For 60 fps both sides must drop: the main thread's serial CPU (17-24 ms, two thirds of it command
  recording) limits every walk ("Next" 1, 3, 4); the GPU is 17-20 ms (grass in meadows, foliage depth
  pre-pass, leaves and shadows in forest: "Next" 2, 5, 7); and cell crossings give 9-23 frames over 50 ms
  per walk (update, object creation and streaming LOD on the worst frames: "Next" 1, 2).
  Today's additions (tall fields, sculpted cliffs, far stamps; `ENGINE_LANDSCAPE_TALL_FIELDS=0
  ENGINE_SCULPTS=0 ENGINE_FAR_STAMPS=0`): forest walk no change (21.0-21.7 on, 21.0-21.8 off); field walk
  +1.1 ms frame, grass GPU 8.0 → 10.25. The walks were 17.1-17.2 (forest) and 18.6-19.1 (meadow) on the
  afternoon of 2026-11-21, so ~4 ms came in between and not from today's additions.
  - [x] Bisected (2026-11-22, forest walk, interleaved pairs; `ENGINE_TERRAIN_STAMPS=0`,
        `ENGINE_FAR_PATCHES=0` added for it): the far grid's full-resolution cliff-cell patches. Stamps cost
        nothing measurable. Patches on / off: frame 20.8-21.0 / 17.2-17.3 ms, camera record 10.0 / 7.5,
        occlusion build 2.7 / 0.9 (occluder triangles 357k / 73k), 7-9 / 2 frames over 50 ms. Fix: far tiles
        give the CPU occlusion buffer their coarse grid only (`Mesh::occluderIndices` and the new
        `Mesh::occluderVertexCount`, set in `TerrainManagerFarTerrainUpdate.cpp`): frame 17.3-17.4 with
        patches against 17.2-17.5 without.
  - [x] Patches still cost two ~150 ms hitches per forest walk (~65 ms without, at the same cell crossings)
        and ~0.4 ms GPU. Replaced 2026-11-22: no far-grid patches at all (`world.fargrid`, `write_archive`).
        Far stamp cells are now the cell's own terrain at stride 4 (16 m grid, edges at full resolution as
        at every terrain level, coarsest stamp or sculpt level), built through the cell's `Rebuild()`; far
        tiles leave them out (`TerrainManager::FarStampShowing`), keep cell detail in tiles that hold one,
        and stitch their edges from archive heights, as for loaded cells. Forest walk 17.3-17.4 ms, 2-4
        frames over 50 ms, worst 62-64 (patches: 5-6 and ~150 ms); terrain and shadow GPU as without
        patches. The 450 m cliff view is unchanged (`.local/terrain-stamps/farcells_450.png`).
- Uncommitted: everything since `4687647e`.

## Next, in order

**Direction (review of 2026-11-21).** Asked: less settings tuning and squeezing, more fundamental changes that
give larger overall gains while keeping the look of the reference frames. What the measurements say:
- The frame is CPU-bound, and CPU cost follows how many scene objects the renderer walks each frame: ~2 ms per
  1,000 prefab batch objects, measured two ways. Vegetation is ~4,000 of the ~5,300 mesh objects (near forest
  ~1,200, near props ~1,000, far blocks ~1,800). By that slope it is ~8 ms of the serial CPU frame (camera record
  7-8 ms, shadow record 3-4, scene prepare ~3); this is an estimate from the slope, not a direct measurement.
- Every vegetation batch is a scene object because placements stream as per-cell layers that create and destroy
  objects. That is also what makes cell crossings hitch (object creation, the 25-35 ms grass commit, the near/far
  block hand-off), and why 16x16 far blocks could not be used.
- The tuning of 2026-11-21 (shadow ranges, pre-pass off, detail distances, reach per plant class) moved the walks
  by about 1 ms either way. The detail budget cannot leave its floor because there is no slack to give it.
- The reference engine streams vegetation instance data and traverses tiles on the GPU ("What is known about
  the reference engine" above).

So: take vegetation out of the scene graph and onto the GPU (1, 2), stop rebuilding the rest of the render scene
every frame (3), then spend the freed time on the look (5-7). Settings tuning waits until 1-3 have landed
("Tuning", at the end of this section). Completed items of the earlier plan are in todo_history.md.

Reference check at full resolution (2026-11-20, `.local/reference-images` against `h1_*` tiles), largest gaps
first: (a) meadow, ref 04: fine thin blades as continuous cover to the horizon; ours is one broad-bladed clump in
rows with soil between, ending at 26 m. (b) forest interior, ref 07: haze between trunks so they fade pale with
depth; ours shows shaded trunks as dark blue-grey posts. (c) vista, ref 08: layered ridge haze and far forest
that still reads as conifer silhouettes; ours is navy blobs on pale untextured mountains and a flat green hedge
at mid distance.

**Far detail and gameplay visibility (user, 2026-11-22).** Paths read as spots, far objects and far terrain disagree,
and actors (players, NPCs, enemies) must render correctly at any distance: not hidden or revealed by grass that is
drawn for one viewer and not another, not floating on coarse far terrain, never culled like scenery. Research,
options and references: [far_detail_and_visibility_research.md](far_detail_and_visibility_research.md). Order:
- [x] Paths shaded per pixel at every terrain level (2026-11-22, agents/terrain). The trails are a contour of
      two-octave value noise, so instead of tracing polylines the terrain shader evaluates the same noise with an
      analytic gradient (`TerrainPathWeight` in Object.hlsl; generic `TerrainManager::ContourPaths`, set by
      LandscapeDriver from the `TrailMask` constants): near cells blend the gravel layer per pixel (only over
      grass, forest soil and gravel, inside the forest height band), far tiles blend the far gravel colour (off
      steep ground), the near cells' fade to the macro map keeps it. Past the pixel footprint the soft edge
      widens and the strength falls by the same share, so far trails stay one faint line. The paint, macro and
      far colours leave trails out; the cell heights sink 12 cm along the track; ground cover (mesh fields and
      the GPU blades' paint) keeps off it via `TerrainManager::ContourPathShare`, the CPU twin of the shader
      function. `ENGINE_TERRAIN_PATHS=0` paints them at the vertices again.
      Captures (`C:/repos/EngineProject-terrain/.local/terrain/c1`, `c2`, before | after): Raised view over a
      forest trail (blotchy blocks → one clean track, near and at ~300 m), MeadowSlope (the meadow trail at
      600-800 m was a row of pale dashes, now a continuous thin path), Trail (ground view, ferns keep to the edge).
      Cost: none measurable (interleaved field and trail walks: terrain GPU 0.52-0.54 / 0.86 ms either way,
      frame 16.3-16.5 / 16.0-16.3 ms).
- [x] Far objects at the far terrain height (2026-11-22, agents/terrain). Far tiles draw each cell as two
      triangles between its corner heights; far placements were on full-detail heights, |far - full| mean 2.9 m,
      median 1.6, p90 6.6, p99 19 m over the 2,209 fine cells (4-13 px at 500 m-1 km). `SnapToFarSurface`
      (TerrainManagerFarObjects.cpp) moves each far placement by the difference when its block is built; cells
      that draw their own terrain (far stamp cells) keep full detail. `ENGINE_FAR_OBJECT_SURFACE=0` turns it off.
      In the dense far forest the change barely shows in stills (Vista, MeadowSlope crops); cost none.
      Not done: loaded cells drawn at stride 2/4 (8-16 m grid, 256-384 m) keep their objects on full detail,
      and coarser far tile levels leave a residue within their screen error.
- [ ] Gameplay-relevant vegetation (bushes, tall fields, shrubs, rocks, logs, trees) with fixed reach (engagement
      range) and presence on every setting and detail budget; only cosmetic grass/flowers thin out. Solid far
      levels (no see-through). Includes the props-layer bushes that stop at ~25 m.
- [ ] Important-actor render class: never culled by distance, budget or settings; occluded only by solid
      conservative occluders; LOD floor; always casts shadows. A test pawn standing/prone at 50-800 m for captures.
- [ ] Terrain refined around important actors (detail islands in far tiles).
- [ ] Grass islands around actors at any distance (with the GPU blade generator) and one coverage function shared by
      AI perception, hit tests and rendering.
- [ ] Later: terrain virtual texture for far detail (paths, decals, macro colour per level).

**Open decisions after the parallel-agent session (2026-11-22; agents/perf, agents/look and agents/terrain
merged into main, worktrees removed; their comparison images are in `.local/agents/<name>/`):**
- Broadleaves at forest edges: placement is in the code but off (`ENGINE_BROADLEAF=all` or `=edge`); every
  variant roofed the ForestEdge camera, and that frame stayed identical across four rule changes, so first check
  the edge rule reaches that tree. Ideas: keep them to the belt's far side from paths and viewpoints, a
  crown-clearance test against trails and viewpoints, or a high-crowned second scanned species.
- Broadleaf LOD0 is ~10.8k triangles (twice a medium oak); only lone meadow trees use it.
- Next for terrain/visibility: cover vegetation with fixed reach and presence, the important-actor render class,
  a test pawn standing and prone in grass at 50-800 m (items 3-6 above).
- Scanned-plant shadows stay off (+0.7 ms shadow GPU, +1.3-2.5 ms forest walk, range-independent); needs a
  cheaper path (caster budget, per-tile caster culling or a shadow-only card level).
- GPU grass: blades lean further in gusts than the mesh grass; the runtime mesh-grass fallback keeps the far
  ring's blade-mode inner radius and does not restore tall_grass_clump.
- First run after a terrain cache-key change rebuilds every cell cache and peaked at 12-13.7 GB (normal runs
  ~3.5 GB); worth bounding.
- More cliff cells with the local-direction sculpt (~3 min each in parallel).

1. **GPU-driven vegetation renderer.** One renderer owns every streamed prefab placement (near cells and far
   blocks) in a GPU placement pool. Streaming a cell uploads or frees its range; no scene objects are created
   for batches. Each view (camera and each shadow cascade) runs one cull pass over the resident ranges: frustum,
   Hi-Z occlusion, per-placement level and dissolve (what InstanceCull.hlsl already does per batch), writing
   compacted instance lists and indirect arguments per mesh part, level and material. Drawing is a fixed set of
   indirect draws per view, however many cells are loaded.
   - Exists: the persistent placement pool (`PersistentInstancePool`), per-placement culling and level choice
     (`InstanceCull.hlsl`), GPU occlusion (on by default), one `ExecuteIndirect` per batch on DX12 with a fixed
     count of 1 (`DX12GraphicsContext.cpp`).
   - New: indirect draws whose count the GPU writes (DX12 `ExecuteIndirect` with a count buffer, Vulkan
     `vkCmdDrawIndexedIndirectCount`). DX11 has no count buffer: it keeps today's path, or draws a fixed
     maximum with zeroed arguments.
   - [x] Baseline (2026-11-22, `n1_*` runs; the machine was in its slow state, so compare interleaved runs,
         not these absolute frame times):

         | Run | Frame, >50 ms | Camera record (material / of it draw loop, footprints / tile prep / startup) | Shadow record (its scene prepare) | Scene prepare | Mesh objects / camera-visible items / draws |
         |---|---|---|---|---|---|
         | FieldWalk standing | 23.9, 0 | 9.7 (4.2 / 2.2, 1.7 / 1.2 / 1.5) | 3.5 (2.9) | 3.5 | 5,449 / 1,531 / 320 |
         | Forest walk | 22.4, 12 | 9.4 (3.8 / 2.0, 1.6 / 1.1 / 1.4) | 4.0 (2.8) | 4.1 | 5,576 / 1,428 / 327 |
         | Meadow walk | 25.9, 16 | 11.1 (4.4 / 2.2, 1.9 / 1.3 / 2.4) | 4.6 (3.2) | 4.6 | 5,635 / 1,518 / 314 |
         | Field walk | 26.0, 25 | 10.1 (3.9 / 2.0, 1.6 / 1.3 / 1.5) | 4.4 (3.0) | 4.8 | 5,780 / 1,406 / 316 |
         | Walk + turning | 26.1, 27 | 11.0 (4.2 / 2.2, 1.7 / 1.4 / 1.3) | 4.7 (3.0) | 4.9 | 5,588 / 1,514 / 345 |

         ~5,500 mesh objects and ~1,450 visible items become ~320 draws: the batching already merges parts
         across cells, so recording pays per object, not per draw. Far object blocks: 289.
   - Approach chosen (2026-11-22): shared part objects. Every prefab layer (cells, near and far blocks) goes
     through `TerrainObjectLayer::PreparePrefabInstances`; instead of one object per prefab part, level and
     block per layer, a registry keeps one object per distinct part configuration for the whole scene, and a
     layer attaches its placement blocks (world-space copies) to it. `Mesh` gains instance blocks: each block
     is its own pool range and tile, culled by its world bounds on the CPU and per placement on the GPU. One
     prepared draw per part instead of one per part per cell; streaming edits block lists, creates no objects.
   - [x] Shared prefab parts, behind `ENGINE_SHARED_PREFAB_PARTS=1` (2026-11-22; running scenes only, the
         editor keeps per-layer objects). `Components/Terrain/SharedPrefabParts` (registry on the Scene: part
         key = source geometry, material, level, shadow role, every mesh setting, project layer; blocks go live
         when their layer finishes the asset, as the objects were enabled; releasing a layer only queues the
         detach, applied once a frame in frame preparation), `Mesh::SetInstanceBlocks`, one tile per block in
         `SceneInstanceBatching` (each source registered once per pass, a stamp on the instance data), blocks
         culled against the view's side planes in the tile loop and in texture footprints (camera or any
         cascade). Cell layers, near blocks and far blocks all go through it (one code path).
         Mesh objects ~5,500 → ~790. Captures at ForestFloor, ForestEdge, MeadowSlope, Vista match the per-cell
         objects (`sp_cmp_*.png`; differences are wind and cloud timing).
   - [x] Found on the way: the shadow view requested texture footprints again although the camera's request
         covers every cascade. Off by default now (`ENGINE_SHADOW_FOOTPRINTS=1` brings it back): shadow record
         -0.5 ms with per-cell objects, -1.8 ms with shared parts; resident textures 770 / 773 MiB either way.
   - Walks, interleaved pairs (2026-11-22, both with the footprint fix; off / on):

     | Walk | Frame median | Frames > 50 ms | Update + prepare + camera + shadow record | GPU (largest) |
     |---|---|---|---|---|
     | Forest | 17.2-17.7 / 15.4-15.7 | 2-4 / 1 | 2.1+3.4+7.5+2.8 / 1.4+1.5+5.6+2.4 | opaque 8.0, shadow 2.9, grass 2.6 |
     | Field | 20.1-20.4 / 18.5-18.6 | 9-10 / 4-5 | 2.9+4.4+8.0+3.0 / 1.7+1.6+5.7+2.7 | grass 9.9, opaque 4.8 |
     | Turning | 21.2-22.1 / 16.3-17.4 | 9-10 / 6 | 2.9+4.5+8.9+3.4 / 1.7+1.7+6.1+2.8 | opaque 8.2, grass 3.3, shadow 2.9 |

     The main thread is no longer the limit: ~10.5-12 ms serial against ~15 ms (forest) and ~17.5 ms (field)
     of GPU. GPU time is next ("Next" 2: grass; the foliage depth pre-pass, leaves, shadows).
   - The parked DX12 hang can now show as a run that records nothing (camera record 0 from the warmup on,
     1.6 ms frames, a grey capture) instead of "device removed at buffer Map": 2 of ~16 runs either way,
     shared parts on or off. The perf runner treats it as a failed run and retries.
   - [x] On by default for running scenes (2026-11-22, `agents/perf`; `ENGINE_SHARED_PREFAB_PARTS=0` turns
         it off). Captured on/off at Backlit, Trail, Cliffs, Escarpment, ForestWall, FieldWalk (plus the four
         earlier): same content, differences are wind and clouds (`.local/perfagent/s0_cmp_*.png` in the perf
         worktree). DX11 and Vulkan render ForestFloor the same with it (DX11 frame 15.9 → 10.0 ms, Vulkan
         16.6 → 16.2; single stills). Stills, DX12, frame off → on: Backlit 16.5 → 13.9, Trail 15.9 → 14.7,
         Cliffs 14.7 → 14.4, Escarpment 17.1 → 12.8, ForestWall 17.4 → 16.8, FieldWalk 17.6 → 17.3.
         One shared-parts run at Backlit was killed by the runner's 12 GB watchdog (13.3 GB); two reruns
         peaked at 3.4 GB (shared parts runs peak 2.8-4.0 GB against 3.9-7.9 off). Not reproduced.
   - [ ] Editor Play mode not checked (cannot run headless): placement picking is per-layer only.
   - [ ] Shared parts give no CPU mesh occluders (`ENGINE_VISIBILITY_OCCLUSION_MESHES`, off by default).
   - [ ] Far blocks are still `TerrainObjectLayer` objects (289) built through the near/far hand-off; their
         parts are shared now, so the remaining items below are about streaming, not draw cost.
   - [ ] Pool ranges per cell and layer; placement ids kept for editor picking and removal at runtime.
   - [ ] Per-view cull over all ranges plus GPU-counted indirect draws, near cells first, behind a switch.
   - [ ] Shadow cascades through the same path.
   - [ ] Far blocks onto it; the near/far hand-off and `farObjectBlockCells` go away.
   - [ ] Remove the near-block and far-block batch objects (`TerrainManagerFarObjects.cpp`).
   - Expected: camera and shadow recording cost per distinct mesh part instead of per cell; cell crossings
     create no objects; detail distances can rise, since cost then follows visible placements only.
   - Look: same meshes, levels and dissolves. Check with captures at every viewpoint before and after (pixel
     diff), then the walks.
   - Editor: keeps per-cell objects at first (selection, gizmos); move it over once the game path is proven.
2. **Ground cover generated on the GPU, fine grass as blades.** A compute pass generates each tile's plants from
   the terrain heights, paint and density with the same deterministic hash, straight into the pool from 1: no
   CPU tile builds, no tile objects, no build backlog, tiles ready the frame they come into reach. Fine grass
   becomes procedural blade strips (Ghost of Tsushima, GodotGrass; "Research references (grass)"): per-blade
   height, width, bend and facing from Voronoi clumps, fewer and wider blades with distance so coverage holds.
   Shrubs, ferns, bushes and flowers stay meshes, placed the same way.
   - Removes rather than tunes: the 25-35 ms grass commit at cell crossings, late tiles at speed, and the
     tile-size trade-offs worked around on 2026-11-21 (level per plant, the 128-per-tile cap, reach per class).
   - Look: what the meadow references need: fine thin blades as one mass, tall grass fields with seed heads,
     cover to 100 m and beyond ("Fields of tall grass" below; expectations table, "Grass cover").
   - Budget: grass is 6.6 ms of GPU on FieldWalk now. Target 4 ms or less at 1080p for denser and finer cover
     than today, measured on FieldWalk and at MeadowSlope.
   - [x] Where grass time goes (2026-11-22, `agents/perf`, `ENGINE_GPU_WORKLOAD_COUNTERS=1`; the grass stage's
         counters are now in frames.jsonl as `gpu_grass_vs_invocations` / `_primitives` / `_ps_invocations`):
         grass is **vertex-bound**. FieldWalk 9.5 ms = 15.1 M vertex invocations, 5.0 M triangles, only 15.8 M
         pixel invocations (~7.6 per pixel) for 221 k plants; MeadowSlope 8.1 ms = 9.1 M vertices, 21 M pixels.
         The far ring alone (134 k tufts, 54 triangles each at every level) was 3.3 ms and half the vertices
         at FieldWalk (ring off: 6.8 ms, 7.5 M vertices, pixels unchanged); tall fields are 2.2 ms.
   - [x] Far ring with real levels (Tsushima's fewer, wider blades with distance): `far_grass_tuft` LOD1 keeps
         45 % of its blades at 1.9x width, LOD2 22 % at 3.4x, one segment each (54 → 7 → 6 triangles;
         `tools/generate_meadow_grass.py`, LOD0 unchanged), at 45 and 90 m, chosen per plant
         (`MeadowGrass::ApplyFar`; `ENGINE_FAR_GRASS_LODS=0` keeps the full tuft). FieldWalk grass 9.5 → 8.0
         ms (15.1 → 8.4 M vertices); field walk 16.6-16.8 → 15.1-15.2 ms (grass 9.4 → 8.1, interleaved);
         MeadowSlope 8.1 → 7.9, ForestEdge and Vista unchanged. Look: the far meadow band past ~45 m reads a
         little smoother (`.local/perfagent/s3a_crop_*.png` in the perf worktree).
   - [x] GPU grass blades, on by default since 2026-11-22 (`ENGINE_GPU_GRASS=0` keeps the mesh grass;
         `agents/perf`). The meadow's fine grass and tall fields are generated on the GPU every frame; shrubs, accents, bushes and the far ring stay
         meshes. How: each meadow `GrassField` in blade mode (`gpuBlades`, set by LandscapeDriver) hands its
         cell's height and grass paint grids to the scene's `GrassBlades` (Assets/Grass), which packs every
         loaded cell into one buffer and owns one carrier object (a 4-segment blade template, one dummy
         placement, `Mesh::gpuBladeField`). In the camera view the carrier's batch runs
         `Shaders/GrassBlades/GrassBlades.hlsl` instead of the placement cull: one group per 2 m patch of a
         camera-centred grid (frustum and occlusion-buffer culled), blades from a hash of patch and index,
         grass paint, the same tall-field noise as `GrassField::TallField` in cell-local coordinates (the
         fields line up with the far ring's tall tufts), Voronoi clumps (height, lean, hue; fountains: upright
         at the heart, arching toward the rim), rank thinning past the keep distance ((7 / d)^2, survivors
         widened by 1 / sqrt(share), the last kept blade grows in) and a shrink fade over the last 10 m of the
         60 m reach. The material shader `Assets/Shaders/GrassBlades.hlsl` builds each blade (bent quadratic
         curve, the generator's leaf outline and palettes, edge-on widening in view space, the grass minimum
         pixel width, the shared grass wind) and shades it with the meadow's `PSGrassMain`. The far ring fades
         in over the blades' last 10 m instead of from 30 m. No CPU tile builds for the fine grass.
         Numbers (interleaved field walks, off / on): frame 16.3-16.7 / 12.2-12.7 ms, grass GPU 8.6-8.9 /
         4.5-4.6 ms, frames > 50 ms 3-6 / 0-2. Stills: FieldWalk grass 9.0 → 4.8 ms (vertices 8.5 M → 2.7 M),
         MeadowSlope 8.4 → 4.3, ForestEdge 6.1 → 4.3. Per-frame grass CPU at cell crossings: p99 6.6-7.2 →
         3.2-3.3 ms (the 25-35 ms commit did not show in either build).
         Look (1440p tiles `.local/foliage/blhi3_on_*` against `blhi_off_*` in the perf worktree): MeadowSlope
         and ForestEdge close over the soil like the mesh grass; FieldWalk reads a little lighter and more even
         (the mesh meadow has darker pockets between tussocks).
         Found on the way: until a material shader has compiled, its draw falls back to the base pipeline; a
         carrier drawn that way read blade records as pool indices and hung the GPU (root SRVs have no bounds
         checks). Carriers now draw only once their shader is resolved.
   - [x] Blades finished and made the default (2026-11-22, `agents/perf`).
         Grass stage at FieldWalk split (debug switch `ENGINE_GPU_GRASS_DEBUG`): leftover meshes 2.9 ms, blade
         pixels ~1.6, blade vertices ~0.15, the generation pass ~0.05. Most of the rest was not grass at all:
         every instanced batch ran reset, barrier, cull, barrier right before its draw (two GPU drains per
         batch, ~190 per frame). The camera view now records all its culls first and runs them in one
         compute pass with two barriers, then replays the draws (`ENGINE_HOIST_CULLS=0` restores the old
         order): FieldWalk 11.9 → 10.9 ms, grass 4.2 → 2.6, foliage depth 2.2 → 0.25 (cull work shows as
         ~1.9 ms opaque other); ForestFloor 13.9 → 11.4; Vulkan FieldWalk 12.4 → 9.7; DX11 10.1-10.3 →
         9.9-10.0. Blade mode also drops the meadow's remaining tall_grass_clump shrub.
         Pockets: a 2.4 m tussock Voronoi; toward the border of two tussocks short blades thin (90 %), lose
         up to 70 % of height and darken toward the roots, so FieldWalk reads as tussocks with shadowed
         hollows like the mesh grass (1440p `.local/foliage/pk2_*`, `pk3_*`); no measurable cost.
         Backends: Vulkan had no SPIR-V for the generation shader (no grass in blade mode); now built.
         DX11 draws blades through its indirect path. A renderer that cannot draw blades marks the field
         unavailable and the meadow rebuilds its mesh grass (`ENGINE_GPU_GRASS_DEBUG=4` simulates it).
         Default on, interleaved (blades / mesh): stills FieldWalk 10.7-10.9 / 14.0-15.0 ms, MeadowSlope
         12.0-12.5 / 15.4-15.5, ForestEdge 11.9-12.1 / 13.7-13.8; walks field 11.4-11.7 / 14.6-14.7 (p99 39 /
         48-53, grass 2.4 / 6.6-6.9), meadow 13.2-13.3 / 13.7-13.8, forest edge turning 13.2 / 13.5-14.1.
   - [ ] Blades lean further over in gusts than the mesh grass (ForestEdge, FieldWalk crops): the wind's
         height weighting differs between the blade shader and the mesh grass.
3. **Retained render scene for everything else.** `PrepareFrameItems` copies every item each frame (2.6 ms for
   9.1k items at the last count) and the render packet retains two buffers per item (18k shared-pointer copies
   and `RetainBuffer` calls on the render thread). Keep object data in a GPU buffer updated from a dirty list
   (transform, material and enable changes) and rebuild draw lists only when the scene's structure changes.
   After 1 and 2 this covers props, terrain cells and plain scene objects.
4. **Views recorded in parallel**, only if 1-3 leave camera plus shadow recording above ~4 ms: shadow cascades
   and camera on separate workers. Needs the per-view state in `SceneRenderer::Render` split first (light and
   cluster buffers, pass buffers, instance pool and batches, material cache, timing structs). Shorter shadow
   ranges are not a substitute: 2/3 of authored made no measurable difference (2026-11-21).
5. **Far forest that reads as forest, and is ready at once.**
   - [ ] Far placements cached per cell on disk, or generated on the GPU from the forest mask, so a launch
         does not regenerate 8 km of forest (~14.5 s until fully ready now).
   - [ ] Octahedral impostors with normal, depth and occlusion atlases, lit with the near trees' leaf model and
         drawn through 1 as the last level: crown texture and shading to ~2 km and the blue shift with distance
         (expectations table, "Far forest"; reference gap (c)).
   - [ ] Measure when the near world is playable, separately from the far fill.
6. **Level chosen by size on screen.** With 1 in place, every placement picks its level from its projected size
   against a pixel-error target (`placementCoverageLod`, `Core/Rendering/ScreenCoverage.h`), the same for
   trees, props and ground cover. The detail budget becomes an overload guard instead of the policy; as the
   policy it has sat at its floor (0.64 of authored distances) on every walk, which is why trees and props
   switch close to the camera.
7. **Depth before shading for all opaque geometry.** Extend the foliage pre-pass (which saves 4-11 ms of leaf
   shading, 2026-11-21) to bark, props and terrain. That makes contact shadows and screen-space canopy occlusion
   possible: dappled light, darker forest interiors (Foliage Phase 3).

**Look and content, on top of 1-7** (each measured on the walks):
- [ ] **Fields of tall grass** (asked 2026-11-21; the blades need 2). References:
      `.local/reference-images/chlorophyll-video` (`02_meadow_flowers_ridge_haze_0m12`,
      `04_meadow_lone_tree_cliff_escarpment`, `01_forest_edge_meadow_slope_0m04`; full-size PNGs in
      `png/`). What they show against ours: knee-to-waist-high grass as one continuous mass with no
      soil between plants; fine thin blades that arch and lean with the wind, not broad-bladed
      clumps; seed heads and stems standing above the mass and breaking the skyline at the slope's
      crest; small white and yellow flowers scattered through it; darker in the depth of the mass and
      lighter at the tips; the cover carries on to the far meadow as a smooth lighter green.
      - [x] First pass 2026-11-22 (ahead of 2, on today's mesh grass): `meadow_tall_grass`
            (`tools/generate_meadow_grass.py`): a metre-wide patch of 7 crowns x ~50 fine blades,
            0.7-1.05 m, mostly upright, dark roots and bright tips (the references' two tones), a few
            wispy seed stems; 1,775 / 976 / 304 triangles; half wind response (full response laid the
            long blades flat). `GrassField` tall fields (`tallFieldDensity`, `tallFieldSize`,
            `tallFieldCoverage`, `shortUnderTallShare`): terrain-space noise; inside a field tall patches
            grow at up to 1.8/m² and the short sward thins to 30 %. Landscape: fields over ~55 % of the
            meadow, 28 m noise, `groundRootShade` 0.85 (`ENGINE_LANDSCAPE_TALL_FIELDS=0` for the old
            meadow). FieldWalk: the foreground is a closed tall mass (`.local/terrain-stamps/tg_zoom3.png`);
            grass GPU 7.5 → 7.8 ms, frame 19.4 → 19.1 (single runs).
      - [ ] Past ~10-30 m it still reads as short grass over dark ground, and it flattens into the far
            ring (0.5 m tufts) beyond: the coarse levels, distance thinning and the far ring need to keep
            the tall mass's height and colour.
      - [ ] Colour against the references: ours is a muted olive; theirs is a deep shaded green under
            bright yellow-green lit blades, with many 2-4 px white and yellow flower specks.
      - [ ] Density that closes over the ground at 1080p from eye height, within the grass budget:
            measure GPU grass time standing at MeadowSlope and the meadow walk (18.6-19.1 ms now).
      - [x] Root-to-tip shading (2026-11-22, agents/look): PSGrassMain darkens the lower three quarters of
            a blade (ambient floor 0.6 -> 0.4, sun 0.82 -> 0.45, backlight too); the mass has dark depths.
            Wind lean not done.
      - [x] Far ring in tall fields (2026-11-22): `far_tall_tuft` (0.75-1.05 m, 72 / 10 / 6 triangles with
            the perf branch's far LOD scheme) at 0.9 per m² inside the meadow's tall fields, same field noise
            (`ENGINE_LANDSCAPE_FAR_TALL=0` off). The far meadow keeps a tufted top instead of a lawn band.
      - Cost of the agents/look changes on top of agents/perf (FieldWalk walk, interleaved): pair 1 18.1 vs
        15.9 ms (camera and shadow record both up, a disturbed run), pair 2 15.8 vs 15.8 ms; grass GPU 8.63 /
        8.33 vs 8.48 / 8.58 ms.
      - [ ] Colour is closer (deeper green, darker depths) but still lighter and yellower than the
            references at the tips; flower specks: scanned celandine/dandelion accents added (WIP, not judged).
      - [ ] Compare unscaled 1440p tiles (`hires.sh`) against the references at MeadowSlope and
            ForestEdge.
- [x] **Bushes pop up from the ground while moving** (user 2026-11-22). Cause: the meadow's bushes were
      distance-thinned like the fine grass, so from ~16 m (0.2 of the 80 m reach) they shrank in and out by
      rank and visibly grew out of the ground near the camera. Fixed 2026-11-22: `Mesh::InstanceRange::thinned`
      (false for the grass field's non-fine species) sets padding bit 1024 / `selection.w` bit 26, and
      InstanceCull.hlsl and Object.hlsl skip the thinning for those ranges. Bushes are now full size to 56 m.
      Cost: none measurable (FieldWalk walk 19.3 ms median, grass 9.52 ms, against 19.5 / 9.57 before).
      - [ ] Still open: the end-of-reach shrink over 56-80 m. Watch for it; if it shows, use a longer reach
            with a dissolve, or move large plants to the props layer with LOD chains.
- [ ] **Bush and small-plant materials look low quality next to the trees, leaves and grass** (user
      2026-11-22): the meadow bushes, shrubs and ferns were generated (vertex-coloured cards, no textures).
      - [x] Bushes, low shrubs, nettles and saplings are scanned now (2026-11-22). `tools/scan_bush_cards.py`
            renders the Poly Haven scans (shrub_04, shrub_02, nettle_plant; CC0) into one alpha-masked card
            atlas (`ScanBushCards_diff.ktx2`, leaves graded toward the meadow green, stems toward bark), then
            builds plants from crossed card pairs with outward volume normals, both sides as separate
            triangles: `scan_bush` 1.5 m (720 / 360 / 144 triangles), `scan_bush_tall` 2.3 m (960 / 480 /
            192), `scan_shrub`, `scan_nettle`, `scan_sapling`; the old bush was ~33k triangles. A sprig scan
            cannot be simplified below ~1,200-1,900 triangles (every leaf is its own UV island), so whole
            scans were not an option. They draw from their own field, layer "Procedural meadow bushes"
            (`MeadowGrass::ApplyBushes`, material `Materials/Grass/scan_bush.material`) on meadow and forest
            soil, 80 m reach; LandscapeDriver drops the generated bushes, bramble and shrub from the meadow
            and forest floor fields (`ENGINE_SCAN_BUSHES=0` restores them). PSGrassMain leans textured
            plants' normals 0.15 toward the sky instead of 0.55, so they keep a shaded side.
            Cost: FieldWalk walk, interleaved A/B, 19.8 / 20.1 ms median against 20.1 / 19.8 with the
            generated bushes; grass GPU 9.43 / 9.22 against 9.81 / 9.70 ms.
      - [x] Ferns are scanned now (2026-11-22). fern_02's texture is five flat fronds; they fill the bottom
            half of the atlas (now 2048x4096), and each fern is a rosette of arching, drooping frond strips:
            `scan_fern` ~1.2 m across (288 / 128 / 40 triangles) in the bush field's thickets, and
            `scan_fern_large` ~1.8 m (384 / 160 / 48) with `scan_fern` in a new layer "Procedural forest
            ferns" (`MeadowGrass::ApplyForestFerns`: forest soil, 0.8 per m² in 10 m thickets covering 45 %,
            50 m reach). The meadow field keeps only its tall grass clumps as shrubs; the forest floor field
            no longer draws `forest_fern`. Both scanned fields now pick detail per plant.
            Cost: not resolved. The machine drifted slower through the session (the generated build alone
            went from 17.4 to 23.2 ms on the same forest walk); the cleanest interleaved pairs give +0.6 to
            +1.0 ms frame and +0.5 ms command record in the forest (one more field's tile draws), grass GPU
            +0.1 ms, nothing on the meadow. Re-measure on a fresh machine. 32 m fern tiles (fewer draws)
            were tried: not all built 60 frames in, and they would arrive a whole tile at a time.
      - [ ] Still generated: herbs (`forest_herb`), flowers, litter, seedlings, ground ivy, tall grass clumps.
            2026-11-22: scanned celandine, dandelion and weed rosettes and periwinkle (third atlas band,
            `tools/scan_bush_cards.py`; periwinkle recoloured to lesser periwinkle's blue-violet). Kept on the
            forest floor (fern layer accents). Not on the meadow: the 10-20 cm rosettes vanish under the
            knee-high grass (FieldWalk), so the meadow keeps its generated flowers on stems. Cost: FieldWalk walk
            16.2 / 16.1 ms against 16.2 / 16.2 on agents/perf (with the meadow accents still in).
      - [ ] grass_test and foliage_test have no bush layer and still draw the generated bushes.
      - [ ] Bushes cast no shadows (the field has `castShadows` off); try it now that they are cheap.
      - [ ] Not judged: the bushes from a raised view, against the light, and close up while walking.
- [ ] Forest edge variety: at least three broadleaf silhouettes, height variety, scanned shrubs (expectations
      table, "Tree variety"; Foliage Phase 7).
      - Looked at 2026-11-22 (agents/look), not built: Poly Haven's broadleaf trees (CC0) are `tree_small_02`
        (small tree, slender curved trunk, airy canopy, 4.3 m; 2.06M triangles), `jacaranda_tree` (18 m, broad
        canopy; 3.86M triangles; subtropical, but out of flower it reads as a broad deciduous crown) and the
        `island_tree_01-03` coastal windswept shapes (skipped). Both candidates are millions of alpha leaf
        cards with separate trunk / branch / leaf materials (per-material texture sets in files.json, which
        `fetch_polyhaven_props.py` does not handle), so PropCook cannot simplify them to a game chain.
      - Built 2026-11-22 (agents/look): `tools/scan_tree_cards.py` bakes `tree_small_02` into two prefabs
        (`Prefabs/Woodland/Broadleaf/Broadleaf_slender_01`, `_upright_01`; the upright one is the same tree
        stretched 0.72 x 1.35 about the trunk, leaf clusters moved rigidly, so both share one card atlas and the
        bark/leaf materials). Trunk and branches are PropCook chains (trunk 4000/1200/460; branches
        6200/1800/300, twig pieces under 0.25/0.4/0.8 m dropped per level, since PropCook keeps the open ends
        of every twig tube and cannot simplify them away). The crown is a billboard cloud: 160 k-means
        clusters of the 30k leaves, two crossed cards each (each leaf goes to the card it faces most, so
        crossing does not double the density), 320 cards / 640 triangles in one 4096 atlas (523 px/m, graded
        toward the meadow greens, depth-shaded), used by every level; cook_foliage_lighting bends their
        normals. The impostor (6 views + top) is baked from the full 1.94M-triangle crown with a vectorised
        point-splat rasterizer (the per-triangle one would take hours), tinted 0.58 to match the cooked cards.
        Placement (LandscapeDriver, `BroadleafSpecies`, `ENGINE_BROADLEAF=0` for A/B): half of the lone
        meadow trees in the open (interior < 0.02 at the tree and 15 m around it), 7-11 m, both variants.
        Silhouettes in edge views: oak domes behind, the slender leaning tree and the upright one in front.
        Cost (FieldWalk, 400 frames, interleaved with `ENGINE_BROADLEAF=0`, same binary): lone-only median
        22.5/22.9 ms vs 23.6/21.6 off (no measurable change; this round ran ~6 ms slower overall than the
        first), command recording +1.2 ms; with the edge placement on too (first round) 16.8/16.5 vs
        16.0/16.4 ms, command recording +0.9 ms. Triangles: LOD0 4000 trunk + 6200 branches + 640 cards;
        LOD1 1200 + 1800 + 640; LOD2 460 + 300 + 640; impostor 38.
      - Edge-belt placement is written but off by default (`ENGINE_BROADLEAF=all` or `=edge`): with any share
        of broadleaves in the belt (tried nearer the meadow, forest > 0.22, > 0.45, > 0.65; 30-45%; scale
        1.2-2.5) one stood over the ForestEdge camera, whose whole view looked into its crown (captures
        `.local/foliage/bl2..bla_ForestEdge_full.png` on agents/look). The belt is dense and these crowns
        start low and spread to one side; the small oaks there are narrow. Oddly the frame stayed
        pixel-identical across the last four rule changes (scale 1.5 -> 2.0 included), while
        `ENGINE_BROADLEAF=lone` cleared it, so check first that the edge rule reaches that tree at all.
        Ideas: only on the far side of the belt from viewpoints/paths; a crown-clearance test against the
        trail and viewpoint positions; a third, high-crowned scan instead of the stretched variant.
      - Lessons: the slender tree's crown starts a third of the way up and leans to one side, so near a
        walker it roofs over the view; the first impostor was 1.7x too bright beside the cooked cards.
      - [ ] Only one scanned broadleaf species exists (CC0, temperate): a second scan (lime, hornbeam, birch)
        would give a real third silhouette instead of the stretched variant.
      - [ ] Close-up look not judged (leaf cards at 523 px/m are soft under a crown at 2 m).
- [ ] **Cliffs as sculpted terrain** (asked 2026-11-21: cliffs that are the terrain itself, with the scans' shape
      and look, no scan objects, smooth from ground to rock). The stamp route before it (per-piece surface
      patches, stages 1-5, far stamps, macro colour and far-grid patches) is in todo_history.md (2026-11-22):
      seen from above it was overlapping shells cutting through each other, with smeared textures
      (`.local/terrain-stamps/top_cliff.png`).
      How it works now: `tools/cliff_sculpt.py` merges the ground and every scan piece reaching a cell into one
      surface. A 0.25 m grid on 6 m-smoothed ground marches each point along its normal to the outermost
      surface (ground heights plus a KD-tree of points sampled on the scans), smooths out turned-over quads,
      simplifies to three levels (`stamp_simplify.simplify_levels(locked=)`) and writes `.sculpt` v2 (cell
      space; per-vertex scan tint with cavity darkening and a rock weight; the rock tile's tiling). Borders
      with grid cells keep the grid's 4 m points; borders between sculpted cells are computed from world
      positions alone (12 m margin), so both sides hold the same points. Rock: a seamless tile cut from the
      Mountainside_01 stamp images (`tools/cliff_rock_texture.py`, 13.8 m repeat) projected from three sides
      in Object.hlsl and tinted per vertex. Engine: `TerrainSculpt` (`TerrainStamp.h`), carried as a stamp
      instance (`LandscapeDriver` `LoadSculptedCells`); the sculpt replaces the cell's grid triangles
      (`Terrain::AppendStampGeometry`) and is its collider; `Mesh::occluderIndices` gives the CPU occlusion
      buffer the coarsest level; `ENGINE_SCULPTS=0` keeps the stamps (A/B).
      Done 2026-11-22 on the test escarpment, cells -1..1,-6 (`python tools/cliff_sculpt.py -1,-6 0,-6 1,-6
      --only X,Z`, ~16 min a cell in parallel): one continuous surface from above and the side, no shards or
      gaps (`esc_top_vs.png`, `esc_side_vs.png`); shared borders identical at all levels (513 points, 0 m);
      border streaks gone (`seam0_v2.png`). Interleaved A/B, side view 1920x1080: frame 17.1-18.1 ms against
      18.5-19.5 with stamps, terrain GPU 2.1 against 3.0-3.3 ms.
      - [x] Re-sculpt 2026-11-22: folds untangled in 3D after the medians (946-2,387 quads per cell remain
            turned over, ~0.5 %; likely overhangs a single sheet cannot hold) and cavity darkening doubled
            (radius 2.5 m, 0.9 per metre). Borders still identical (513 points, tint difference 0). The bright
            slivers are gone (`esc_side_v3.png`). The "hole" at the x=0 border (`hole_zoom_v3.png`) is a real
            crevice: camera rays through it hit front-facing triangles 5-14 m deeper than the rock around it,
            in shadow.
      - [ ] Shape: from the side it is one even wall (`esc_side_v2.png`): the outermost surface fills the
            gaps between pieces that gave the stamps their blocky look. Next: keep the recess between pieces
            that only touch; march along the local rock direction instead of one slope-wide one.
            2026-11-22: a second march along the scans' smoothed normals (LOCAL_MARCH) re-sculpted cells
            -1..1,-6 (518 s for all three in parallel). Close to the face the vertical smears and stretched
            crevice are gone and the blocks vary more; from the ground (~110 m) more relief and fewer vertical
            stripes (`.local/foliage/cliff_old_new.png` in agents/look: old | new). Quads left turned over:
            2,420 / 6,903 / 5,606 per cell (946-2,387 before), not visible as holes in these views.
      - [ ] Rock tile: only ~64 % of the cut is clean, flat-on rock; faint hatching from above. Find a
            cleaner source.
      - [ ] Whole map: 300 cliff cells at ~16 min each (the pure-Python simplifier is most of it): speed it
            up or run overnight. Then retire the stamps, stamp proxies, stamp images and far stamps' stamp
            path.
      - [x] Far: within `farStampRadius` (8 cells) a cliff or sculpted cell draws as its own terrain at
            stride 4 (far stamp cells, see the performance section in Status); checked at 450 m.
      - [ ] Far stamp cell cost not re-measured since they became whole cells (was +0.8 ms at the 450 m
            view as rock over the far tile). Past 8 cells cliffs are only the macro colour and normals.
      - [ ] Moving check of the 384 m hand-off: drives froze three times around frame 410 with process
            memory past 18 GB (GPU hang suspected; static captures are fine). No drives until the user
            okays them.
      - [ ] A ground view of the escarpment (reference frame 04): a clearing in front of the face, which the
            forest mask cannot express yet.
      - [ ] Bare sheer terrain outside cliff cells shows the rock texture as vertical stripes (cause not
            checked).
- [ ] Radius 3 leftovers: far-terrain colour at the foot of distant cliffs (`qf5.png` against `qf3c.png`); one
      rim tree's silhouette as a far placement (`qc5.png`, `qc3.png`).
- [ ] Variants from the earlier reference sets: still water with reeds and lily pads, haze between trunks,
      layered ridge haze, streams (Landscape look 7; Foliage Phases 6 and 7).

**Tuning, after 1-3** (settings, not structure; revisit once the frame has slack):
- [ ] Detail budget numbers (6 ms GPU, 4 ms CPU) against a 16.7 ms frame; superseded as the policy by 6.
- [ ] Ground cover reach (50 m, shrubs 80 m), fade bands, shadow ranges, exposure metering at forest edges.

## Did not work / reverted

- Foliage depth pre-pass off (2026-11-21, to free GPU time): leaves went 1.0 → 4.8 ms on the field walk and
  1.5 → 13 ms in the forest (frame 19.1 → 20.9 and 18.8 → 25.3 ms). It pays for itself many times; kept on.
- Shorter shadow ranges for trees, props and undergrowth (2/3 of authored): no measurable change; see
  "Next" 4.
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
- A shrub belt of the pack's bushes along forest edges: under 2 m, cartoon leaves, and not drawn past ~25 m;
  removed (2026-11-20).
- Generated spire, overhang and ledge shapes as cliff filler: stood through the canopy as rows of columns;
  no longer placed.

## Not done (index; details in the sections below)

- **Look:** brightness, colour and sky rows pass at the old MeadowSlope pose (not re-measured at the new one);
  grass reach, far-forest texture and tree variety do not; the forest edge is closed and the terrain has
  hills and an escarpment as of 2026-11-20 (expectations table, "Terrain and forest edge pass").
- **Frame rate:** 60 fps standing or moving (Status above).
- **World:** streams and rivers; vines on trees; more than six terrain paint layers (mud, moss, litter, scree);
  sculpted cliffs map-wide (one escarpment so far, "Cliffs as sculpted terrain"); scanned shrubs; water with
  reeds and lily pads; far-terrain silhouettes (128 m polygons).
- **Light and air:** bounce lighting; contact shadows (blocked by the forward renderer); soft shadow penumbra; froxel
  fog; depth of field; leaf gloss and wetness; SH sky ambient for grass and meadow plants; cloud shadows matched to
  the volumetric clouds; long-range shadow on non-terrain objects.
- **Trees:** oak-pack impostors not re-baked with the leaf lighting; octahedral impostors; trimmed leaf cards;
  standalone (non-instanced) prefab trees take 64 ms of shadow submission; more tree models without the per-asset
  CPU cost.
- **Grass:** fade band still visible at ~25-35 m; far palette not matched to the cover colour; LOD distances not
  re-evaluated after thinning; clumping; tiles built ahead of the camera; bushes limited to the grass radius; pale
  ground between blades in the test meadow; blade-mottling cost not measured.
- **Antialiasing:** Vulkan has no MSAA and no TAA; temporal upscaling is DX12 only (game renderer).
- **Editor:** terrain generation window; asset-generator tool not exercised in a running editor; AO in editor
  viewports; optional 3D LUT for grading.
- **Not measured:** cost of the 9-tap AO lookup; cliff pieces per cell; the detail model against measured GPU cost
  while turning.

## To investigate

- [ ] Scanned props raised draw counts (ForestFloor 292 → 367 submitted, 1371 → 1825 candidates) and the
      ground views now measure ~31 ms (CPU record ~19 ms; single runs): the per-asset CPU cost again.
      CPU round 3 brought the same view back to 24.0-25.0 ms; the per-candidate cost itself is unchanged.
- [ ] One of fourteen landscape runs on 2026-11-19 ended during loading with zero frames and no error text (the
      window-closed path in `GameApplication`, exit code 2). Not reproduced in the seven runs after it. It came one
      build after the static-item reuse test moved onto workers, so watch for it.
- [ ] Capture runs that render nothing: 4 of ~50 runs on 2026-11-20 produced a uniform grey image with
      `camera_record_ms` 0, `scene_replay_ms` 0 and a 6-9 ms frame, yet reported "valid" (twice Vista, once
      ForestFloor, once the first no-cookie Vista). The benchmark should fail a run whose camera view never
      recorded; until then check `camera_record_ms` before trusting a capture.
- [ ] Bushes in the props layer (single LOD, no LOD band) stop being drawn ~25 m from the camera on the
      landscape, at detail scale 0.6 and 1. Not the layer's `viewDistance` (220 m) and not placement thinning
      (grass only); cause not found. It is why the undergrowth bushes never show at a forest edge from the
      meadow.
- [ ] Under the canopy, trunks and stumps in full shade are lit a bright blue-grey by sky ambient (no
      occlusion from the crowns above), so the forest interior shows as a picket row from the meadow
      (`h1_wall_pickets.png`).
- [ ] Far tree stand-ins on the mountains render near-black navy; pale ellipses on meadow crests at
      150-300 m (cause guessed, see the full-resolution look pass).
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

### Research references (foliage performance, web search 2026-11-22)

Details, per-technique breakdown and full reference list: [foliage_performance_research.md](foliage_performance_research.md).

What other engines do, against where our GPU time goes now (after shared prefab parts: forest walk GPU
~15 ms = opaque 8.0 of which foliage depth pre-pass 2.5-4.4, shadows 2.9, grass 2.6; field walk ~17.5 ms =
grass 9.9, opaque 4.8). Most sources are talk abstracts, slides or press summaries; numbers are theirs, unverified.

| Engine / game | What they do | Bears on |
|---|---|---|
| **Chlorophyll Engine** (Toni Macaroni; the reference video; wccftech 2026-11-04, hwupgrade.it) | Handwritten engine, ~7 months. Clustered LODs (Nanite-like virtual geometry); **voxels instead of impostors** for distant vegetation; per-instance data dropped for the farthest proxies; screen-space + voxel GI, no hardware RT. Developer's figures: ~130 fps average, ~80 in dense forest, ~200 aerial, RTX 3080 at 1440p. No culling details published. | Far forest (Next 5), tree LODs |
| **UE 5.7 Nanite Foliage** (experimental; Witcher 4 demo, 60 fps on PS5) | Three parts: Nanite **voxels** (near-pixel aggregates past a distance, keep animation and material), **assemblies** (reusable parts, the memory/disk saving that makes a forest fit), GPU skinning for wind. Users report holes when meshes go voxel, and advise cached shadows with it. | Far forest, wind, shadows |
| **Ghost of Tsushima** (GDC 2021, Wohllaib) | Grass as individual procedural blades generated on the GPU, Voronoi clumps, per-blade shape and wind, blades facing away from the camera widened in view space. ~83k blades on screen in ~2.5 ms (secondary source). | Grass 9.9 ms (Next 2) |
| **Ghost of Yōtei** (PlayStation Blog tech deep dive, 2025) | Instance management moved to the GPU and aggressive culling before the G-buffer; reported to double the amount of grass and objects. | Next 1 and 2 |
| **Horizon Zero Dawn** (GDC 2017, van Muijden) | All vegetation placed procedurally on the GPU from 2D world maps (~4 MB/km²): 500+ asset types, 100k+ objects, ~250 µs of GPU per frame; deterministic, locally stable. | Next 2 (placement), far cells (Next 5) |
| **Horizon Forbidden West** (GDC 2022, McLaren) | **Deferred texturing / visibility buffer for foliage and alpha-tested geometry**: a cheap visibility pass, then material analysis and shading in compute, overlapped with G-buffer and shadow rendering. | Foliage depth pre-pass, leaf overdraw |
| **Alan Wake 2** (Northlight; GDC/REAC 2024) | Mesh shaders with meshlet-level culling; vegetation bent by ~300k bones a frame skinned on the GPU. | Leaf/tree cost, wind |
| **AMD work graphs + mesh nodes** (GPUOpen, GDC 2024) | Grass patches generated by one mesh-shader workgroup each (16x16 blocks); trees generated on the GPU instead of stored (tens of GiB → KiB). AMD reports ExecuteIndirect 1.64x slower than work graphs in their demo (RX 7900 XTX, pre-release driver). | Later; DX12-only, young |
| **Haar & Aaltonen, AC Unity** (SIGGRAPH 2015) | Cluster culling, any number of meshes per draw, two-phase Hi-Z; ~100 + 50 µs for the depth pyramid on PS4. | GPU occlusion (have it) |
| **Fortnite** (octahedral impostors; VSM tech blog) | Impostors at 2048 colour/alpha + 1024 normal/depth per tree. VSM caching breaks on swaying trees and a moving sun; they cache animated casters in a base pose. | Far forest, shadows |
| **Insomniac / CRYENGINE / Cyberpunk 2077** (shadow caching) | Scrolling cached cascades: far cascades kept and scrolled, only newly exposed edges re-rendered; near cascades and moving things drawn each frame. | Shadows 2.9 ms |

**Options, ordered by expected gain on our measured costs:**
1. **Grass on the GPU as blades** (Next 2; Tsushima, AMD, Horizon placement): the field walk's 9.9 ms of
   grass. Blades generated per tile from a hash (no meshes, no CPU tiles), fewer and wider blades with
   distance, LOD chosen in the cull. Target 4 ms or less.
2. **Foliage depth pre-pass and leaf overdraw** (2.5-4.4 ms in the forest): measure first (pre-pass on/off
   per pose, overdraw view). Cheap steps: tighter leaf cards (Persson), front-to-back order within a batch.
   The structural fix is Horizon Forbidden West's visibility buffer for alpha-tested foliage; Nanite
   Foliage avoids alpha by using real leaf geometry.
   - [x] Measured (2026-11-22, `agents/perf`, shared parts on). The pre-pass pays for itself many times:
         without it (`ENGINE_FOLIAGE_DEPTH_PREPASS=0`) leaf shading is 12-15 ms (forest walk 15.0-15.1 →
         23.5-23.9 ms). With it: depth 3.4-4.0 ms + leaves 1.3-2.0 ms. The pre-pass is fragment-bound:
         `ENGINE_GPU_WORKLOAD_COUNTERS=1` shows 72 M pixel-shader invocations at ForestFloor (~35 per pixel at
         1080p) and 29-30 M at Vista, against 3.3-3.7 M triangles.
   - [x] Tried and removed (no gain): terrain, bark, rocks and grass before the pre-pass (pre-pass -0.3-0.4 ms
         but bark +0.6 and Vista terrain +0.7, walk unchanged at 15.1-15.2 ms); camera-view tiles sorted
         near to far (pixel-shader invocations unchanged: the pre-pass shader discards and writes depth, so
         early depth rejection does not happen whatever the order).
   - [ ] What would cut it: fewer rasterised leaf fragments. The big oak's LOD0 leaf cards (7,986
         triangles, 1,586 m²) are only 25-34 % opaque at the 0.32 cutoff (Monte Carlo over the card UVs,
         depending on the v convention), so cards trimmed to their alpha (Persson) could cut the pre-pass and
         the leaf share of shadows by roughly a third, at about 3x the leaf triangles (vertex cost to be
         measured: the vertex shader carries instancing and tree wind). Beyond that: a visibility buffer
         for alpha-tested foliage (Forbidden West), or real leaf geometry with cluster LODs.
3. **Cached far shadow cascades** (2.9 ms in the forest): scroll the far cascades and draw trees there in a
   still pose (Fortnite's base pose); keep the near cascade per frame. Our sun is fixed.
   - [x] Done 2026-11-22 (`agents/perf`): stepped cascades, on by default (`ENGINE_SHADOW_STEPPED_CASCADES=0`
         turns them off). The cache already existed (cascades re-rendered only when their centre moves, at
         most two a frame while walking, the far one every 240-600 frames); the middle cascades followed the
         camera texel by texel, so walking re-rendered ~2 cascades a frame. Now every cascade past the
         nearest moves in 1/8-range steps like the far one, draws plants in their rest pose, and leaves the
         rotating wind refresh to the nearest. Interleaved walks: cascades re-rendered per frame 1.96-2.22 →
         1.13; shadow GPU 2.6-3.1 → 1.1-1.4 ms; forest walk 14.6-14.9 → 13.1-13.4 ms, walk + turning
         15.4-15.5 → 13.9-14.0 ms (frames > 33 ms 17 → 3-13). Captures at ForestFloor, ForestEdge,
         MeadowSlope, Vista, FieldWalk: same shadows, differences are wind. New frames.jsonl key
         `shadow_cascades_refreshed` (bit mask).
   - [x] Longer shadow ranges (2026-11-22, `agents/perf`). Interleaved walks (forest, field, turning) and
         stills at ForestEdge, ForestWall, MeadowSlope, Vista, FieldWalk and a mid-range view with trees at
         ~100-250 m (`--camera-position 40,-8,-80 --camera-target 140,-27,90`, "MidB"); looks from stills
         with `ENGINE_WIND_TIME=100` (freezes wind; cloud shadows still move).
         1. Trees cast with their LOD1 mesh to 220 m (was 90), `ENGINE_TREE_SHADOW_DISTANCE`. Alone it changed
            nothing: the near cascades ended at 150 m, and `foliageCasterDistance` (150 m) caps every
            wind-animated caster, the oaks among them (it would have left trees at 150-220 m with no shadow,
            since the impostors only cast past 220 m).
         3. Near cascades to 250 m (was 150) with the split blend 0.65 → 0.79, so the first cascade keeps its
            ~18 m (second 43 → 49 m, third 49-250 m), and the foliage caster budget 250 m: the
            `RealtimeShadowSettings` defaults, project loader fallbacks and template. A/B switches
            `ENGINE_SHADOW_NEAR_DISTANCE` (keeps the authored first split), `ENGINE_FOLIAGE_CASTER_DISTANCE`;
            the old behaviour is tree 90 / near 150 / foliage 150. Projects that write 150 m keep it.
            Cost 1+3 (new / old): shadow GPU forest walk 1.33 / 1.18 and 1.54 / 1.48 ms, turning 1.35 / 1.30
            and 1.36 / 1.52; shadow recording +0.1-0.36 ms CPU in most stills; frames within drift. On by
            default. Look: near shadows unchanged (ForestFloor 0.02 % pixels), ForestWall's canopy at
            90-220 m self-shadows more deeply, elsewhere ≤ 0.2 % (the meadow ridge at ~150 m in MidB).
         2. Rocks, boulders, logs and stumps cast to 150 m (was 80), `ENGINE_PROP_SHADOW_DISTANCE`: forest walk
            shadow GPU 1.23 / 1.46 ms against 1.36 / 1.56 off, frames 15.2 / 15.7 against 15.2 / 16.2: no
            measurable cost, on by default. Scanned bushes, shrubs and ferns (`ENGINE_SCAN_BUSH_SHADOWS=1`,
            out to their 80 m reach): shadow GPU +0.7 ms (2.01 / 2.25), shadow recording +0.8-1.1 ms CPU,
            forest walk frames +1.3-2.5 ms with more spikes, for a subtle look (bushes self-shadow and read
            more solid at MeadowSlope). Off by default.
   - [ ] Scanned plant shadows cheaper: the 30 m variant (`ENGINE_SCAN_BUSH_SHADOW_DISTANCE=30`) cost as much
         as 80 m (2.33 / 2.38 ms), so the range is not what limits it; check the grass caster budget and
         per-tile caster culling of the scan fields, or a shadow-only card LOD.
4. **Far forest as voxels or octahedral impostors** (Next 5; Chlorophyll, UE 5.7): mainly look and memory;
   per-instance data dropped for the farthest proxies.
5. **Cluster LOD / mesh shaders for trees** (Chlorophyll, Nanite, Alan Wake 2): the reference engine's core,
   and the largest change; the RTX 4090 supports mesh shaders (DX12 and Vulkan; DX11 would keep today's path).
   Only after 1-3 show where the forest's GPU time sits.
6. **Work graphs**: watch; too young (driver support, no amplification shaders with mesh nodes).

Phases are ordered by payoff per cost. Phase 1 is shader and asset-cook work with almost no frame cost; Phase 2 buys
the headroom for Phases 3-6. Phases 1b, 1c, 1d and 7 came from user reports during the work.

Ordered by payoff per cost. Phase 1 is shader and asset-cook work with almost no frame cost and should
close most of the visual gap on trees. Phase 2 buys the headroom for Phases 3-6.

### Foliage Phase 1: Leaves lit as volumes (biggest visual win, ~free)
- [ ] **Leaf gloss**: tried oak leaf roughness 0.86 → 0.6. No visible gain in the test poses, and on the
      vista (looking toward the sun) crown tops caught a white grazing sun glint (69 px > 170 in the
      near-forest crop vs 0 before). Reverted to 0.86. Revisit with the wetness look, likely with a
      leaf-specific specular lobe rather than plain GGX.
- [ ] Oak-pack impostors (`*_Impostor`, the pack's Billboard_Mat) are not re-baked, so they still carry the
      old flat leaf lighting. The forest clusters were re-baked from the cooked meshes (Foliage Phase 1c).
- [ ] `wetness` scene value (lower roughness, darker albedo) for the GTA VI rain look.

### Foliage Phase 7: World detail (2026-11-19, user request: paths, streams, small things, cliffs/overhangs)
Asset inventory: the repo's only textured, LOD'd vegetation is the oak pack (17 models, 8 used), nine pack
bushes, twelve scanned rocks and one log (`Assets/Other` also holds an unused `grass_patches_-_circle` and the
`real_bush` source). Everything else is generated. The terrain shader has six fixed paint layers.
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
- Cost (single runs, DX12): Trail 27.1 ms, ForestFloor 26.0 ms, both CPU-bound as before (record ~16.5 ms,
  GPU 11.7-13 ms).

### Foliage Phase 2: Pay for it (performance)
Progress 2026-11-19 (landscape ForestEdge, DX12 1080p, single runs; frame / update / prepare / record ms):
27.1 / 1.8 / 7.8 / 16.2 → 24.1 / 1.6 / 6.8 / 14.4. Still ~41 fps: CPU-serial and GPU (~14.5 ms) both short.
- **Finding: the landscape's foliage cost is geometry, not pixels.** At 960x540 foliage depth only drops
  4.98 → 4.07 ms (leaves 2.06 → 1.33). 41M triangles submitted (25.5M candidates), 59.5k placements at LOD0,
  detail budget pinned at its 0.6 floor (13.2 ms geometry vs a 6 ms budget). The Detail view shows the near
  and mid trees on the right bands; the cost is the forest *behind* the visible edge. The CPU occlusion test
  hides 282 of 2356 tested items (whole tiles). → GPU Hi-Z occlusion per placement is the main lever for
  forest views; card trimming is secondary (leaf cards are only 36-39 % opaque, but this view is not
  pixel-bound).
- [ ] **Moving camera, remaining hitches.** Walking still has clusters of 80-120 ms frames each time a
      cell boundary is crossed (3 clusters in 400 frames): `update_post_physics_ms` 1.3 → 22,
      `update_objects_ms` 1.6 → 13, `prepare_graphics_ms` 0 → 7, `stream_lod_ms` 0 → 6. Walking also
      raises the steady cost (update 1.8 → 4.6 ms, prepare 4.7 → 8.9). Profile a walking run
      (`--drive-forward 380 --cpu-sample`) and spread that work over frames. Every later performance
      check uses the three moving runs as well as the still poses.
- [ ] **Next CPU steps** (2026-11-21: superseded by "Next" 1, 3 and 4 at the top of this file; kept for the
      numbers. Still camera: camera view ~11.5 ms, shadow view ~3.9, prepare ~4.7, update ~1.8;
      walking after the 2026-11-20 static-item and grass-pool changes: ~11 / ~4.7 / ~6 / ~3.4):
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
- [ ] **Standalone prefab trees**: find why `shadow_submission_ms` is ~64 ms for 84 non-instanced prefab
      trees (Phase 0 finding; `build_foliage_test_scene.py --standalone` reproduces it). The fine shadow
      timers (`geometryDrawMs`, `maxBatchMs`) only record with `Scene::SetUpdateProfilingEnabled`, which
      only the editor profiler sets; expose it to GameBackendCapture first.
- [ ] **Far blocks follow the detail budget** (also listed under Landscape look, section 0): far forest is outside it and is what
      makes the vista GPU-bound.
- [x] **Temporal upscaling** (2026-11-23, DX12 game renderer). The game now applies an internal render scale:
      the scene's `renderScale` times the quality preset's (as the editor's game view did), or
      `ENGINE_RENDER_SCALE=0.5..1` for A/B runs. `DX12GameRenderer` keeps the swap chain at window size and
      sizes the depth target, viewport and scene colour by the scale (`GetRenderWidth/Height`,
      `ApplyRenderScale`, applied between frames like a resize); scene views record at the render size.
      With TAA, `TemporalResolvePS` upscales when the scene is smaller than the history (`TemporalUpscale`:
      3x3 samples placed by the jitter, Gaussian weights, more weight for a sample landing near the output
      pixel, same history clipping and reprojection). Without TAA, `UpscalePS` stretches bilinearly.
      Benchmark header: `render_width`, `render_height`, `temporal_upscaling`. Checked on
      lighting_showcase, 960x540, RTX A2000 Laptop: opaque GPU 0.357 → 0.194 ms at 0.67; edges close to
      native, slightly softer (`.local/taau/edges.png`: native | upscaled | bilinear).
      Detail follows the output: `SceneRenderer::SetDetailResolutionScale` (output / render pixels, set
      by the game before recording) makes LOD coverage, the detail budget, texture footprints and the
      LOD selection's screen view use output pixels. Without it a near scanned bush in grass_test dropped
      to a coarse level with eroded leaves and a dark card at 0.67, with TAA or bilinear alike.
      grass_test turning at 1920x1080 (`--look-yaw 3`, no drive so poses match; two interleaved pairs,
      RTX A2000 Laptop): frame 25.5 / 25.5 → 16.5 / 16.4 ms; GPU grass 15.7 → 10.7, terrain 3.6 → 2.0,
      clouds 2.8 → 1.4, AO 0.6 → 0.2 ms. Look (`.local/taau/turn2/full3.png`, native | 0.67 TAA |
      0.67 bilinear): same LOD everywhere, upscaled slightly softer, no ghost trails while turning;
      bilinear has stair-stepped edges. Not checked: walking (drives move by time, so runs at different
      frame rates end at different places).
      - [ ] Landscape demo walk at 0.67 (the 1080p FieldWalk load did not finish in 10 minutes on this
            machine at native scale on 2026-11-23; stopped).
      - [x] Texture mip bias (2026-11-23): `ObjectData.viewTextureParams.x` = -log2(output / render
            pixels) while TAA upscales (0 for shadows and the bilinear stretch), added in
            `TextureLodBias` and the new `AlphaLodBias`, so every material, grass and terrain
            (`SampleGrad` gradient scale) sample is biased; ObjectGPUData 1936 → 1952 bytes. Alone it
            moved sharpness ~2 % (Laplacian variance, grass_test ground): the softness was the
            reconstruction. `TemporalUpscale` now sizes its kernel in output pixels and fetches history
            with a 5-tap Catmull-Rom: sharpness 55 → 156 against native 167 (`.local/taau/sharp4.png`).
      - grass_test now has a tree belt (`tools/build_grass_test_scene.py`: 3 jittered rows, 4/11/18 m in
        from each edge, oaks and conifers, 129 trees, gap for the explorer; `--no-trees`; foliage_test
        keeps its grove without it). Regenerating also took the demo's current exposure (-0.75) and
        ground-weighted metering. Use a 600-frame warmup: far trees are untextured impostors until
        streamed. Turning, 1920x1080, two interleaved pairs: native 37.7 / 36.7 ms → 0.67 24.7 / 24.3 ms;
        grass 16.2 → 10.7, terrain 6.0 → 3.3, leaves 3.0 → 1.8, foliage depth 2.4 → 1.5, clouds 2.7 →
        1.3 ms; shadows unchanged 1.6 ms (`.local/taau/timing_trees/pair.png`: native | 0.67).
      - [ ] Scanned bushes' coarse level looks broken (eroded leaves, a dark card) wherever it shows,
            e.g. near the camera at 1280x720 native: an asset LOD problem, not upscaling.
      - [ ] UI draws into the scene pass, so it renders at the render size and goes through TAA.
      - [ ] DX11 and Vulkan ignore the scale (render at full size).
      - [ ] Grass minimum pixel width and other in-shader pixel-size rules still follow the render size.
- [ ] Check: vista ≤ 16.7 ms with Phase 1 on; record the stage split against the budget table.
      Latest (2026-11-19, during CPU round 3, single runs, DX12): vista 25.7 ms (CPU record 15.7, GPU
      opaque 7.7); ForestEdge 26.9 (record 16.8, GPU opaque 8.8). Before that: vista 28.4, ForestEdge 27.8. **The GPU is now within budget at ground level; the CPU (update + prepare + record ~27 ms,
      serial) is the blocker.** Every CPU stage moved ±30 % together between runs today (machine load), so
      compare CPU changes with interleaved A/B pairs only. The forest floor adds 133 candidate draws and
      ~110 active grass tiles on the vista.

### Foliage Phase 3: Shadows and sky visibility (dappled light)
- [ ] **Canopy sky-visibility map** (texture version): top-down crown density around the camera, if the
      shadow-tap estimate's downwind offset or cost becomes a problem.
- [ ] **Soft shadow penumbra** on cascades 0-1: blocker search (PCSS-style) so canopy shadows on the ground
      are sharp near trunks and soft high up. Needs each cascade's light-space depth range in the object
      data to turn depth gaps into a penumbra width (not there today).
- [ ] Sharper near cascade (tighter first split or higher atlas share) for leaf-sized shadow detail.
- [ ] **Screen-space contact shadows**: blocked by the forward renderer: surfaces shade before the frame's
      depth exists (only the foliage pre-pass and last frame's AO copy), so a screen march would read stale,
      reprojected depth. Revisit with a depth pre-pass for all opaque geometry.

### Foliage Phase 4: Forest floor and undergrowth (RDR2 / GTA VI shots)
- [ ] Grass clumping (Grass Phase 7 open item). Dry blades: their share went 10 % → 22 % in Foliage Phase 1d
      and `dry_grass_clump` grows on the forest floor; a dry species mixed by the dry/lush noise is open.

### Foliage Phase 5: Trees
- [ ] **Octahedral impostors** (normals + depth + occlusion atlases) replacing the 6-view + top bake, lit
      with the Phase 1 leaf model so far trees keep translucency and dark interiors.
- [ ] Tropical set if the game needs it (palms, mangroves, broadleaf shrubs): palm generator with frond
      cards; mangrove prop roots. Decide with the biome plan.

### Foliage Phase 6: Air and camera
- [ ] Froxel volumetric fog (sun shafts through canopy, RDR2/pine shots). Re-use the cascade shadow; ~160x90x64
      froxels, temporally reprojected; budget ≤ 1 ms. Replaces the per-pixel shaft march in the forward shaders.
- [ ] Depth of field (shots 2, 4, 5): half-res gather bokeh, off by default, for cinematic/photo mode.

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
- [ ] The edge still reads as a band at ~25–35 m; try a wider `distanceFadeFraction`.

### Grass Phase 2: Terrain matches the grass (coverage + distance)
- [ ] Far tiles beyond the loaded cells use the macro colour palette (`LandscapeDriver` `farColors`,
      `mean[Grass] × .85`), not this tint. Match that palette's grass entry to the cover colour.
- [ ] Check: top-down and grazing captures near the grass edge; the edge should be hard to locate.
      Tune `groundTintStrength` / `groundRootShade` from captures. Related: the ground between blades in
      the foliage_test Meadow still reads paler than the grass (Foliage Phase 1d open item).

### Grass Phase 3: Density falloff with width compensation (coverage at mid range)
- [ ] Re-evaluate the fixed LOD distances (7 / 13 m in LandscapeDriver). With smooth thinning, push LOD
      switches further out, or regenerate LOD1/LOD2 meshes with less aggressive sward removal.
- [ ] Consider Tsushima-style view-space widening of blades seen edge-on (GodotGrass reference).
- [ ] Use the saved cost to push the grass radius out, or hold it steady under the detail budget.

### Grass Phase 4: Lighting as one surface
- [ ] The plan had the lean grow with distance (≈0.3 near → 0.8 far); not checked whether the shader does
      that or whether the far field needs it.
- [ ] Sky (SH) ambient instead of the flat `ambientUnlit` for grass and meadow plants (`PSGrassMain`,
      `PSFoliageMain`); changes meadow brightness everywhere, so it needs its own tuning pass. Canopy shade
      already scales the ambient (Foliage Phase 3).

### Grass Phase 5: Build tiles ahead
- [ ] Create and build tiles in a ring beyond the radius (`retentionRadius` exists) so they are resident
      before they reach the fade band.
- [ ] Prioritise tiles in the direction of travel (dot of tile offset with viewer velocity) over pure distance.
- [ ] Consider damping how fast `DetailRadiusScale` may move the grass radius, or excluding the fade band
      from it, so the edge does not slide visibly.

### Grass Phase 6: Grass texture past the radius (flat distance)
- [ ] Shell texturing is likely too expensive on the current low-spec target; revisit only if 6a/6b fall short.

### Grass Phase 6½: Antialiasing (grass edges stair-step and shimmer)
At the review the live renderer had no antialiasing (MSAA only in the DX11 video-export renderer), and grass
blades are 1–2 px wide at a few metres, so their edges aliased and shimmered under wind. Now: FXAA, MSAA
(DX11, DX12) and TAA (DX11, DX12; on by default). Temporal upscaling from a render scale exists on DX12 (Foliage
Phase 2).
- [ ] MSAA and TAA are done on DX11 and DX12 (todo_history.md); Vulkan has neither (renders without MSAA, no
      jitter applied). Not exercised: live MSAA switching in the editor's DX12/DX11 views. Capture overrides:
      `ENGINE_MSAA`, `ENGINE_FXAA`, `ENGINE_TAA`, `ENGINE_GRASS_MIN_WIDTH`, `ENGINE_AUTO_EXPOSURE`.

### Grass Phase 8: Larger, thicker foliage
- Coverage results on grass_test.scene (DX12 960×540, terrain share of ground pixels, near / mid / far
  screen bands; far is mostly open terrain past the grass radius):

  | Pose | Before (grass only) | + bushes | + thicker cover | Grass GPU |
  |---|---|---|---|---|
  | Raised (4 m up) | 42 / 71 / – % | 47 / 34 / 90 % | **23 / 16 / 87 %** | 2.4 → 3.4 ms |
  | Eye level | 36 / 34 / – % | 53 / 25 / 47 % | **21 / 25 / –** % (22 % overall) | 2.6 → 3.7 ms |

  Near and mid gaps roughly halved. Remaining terrain is (1) past the ~36 m grass radius (Phase 6
  impostors / longer range) and (2) small gaps between tussocks up close.
- [ ] Bushes share the grass radius (~36 m); larger bushes should reach further (their own range or a
      props/foliage layer).
- Coverage now (grass_test.scene, DX12 960×540; terrain share of ground pixels, overall | near /
  mid / far): raised pose **26.5 %** | 23 / 15 / 44 %, eye level **14.8 %** | 22 / 8 / 13 %
  (started at ~42–57 %). Grass GPU 3.6–3.8 ms with 17k plants (detailed grass, bushes, far ring).

### Grass Phase 7: Mesh/coverage tuning (optional)
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
- Far blocks and the detail budget, and cell-crossing spikes: tracked in Foliage Phase 2 ("Far blocks
  follow the detail budget", "Moving camera, remaining hitches") and "Next" 1.
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
  - [ ] Shader compile time itself: Object.hlsl ~21 s per entry in fxc (the fog include added ~2 s). Look at what
    fxc unrolls/flattens (lighting loops, terrain layers); cold-cache startup is still ~65 s
  - [ ] GrassField::LoadSources (0.18 ms) re-derives species LOD paths from string maps every frame, including a
    by-value copy of `ObjectLodProvider::GetObjectLodMeshes()`; needs revisions on its inputs to memoize

Looking around (`GameBackendCapture --look-yaw <pixels/frame>` feeds synthetic mouse-look through the normal input
path; pin detail with `ENGINE_DETAIL_SCALE=2.5` for comparable runs):

- [ ] Shadow record +0.2 ms from sharing workers with the early occlusion job

Vista 2026-11-16 end of day (adaptive detail, median/p95/p99 ms): static 10.0/12.5/14.6 (was 12.0/14.8/19.1);
fast turn `--look-yaw 20` 10.3/14.2/16.8 (was 15.2/24.8/30.3); slow turn `--look-yaw 6` 10.9/14.7/17.7
(was 12.0/17.8/20.6). Pinned detail 2.5 static: 10.2-10.35. Single runs vary by up to ~1 ms on this machine:
compare stage timers or interleaved A/B pairs, not single frame medians
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
- Light shafts (shadowed in-scatter) are done (todo_history.md). Open:
  - [ ] Sky pixels are not shadowed (the skybox has no shadow map), so shafts never cross open sky
  - [ ] Animate the shaft dither now that TAA exists (on by default since 2026-11-19; the LOD dissolve dither
    already rotates per frame, Foliage Phase 1b); 4-6 samples may then suffice
  - [ ] Not yet seen as distinct shafts: needs a low sun behind trees/ridges with moderate fog. Judge with the step 0
    shot when tuning defaults (below)

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

### 5. Sky computed from the sun
- Physical aerial perspective, volumetric clouds, the cloud-shadow cookie and the half-resolution cloud
  march are done (todo_history.md). Open:
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
- [ ] Re-run the step 0 benchmark, standing and moving (`--drive-forward`, `--look-yaw`): 60+ fps floor while
  walking and turning, 100+ on typical poses (RTX 4090). Now ~40 fps standing, ~28-32 fps walking (see Status)
