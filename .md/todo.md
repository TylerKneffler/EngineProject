# Todo

Open work only, as checkboxes. Finished items go to todo_history.md; notes, research and plans are in landscape_notes.md, foliage_notes.md, grass_notes.md and engine_notes.md. Code comments that cite "todo.md Foliage Phase N" (the Grass phases were replaced by "Grass to the horizon" on 2026-11-25) or a todo.md section: the open items are here under the same heading, the description in the matching notes file.

## Now: 80 fps while moving, and terrain that does not pop (2026-11-25)

User, 2026-11-25: "a steady 80 fps average while moving and looking around"; "portions of terrain changing
quality and causing it to flicker and change right in front of the camera"; "terrain materials close->far is
very abrupt, may need in-between material qualities or a longer high-quality distance"; "terrain materials at a
medium quality can be repetitive patterns breaking the realistic look".

**Goal.** 80 fps average = **12.5 ms median** on the meadow walk, forest walk and walk + turning (1920x1080,
6 m/s, RTX 4090, native, interleaved pairs), p99 ≤ 25 ms, no frame over 50 ms. Budget: GPU ≤ ~10.5 ms and the
main thread's serial CPU (update + prepare + camera + shadow record) ≤ ~10.5 ms. 60 fps stays the floor with
richer foliage, 100+ the aim on standing poses; upscaling is headroom, not the route.

**Where we are.** Meadow walk 18.2-19.4 ms median (≈52 fps), p99 40-44, terrain GPU ~2.0, grass GPU ~2.3-2.5,
camera record 7.2-7.7, update 1.5-1.7 ms (table under "Terrain and grass detail at distance" below). The same
kind of walk was 11.8-13.3 ms on 2026-11-22/23 (landscape_notes.md, the 6 m/s route table; todo_history.md,
"Blades finished": walks 11.4-13.3) and already 18.2-19.6 on 2026-11-24 *before* the volume conversion
(todo_history.md, volume-first step 0, height grid). So ~6 ms came in between 11-23 and 11-24, not from volume
terrain. Caveat: `e80eaec1` changed the meadow walk camera, so the routes may differ.

**What the code does today (read 2026-11-25, suspects until a capture ties them to what is seen):**
- Far volume nodes (`TerrainManagerFarVolume.cpp`): the wanted set is re-chosen every `farVolumeStepMetres`
  (32 m) and **swaps in whole, in one frame, with no fade**; a node splits when `distance < farVolumeDetail *
  size`, with no hysteresis. Far nodes are untextured: far-palette vertex colour or the macro colour map, lit
  by ambient + Lambert only (Object.hlsl flag 16384).
- A loaded cell whose volume starts to show replaces its far node in one frame: flat far-palette colour →
  textured, fully lit near material (and back when it unloads).
- Volume chunk levels (`VolumeTerrain::LevelAt`: 64 / 128 / 256 / 600 m) have no hysteresis; a level change
  cross-fades two different meshes with dither for `lodFadeSeconds` 0.35. Paint weights are per vertex, so at
  8/16/32 m spacing rock, gravel and grass boundaries **move** when the level changes, not only the shape.
- Border chunks are forced to the node spacing (`BorderLevel`) while a neighbour cell is not loaded and switch
  back when it loads.
- Material bands by distance (Object.hlsl, terrain path): relief < 15 m; hex-tiling 24 → 40 m; dual-scale
  albedo 28 → 60 m; **40-300 m one sample per layer at a 2 m repeat** (`layerRepeatMetres` 2; the per-cell
  UV offset `terrainVariation` is off whenever hex-tiling is on, so nothing breaks the repeat there); macro
  colour map from `macroColorNear` 256 m; layer means 300 → 1500 m; past the loaded cells (~384 m) far nodes
  with no texture and simpler light. Several steps stack between 24 and 60 m and again at ~256-400 m.

### Step 0: baseline and the missing 6 ms
- [ ] Re-baseline HEAD: meadow walk, forest walk, walk + turning; two interleaved runs each; full stage split
      (GPU per stage, CPU serial per stage, frame histogram, peak memory). This is the 80 fps scorecard; keep
      the table here and update it after each step below.
- [x] Find the ~6 ms between 2026-11-23 and 2026-11-24 (2026-11-25). It is the camera view's GPU
      instance cull, not terrain. "Opaque other" is now split into its own timers (`gpu_instance_cull_ms`,
      `gpu_impostor_ms`; GpuTimingStage::InstanceCull / Impostor): meadow walk cull **9.7-9.9 ms**, impostors
      0.3, other opaque draws 0.2. Cause: `a0643d5e` set `farClusterDistance` 2200 -> 0 (the painted canopy past
      `farCanopyDistance` replaced cluster impostors), so far object blocks hold every single tree out to
      `farObjectRadius` 64 cells (~8 km): resident placements 411k (11-23) -> 1.70M, of which far objects
      1.65M in 287 blocks (`ENGINE_PLACEMENT_DUMP=<sample>` prints placements per layer). The cull touches
      all of them every frame (~6 ns a placement). Not the cause: fair-visibility LOD (`ENGINE_FAIR_LOD=0`:
      no change in cull or frame), DPI awareness (old captures were 1920x1080 too).
      A/B, scene copy with `farObjectRadius` 18 (~2.3 km), interleaved (`.local/routes/s0_far{64,18}_r{1,2}`):

      | farObjectRadius | median ms | p99 | > 50 ms | cull GPU | camera rec | placements | peak |
      |---|---|---|---|---|---|---|---|
      | 64 (now) | 19.08 / 18.71 | 52.8 / 50.0 | 13 / 10 | 9.87 / 9.66 | 7.9 / 7.8 | 1.70 M | 4.6 GB |
      | 18 | 12.87 / 13.58 | 34.7 / 37.9 | 2 / 3 | 2.43 / 2.54 | 5.6 / 5.9 | 0.22 M | 3.3 GB |

      But 18 loses the forest past ~2.3 km at Vista (`.local/routes/s0_look_cmp.png`, top 64, bottom 18): far
      blocks do draw past `farCanopyDistance`, and the painted canopy reads as flat green fields there. Not
      applied.
- [x] Make the far-forest cull cheap instead of cutting reach (2026-11-25). Cause: the persistent placement
      pool (`PersistentInstancePool`) lived in Upload memory, so the cull (and the draws) read every
      candidate's 64-byte record across PCIe each frame (~800k candidates, ~11 ns each). Not the cause:
      parts/levels culling the same placement (a whole-block distance test, `ENGINE_BLOCK_DISTANCE_CULL`,
      cut candidates only 821k -> 801k and the cull 9.3 -> 9.1 ms; kept, it is exact) and Hi-Z occlusion
      (`ENGINE_GPU_OCCLUSION=0`: no change). Fix: the pool lives in GPU memory; `Upload()` collects changed
      ranges and `FlushUploads()` copies them in through a per-frame staging buffer
      (`IGraphicsContext::CopyBufferRegion`, DX12 + command packets; DX11/Vulkan keep the Upload pool;
      `ENGINE_POOL_GPU_RESIDENT=0` for A/B). Meadow walk, interleaved (`.local/routes/s0_gpu_{on,off}_r{1,2}`):

      | pool | median ms | p99 | > 50 ms | cull GPU | opaque GPU | camera rec | update |
      |---|---|---|---|---|---|---|---|
      | Upload memory | 19.07 / 18.17 | 48.5 / 48.6 | 10 / 10 | 9.59 / 9.08 | 11.4 / 10.8 | 7.9 / 7.5 | 3.0 / 2.9 |
      | GPU memory | **16.43 / 16.07** | 40.1 / 39.2 | 10 / 9 | **1.25 / 1.24** | 2.9 / 2.9 | 7.8 / 7.6 | 2.9 / 2.8 |

      Look unchanged (Vista 0.7 %, MeadowSlope 1.3 % of pixels over 10 levels: dither/TAA noise on foliage
      edges, `.local/routes/s0_gpu_look_ms_cmp.png`). The 8 km forest stays.
- [ ] Now CPU-bound: GPU ~9 ms, main thread ~15 ms. Against 11-23 (`base1123`): update 1.1 -> 2.9 ms,
      camera record 5.5 -> 7.7, scene prepare 1.4 -> 2.4, scene objects 1,568 -> 6,645, camera-visible
      items 320 -> 483. Objects by kind (`ENGINE_PLACEMENT_DUMP=<sample>` also prints `[ObjectDump]`):
      4,388 spline `Point`s (4,292 of them under the 60 `Trails/Trail NNN` splines), 483 grass tiles,
      299 distant-terrain nodes, 287 far object blocks, 226 volume chunks.
      - [x] Trail points (2026-11-25). A/B with a scene copy without the trail splines' points: frame
            16.4-16.6 -> 14.7-15.1 ms (update -0.97, scene prepare -0.65: `update_objects` +0.66 from the 60
            `TerrainTrail`s rebuilding point signatures each frame, `prepare_items` +0.41 and
            `prepare_transforms` +0.20 from passes over every object). Fixed the first: a static
            `TerrainTrail` skips the per-frame check once published (editor edits still republish through
            the manager's trail watcher): `update_objects` 2.42 -> 1.75 ms, walk 15.68 / 15.82 ms.
      - [x] The per-object passes (2026-11-26). Static leaves without components stay out of the
            per-frame transform snapshots (`Scene::PreparesTransform`; lazy transforms when a spline reads
            them) and objects with no mesh, sprite or spatial manipulator get no frame lookup entry
            (`SceneFramePreparation`). Interleaved (`ENGINE_SKIP_STATIC_LEAVES=0` off): scene prepare
            2.32-2.39 -> 1.67-1.72 ms, update -0.2, frame -0.4 to -0.7 ms. The points stay ordinary scene
            objects (editor-first rule).
      - [x] Far-object texture footprints per block (2026-11-26, `ENGINE_FOOTPRINT_WHOLE_FAR_BLOCKS=0` off):
            one cached box projection per far block instead of per 512-placement chunk; footprints
            2.3 -> 2.1 ms only (skipping far blocks entirely measured 1.3 ms, so most of that cost is
            elsewhere in the far-block walk: open).
      - [x] Performance overlay (2026-11-26, user request): `UILineGraph` component + `PerformanceOverlay`
            (Game/Application), project setting `showPerformanceOverlay`, F3, `ENGINE_PERF_OVERLAY=0/1`;
            runs keep it on (user). It first cost ~1.5 ms, which exposed three engine costs, now fixed:
            `UILayout::Resolve` walked all 6.6k objects twice a frame once any canvas existed (now the
            canvas registry; post-physics 0.5 -> 0.03 ms), the UI packet path created a new vertex buffer
            every frame (now pooled; packet prepare 0.33 -> 0.10), and every text re-resolved its font path
            on disk every frame (now cached; retained UI 0.84 -> 0.19 ms). Overlay cost now ~0.2 ms.
            Text is soft because UI draws inside the scene pass (TAA, bloom, haze): Foliage Phase 2
            "UI draws into the scene pass" item; labels use capitals until then.
      - [x] Resize while loading crashed (2026-11-25): AO, AO depth copy and depth pyramid buffers were
            freed on growth while recorded packets still bound them; now retired like the cloud samples.
            The GPU placement pool and its staging are shared so packets keep them alive. Crash dumps:
            `%LOCALAPPDATA%\Engine\crashes` (GameApplication). `Game.exe --scene <path>` added.
      - [ ] Camera record 5.5 -> 7.7 ms: ~525 terrain objects (226 volume chunks + ~300 distant-terrain
            nodes) against 49 cells + far tiles before; texture footprints 1.6 -> 2.3, material other
            2.1 -> 3.3. One draw per cell/level for volume chunks (Step 4) and fewer, larger far nodes.
            Measured 2026-11-26: ~200 fewer distant nodes (`ENGINE_FAR_VOLUME_DETAIL=1.5`) saved only
            ~3.5 us per visible item, so terrain object count is not the lever.
            - [x] Far-forest texture footprints (2026-11-26): per impostor part the far blocks are
                  walked every 8 frames (staggered) and their combined demand reused in between
                  (SceneTextureFootprints, far aggregate). Interleaved (`ENGINE_FOOTPRINT_WHOLE_FAR_BLOCKS=0`
                  off): footprints 2.35 -> 1.2 ms, camera record 8.0 -> 6.8, frame 15.1-15.4 ->
                  14.05-14.2 ms; resident textures +1-7 MiB.
            - [x] CPU occlusion buffer (2026-11-26): its ~1 ms build sat on the main thread before camera
                  recording (wait = build: the early start did not overlap) and it hid only ~67 of 546
                  tested items; instanced vegetation has GPU Hi-Z occlusion anyway. Interleaved off/on:
                  meadow 12.67-12.83 / 13.97 ms (GPU +0.2-0.3), forest 13.0 / 14.6-15.0 (GPU unchanged),
                  Vista (GPU-bound) 17.3 / 17.0-17.9 (noise). New scene setting `cpuOcclusionCulling`
                  (Properties > Visibility, saved with the scene; `ENGINE_VISIBILITY_OCCLUSION_BUFFER`
                  overrides), off in landscape_demo.scene, default on elsewhere. If a scene needs it,
                  a cheaper build (parallel raster tiles, fewer occluders) is the follow-up.
            - [ ] Shadow record 2.5 ms, of it 1.7 "scene preparation": the shadow view repeats the
                  camera's per-view draw preparation over the same items (share it: "Retained render
                  scene", engine_notes.md Next 3).
            - [ ] Instance tile prepare ~1.0, submission ~0.9, draw loop ~0.9, other ~1.1 ms.
      - Main thread 2026-11-26 (meadow 1000 frames, overlay on): update 2.1 + prepare 1.7 + camera 6.8 +
        shadow 2.5 = ~13 ms; GPU ~9 ms; frame 14.05-14.2 ms (~71 fps). 12.5 ms needs ~1.6 ms more.
      - **Scorecard 2026-11-26 (1000 frames, 6 m/s, overlay on, two runs): meadow 12.33 / 12.78 ms,
        forest 12.94 / 12.67 ms (~77-81 fps): at the 80 fps target on these walks.** Not yet: walk +
        turning, 23 m/s, the full 534 m traversal (p99 24.5 ms, crossings 35-37 ms), and headroom (the
        GPU is ~9-11 ms on these walks, ~17 ms at Vista).
- [ ] Frames over 50 ms: 9-10 per walk (11-23: 0). Find them (cell crossings, far-volume swaps, chunk
      installs) with per-frame stage logs.
      - [x] Route for it (2026-11-26, user: "extend meadow walk path to capture cell traversal"):
            `MeadowTrailWalk` now follows its trail for 800 m (135 points, 8 cell borders; was 480 m and
            a 1000-frame run walked only its first 100 m, one border). Made by
            `.local/routes/extend_meadow_walk.py <length>` (same start and direction, the tool's trail
            sampling and eye heights; only that scene item is replaced; the points are ordinary editable
            spline points). The full walk is 8000 frames at 6 m/s: `perf_runner.py ... --frames 8000`.
            The 1000-frame scorecard runs still cover only its first 100 m.
            Shortened to 534 m (user, 2026-11-26): 90 points, 4 cell borders, `--frames 5300`.
      - [x] Crossing spikes (2026-11-26). Every crossing gave 5-9 frames of 35-73 ms: the update jumped
            15-50 ms, mostly `stream_far_objects_ms` (+23-29 ms for 3-4 frames). `ENGINE_FAR_OBJECT_LOG=1`
            (TerrainManagerFarObjects) splits it: scheduling new far cells read each cell's layers from the
            object archive on the main thread (~0.8 ms a cell: 32 cells/frame in play = 20-34 ms, 128 at
            load = 90-130 ms); the jobs read them now. Also: a crossing re-walked all 289 blocks' cells
            (6-10 ms) and rescanned 16.6k cells (2.6 ms); now only blocks on the band between the old and
            new far squares walk their cells (others re-check from cached members) and the scan adds only
            the cells that entered. Walk (534 m, 4 crossings): >50 ms frames 32 per 8000 -> 3 per 5300,
            p99 34.1 -> 29.9, crossing peaks 55-73 -> 40-45 ms; far-object update at a crossing 5-8 ms.
      - [x] Crossings at 40-45 ms (2026-11-26). Logs (`ENGINE_COMMIT_LOG=1`, `ENGINE_UPDATE_TYPE_LOG=1`):
            (1) the scene commit spent 7-14 ms removing even one object: removal drained the GPU
            (`m_beforeGpuResourceDestruction` = render loop WaitIdle) for anything but a bare Mesh+Material
            leaf, i.e. every volume chunk and distant node (extra `TerrainBatchAppearance`), far-object
            block, chunk collider and unloaded cell. Runtime-only subtrees of plain objects whose
            components are all engine terrain/render/grass/layer/collider types are now retired like the
            leaves (destroyed now, storage freed behind the frame fence; Scene::RemoveObject). (2)
            `UpdateArchiveMap` -> far volume node installs took 6-14 ms for 1-4 nodes: the content-cached
            `AcquireMeshBuffer` hashed every vertex and waited for the copy; nodes now install their own
            buffers (`CreateStaticVertexBufferDeferred` + upload indices, `Mesh::InstallOwnTerrainBuffers`,
            `SetTerrainGeometry(..., deferBuffers)`; DX12; others unchanged): 3-4.5 ms. Walk (534 m):
            p99 29.9 -> 24.5 ms, frames > 33 ms 35 -> 10, > 50 ms 3 -> 2, crossing peaks 40-45 -> 35-37
            (first crossing 50). Vista unchanged (clouds and dither only).
      - [ ] Crossings still peak at 35-37 ms: `TerrainManager` update ~12-16 ms left (stream cells 2-5,
            far objects 1-5, far node installs 3-4.5) + VolumeTerrain chunk work 1-2.
      - Frames 1963-64 of that walk (81, 136 ms): every GPU stage inflated together, an outside stall.
- [ ] Check why far blocks draw past `farCanopyDistance` (`block.layer->viewDistance` is set to it) and decide
      which one is meant to hold: either the canopy paint takes over there (and needs to read as forest), or
      the blocks are the far forest and the canopy distance moves out.

### Step 1: terrain flicker and pops (evidence first)
- [ ] Evidence: still camera with wind frozen (`ENGINE_WIND_TIME`; any change between frames is flicker) at
      MeadowSlope and the meadow trail, and a per-frame burst of a slow walk; log far-node swaps
      (`ENGINE_FAR_VOLUME_LOG`), chunk level and border-level changes and cell show/hide
      (`ENGINE_VOLUME_TERRAIN_LOG`) per frame; diff consecutive frames and tie every changed region to one
      event. Debug view colouring terrain by chunk level / far node size.
- [ ] Hysteresis: chunk levels and far-node splits switch to the coarser level at bound x ~1.15 and back at
      the bound, so a camera near a boundary does not flip.
- [ ] Paint independent of the mesh level: weights per pixel from a per-cell paint texture (Object.hlsl
      already has the paint-map path, flag 524288) instead of per vertex, so a level change moves only the
      shape and never the rock/grass/trail boundaries.
- [ ] Geomorphing (CDLOD): each level's vertices also carry the parent level's position, blended by distance,
      so a switch is continuous; replaces the dithered two-mesh cross-fade that shimmers (and smears in TAA).
- [ ] Far nodes: fade per node (old out, new in over ~0.3 s) or swap per subtree as it is ready, not the whole
      set at once; same hysteresis as above.
- [ ] Loaded cell ↔ far node hand-off changes geometry only: needs Step 2's shared material in the band where
      they meet.
- [ ] Recheck after each fix with the same still and burst captures; record pops per 100 m walked.

### Step 2: near → far material continuity ("in-between quality")
- [ ] Measure first: luminance, hue and contrast against distance along the meadow walk view and from
      MeadowSlope (1440p tiles), and a debug view showing which material band each pixel is in; mark every
      visible step.
- [ ] Middle material tier, 40 m to the loaded edge: one sample per layer with normal maps, roughness and AO
      kept, the repeat scaled with distance (Step 3), the macro colour folded in gradually; so "medium" reads
      as the near material at lower frequency, not a different material.
- [ ] Far nodes textured too, out to ~800 m-1 km: carry the two strongest layer weights per vertex (from the
      surface painter), sample coarse mips only, same light terms as near terrain (sun, ambient, AO, a fixed
      roughness); past that the macro colour map. The loaded border then stops being a material edge. Cost
      against terrain GPU (~2.0 ms now).
- [ ] Distant steep faces as textured triplanar rock on far nodes (also listed under "Terrain and grass detail
      at distance"; one item).
- [ ] Spread the band edges so no two changes stack (hex end, dual-scale start, macro start, layer means, far
      nodes): each blend over a wider range, staggered.

### Step 3: medium-range repetition
- [ ] Anti-tiling past 40 m: keep a cheap stochastic sample there (two hashed-offset/rotated samples per 4-8 m
      cell blended, or hex-tiling at a lower tier to ~150 m); turn `terrainVariation` back on where hex-tiling
      has ended. Measure the cost per tier.
- [ ] Repeat that grows with distance: 2 m near → 8 m → 32 m in octaves, blended between two scales, so texel
      density stays matched and the pattern frequency is not visible as a grid.
- [ ] Large-scale variation from ~30 m: patches of hue and brightness (20-200 m noise, plus the macro colour
      map's 16 m texels as a multiplier), like the references' meadows, instead of only from 256 m.
- [ ] Check: 1440p tiles at MeadowSlope, FieldWalk and the meadow walk before/after; repeat visible = peaks in
      the image autocorrelation across the 40-300 m band.

### Step 4: performance to 12.5 ms (after Step 0's fix; each measured interleaved)
- [ ] Camera record (7.2-7.7 ms, the largest serial item): texture footprints (~1.6 ms) on a worker or every
      few frames; the CPU occlusion buffer started earlier or last frame's (~0.8 ms wait); attribute the
      ~1.2 ms "unattributed".
- [ ] Volume terrain draws: compact volume vertex (264-byte mesh vertex today) and one draw per cell and level
      instead of per chunk (~6x the height grid's draws); far nodes 0.56 M vertices at detail 2. Target
      terrain GPU ≤ 1.2 ms and less camera record.
- [ ] Retained render scene ("Next" 3 in engine_notes.md): `PrepareFrameItems` copies every item each frame.
- [ ] Cell crossings: `update_post_physics_ms` 17-22 ms on crossing frames (scene commit of the new cells'
      objects) is the p99.
- [ ] GPU "opaque other" (~4 ms on earlier walks): break it down.
- [ ] Re-decide `loadedRadius` 4 (below: +0.25 ms terrain GPU, +0.5 update, +0.4 camera record) only once
      Step 2 makes far nodes look right; it may not be needed.

## Grass to the horizon: wind waves and cover without more blades (2026-11-25)

Replaces the old Grass Phase 3-8 lists (what was still open is under "Carried over" at the end; what was
already done is in todo_history.md). User, 2026-11-25: extend grass and wind waves to distant terrain without
many high-quality blades; objects should still sit in the grass at a distance, and be able to hide in it.

**Idea.** Past the blades the ground itself becomes the grass, in three layers that share one grass
description (share and height from the paint and the blade generator's hash) and one wind field:
1. Wind waves as shading: from far away grass reads by its lean (paler sheen leaning away from the camera,
   darker and greener leaning toward it), not by blades.
2. A grass layer at grass height (~0.3-1 m) with a broken, tufted top: it writes depth, so the lower part of
   anything standing in the grass (rocks, logs, props, actors) is hidden without per-object work.
3. One coverage function (grass height and density at a point) that the grass layer, AI perception and
   line-of-sight tests all read, so concealment is the same near or far, on any settings
   (far_detail_and_visibility_research.md, section 4, options A and C; option B as the last resort).

**Today.** Blades to ~60 m (`grassRadius` 50 + 10), far tufts to 200 m (under a pixel from raised views at
100+ m), and the loaded cells' ground cover: parallax tuft layers (Object.hlsl ~339) and `GrassGroundWind`
(Object.hlsl 388, used ~3685). Far volume nodes take the unlit path (flag 16384) and do neither, which is
presumably why the plain below MeadowSlope shows no waves (not confirmed). Far nodes carry no paint, so
they do not know where grass is.

**Budget.** Inside the 12.5 ms frame of the plan above: layers 1-2 together ≤ ~0.8 ms GPU (estimates: shading
0.1-0.3, the grass layer 0.3-0.6), grass GPU otherwise held at today's ~2.3-2.5 ms. Measured on the meadow
walk and MeadowSlope, interleaved.

### G1: grass data on distant terrain (shared with Step 2 above)
- [ ] Confirm which surface draws the meadow plain from MeadowSlope (far node or loaded cell; debug colour).
- [ ] Far nodes carry the grass share and the mean grass height per vertex (from the surface painter and the
      blade generator's height rules), plus scene time and the view direction in their draw data.
- [ ] Keep `grassRadius` 50: the radius is not the lever for distant grass (meadow walk table, "Terrain and
      grass detail at distance").

### G2: wind waves on distant grass (small; fixes "no swaying past the hill")
- [ ] Run `GrassGroundWind` on far nodes, same field, direction and speed as the blades, so gust fronts carry
      on across the hand-off from blades to tufts to ground.
- [ ] Lean sheen instead of a flat tint: brightness and hue follow the lean against the view direction
      (strength now `.34 * lean + .12 * |gust|`).
- [ ] Tune against moving captures (`ENGINE_WIND_FRAME_STEP`), not stills; from MeadowSlope the bands should
      read on the plain at 100-500 m.
- [ ] Far ring from above: let the ground's grass colour and sheen carry the meadow where the tufts fall
      under a pixel, rather than more or larger tufts.

### G3: the grass layer (objects sit in it, actors can hide in it)
- [ ] A second surface over grass-painted ground at the local grass height: the far nodes (and loaded cells
      past the tufts) drawn again, raised along the normal, with a dithered tufted top edge (TAA resolves
      it), the cover colour and the same wind (small sway and the G2 sheen). Writes depth, casts no shadow.
- [ ] Fade in where blades and far tufts thin out (~50-200 m) at a height matched to theirs, so the hand-off
      does not show; out to ~1-1.5 km, past which it is under a pixel and G2 carries it.
- [ ] Fixed reach and presence on every setting and detail budget (cheaper, never removed): it is cover.
- [ ] Check: rocks, logs and a test figure standing, crouching and prone in grass at 50 / 100 / 200 / 400 /
      800 m; their lower part hidden by the same amount at every distance.
- [ ] Fallback if the extra draw costs too much: raise the terrain itself by the grass height in grass areas
      (no extra draw, but a solid top with no gaps, and it disagrees with collision).

### G4: concealment that is fair
- [ ] Coverage function: grass height and density at a point from the paint and the blade hash, shared by
      the blade generator, the grass layer, AI perception and line of sight.
- [ ] Past the grass layer's reach (if there is one): an actor's lower part fades into the grass colour up
      to the coverage height (option B), not sunk.
- [ ] Cover vegetation (bushes, tall fields, shrubs) keeps a fixed reach and solid far levels: never thinned
      away by distance, the budget or settings (research section 5).

### Carried over from the old Grass phases
- [ ] Mesh grass LOD distances 7 / 13 m (the forest-floor and far grass fields in `landscape_demo.scene`): with
      smooth thinning, push the switches out or regenerate LOD1/LOD2 with less sward removed. (Was Phase 3.)
- [ ] Antialiasing on Vulkan: no MSAA and no TAA (no jitter); live MSAA switching in the editor's DX12/DX11
      views not exercised. Overrides: `ENGINE_MSAA`, `ENGINE_FXAA`, `ENGINE_TAA`, `ENGINE_GRASS_MIN_WIDTH`,
      `ENGINE_AUTO_EXPOSURE`. (Was Phase 6½.)
- [ ] Dry grass mixed in by the dry/lush noise as its own species (Foliage Phase 4; `dry_grass_clump` grows
      only on the forest floor).

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
- Wind over distant grass (now in the loaded cells' ground cover; distant grass plan: "Grass to the
  horizon" above): `GrassGroundWind` (Object.hlsl) shades the blades'
  broad gust fronts (same field, direction, speed) on the ground cover past the blades; terrain draws
  with ground cover now get scene time (`SceneRendering.cpp`). Two MeadowSlope captures 6 s apart
  (`ENGINE_WIND_TIME=10/16`, `.local/foliage/wind2_*`): bands faint on the slope, none on the lit plain
  below (presumably far-volume nodes, which skip this shading; not confirmed).
- A/B overrides added: `ENGINE_TERRAIN_LOADED_RADIUS`, `ENGINE_LANDSCAPE_GRASS_RADIUS`,
  `ENGINE_FAR_GRASS_RADIUS`.

Recommendations:
- [ ] Set `loadedRadius` 4 in `landscape_demo.scene` (cheap per the table). Re-check the forest walk and
      23 m/s runs before calling it settled; 5 not measured.
- Grass radius, wind bands and the far ring from above: moved to "Grass to the horizon" (G1, G2).
- [ ] Distant steep faces: textured rock on far-volume nodes (triplanar rock on the steep band) so faces
      past the loaded radius stop reading as flat tan; this would let `loadedRadius` stay lower. Part of
      "Now", Step 2.
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
- Dry grass species: moved to "Grass to the horizon", Carried over. Clumping is done (blade Voronoi clumps).

### Foliage Phase 6: Air and camera
- [ ] Froxel volumetric fog (sun shafts through canopy, RDR2/pine shots). Re-use the cascade shadow; ~160x90x64
      froxels, temporally reprojected; budget ≤ 1 ms. Replaces the per-pixel shaft march in the forward shaders.
- [ ] Depth of field (shots 2, 4, 5): half-res gather bokeh, off by default, for cinematic/photo mode.

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
- [x] Editor tool: terrain generation window (size, relief, seed, erosion iterations) that regenerates and reloads the landscape.
  2026-11-25: Tools > Terrain generator (see "Not achievable in the editor", base heights)

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
