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

**Feel while turning (2026-11-26, done).** The user reported "look controls felt laggy" at a measured
~85-90 fps (14 ms from input to the end of the frame). Turning the TAA default off did not change it. Cause:
mouse-look read the cursor's offset from the window centre at capture and re-centred it after the update, so
any motion in between was lost, plus pointer acceleration. It now uses raw mouse counts (WM_INPUT,
`NativeInputSnapshot::rawMouse*`, FirstPersonController); the user: "much more responsive now". TAA stays off
by default (`RenderingEffectsSettings::taa`); walks measured before 2026-11-26 ran with TAA on.
Also fixed: the performance overlay read 0 fps outside benchmarks (frame interval was only set for them).
TAA back on, with a fast-motion cap (`taaMotionFeedback` 0.6, Preferences "TAA History Weight (Fast Motion)":
less history, tighter clip, Catmull-Rom history and lighter settle blur while pixels move 4-32+ px a frame;
user: "taa visible when rotating very fast"). Crash fixed (2026-11-26, interactive play, access violation in
`GraphicsCommandPacket::SetTexture`): material records kept across frames also copied the draw's own texture
slots (terrain/macro maps) without checking them, so a rebuilt map left a freed texture in the record; records
now match those slots too (misses still ~0 a frame).

**Fast flight: mass changes and cost (user, 2026-11-26: "flying fast and all objects mass updating and
changing causing immersion/natural look to break").** Flight is 25 m/s, 250 m/s boosted (landscape_demo
FirstPersonController). Evidence: boosted flight from MeadowSlope at ~90 m and at 1.5 m clearance
(`.local/routes/fly_1126`, GameBackendCapture `--drive-boost`, bursts of consecutive frames).
- [x] F1 Grass ignored camera height: tiles were chosen by horizontal distance, so at 90 m up the whole ring
      stayed built and was rebuilt every frame (50-95 tiles/frame, grass 20-56 ms CPU). Ranges, the
      per-field early-out and tile LOD distance now include the height above the field's ground
      (GrassField::Refresh). High flight: grass 24.8 -> 1.4 ms, frame median 65 -> 30 ms.
- [x] Automated flight (user: "setup a flight path and auto run instead of having me manually do it"):
      `FlightTour` route, 18 km loop at 60-200 m above the ground (tools/add_flight_route.py, run by
      build_landscape_demo.py so a rebuild keeps it); `python .local/routes/fly.py [--speed 100]` flies it
      and prints a summary (perf_runner also takes `--routes FlightTour`). Baseline 2026-11-26 at 100 m/s:
      median 18.2 ms (55 fps), p95 35.8, p99 56.8, 177 frames > 50 ms; far blocks preparing median 11
      (max 69), prefab layers pending median 8 (max 47).
- [ ] F2 What is left while flying. The "handoff waits" are the window thread waiting for the simulation
      thread (update, then prepare + record), not background jobs. At 250 m/s the update's serial cost is
      TerrainManager::Update (~10 ms of 11, `ENGINE_UPDATE_TYPE_LOG=8`). Done so far (2026-11-26):
  - [x] Far object blocks: gathering ~17k placements and clustering past farClusterDistance took 5-8 ms a
        frame on the main thread. Blocks more than a cell beyond the loaded radius now build on a worker
        (BuildFarBlockPlacements; cells' placements shared read-only) and install when ready; blocks next to
        the loaded cells still build at once (no doubled trees or holes at the takeover).
        `ENGINE_FAR_BLOCK_ASYNC=0` for A/B. 6 km at 250 m/s: frames with far objects > 5 ms 82 -> 6, p99
        55.9 -> 49.7, > 50 ms 23 -> 14.
  - [x] Far volume node installs: the vertex buffer is created by the node's build job
        (`ENGINE_FAR_WORKER_BUFFERS=0` for A/B); main-thread install 1.77 -> 0.64 ms median (2 nodes/frame).
  - Walks unchanged by these (forest 12.3-12.5 off vs 12.4-12.7 on). Note the forest walk is 12.4-12.7
    now against 11.7-12.2 earlier on 2026-11-26 with both off too: something else crept in (TAA
    Catmull-Rom while moving? material record check?), worth an A/B.
  - [ ] Left at 250 m/s (~20 ms median): the sim thread's prepare + record (~11 ms: detail_cpu_render 6.8,
        command_record 7.0) and update ~8 (stream cells 3.7 on crossing frames, far object evaluation
        ~2 ms of membership walks, the index buffer of far node installs).
- [ ] F3 Tree backlog: far object blocks preparing 30-97 and prefab layers pending 40-130 during flight, so
      trees arrive late and many at once. Prioritise along the direction of travel and by screen size;
      predict the camera (velocity x lead time) for streaming; at speeds streaming cannot keep up with,
      keep the far representation (blocks, impostors) instead of loading full detail passed in a second.
- [ ] F4 Trees switching one at a time, sporadically, read as a flickering forest when flying (user,
      2026-11-26). Cause: instanced LOD dissolves were distance bands of +-4 % of the switch distance
      (max 6 m), crossed in a few frames at 100 m/s and in one at 250, so each tree swapped LOD in one
      step at its own moment. Ruled out (FlightTour bursts, `.local/routes/flicker.py`, motion-compensated
      `popmap.py`): the TAA fast-motion cap (pop pixels 5.9 -> 5.5 %), single-view impostors past 600 m
      (5.92 -> 5.87 %), auto quality (Reference preset; detail scale stays 1). High route: 0.05-0.3 % of
      tiles change beyond the motion; low route (FlightTourLow, 18-75 m) up to 10 %, but near-tree parallax
      dominates there, so the measure cannot separate pops.
  - [x] Speed-scaled fade bands: the band widens by half the distance the camera covers in
        `SceneSettings::lodTransitionSeconds` (default 0.5 s, scene setting "LOD Transition (s)";
        `ENGINE_LOD_TRANSITION_SECONDS` for A/B), capped at a quarter of the switch distance; one width
        in Object.hlsl InstanceLodFadeWidth, InstanceCull.hlsl and the CPU tile pre-cull. Cost on
        FlightTourLow at 100 m/s: GPU opaque 8.9 -> 9.5 ms, foliage depth 3.0 -> 3.2, frame median
        unchanged (CPU-bound). Needs the user's look check.
  - [ ] If still visible: a time-based per-placement fade (state per pool slot in the cull pass) with
        hysteresis; and the 600 m single-view impostor snap.
- [x] F5 Abrupt low -> high detail when moving fast (user, 2026-11-26: "the change from low to high lod is
      abrupt ... make it smoother when moving across the terrain fast"). Cause: a cell's own trees replaced
      its far impostors in one frame, and at 250 m/s the cells were ready late (takeover at 3 cells: 117,
      2: 83, 1: 44, 0: 5 vs nearly all at 3 walking). Fix: near/far cell handover cross-fade
      (Rendering/CellHandover.h): once a cell's objects are ready its progress runs 0 -> 1 over
      lodTransitionSeconds; near placements in it keep pixels with a handover threshold < p, far impostors
      the rest (own interleaved-gradient dither so it does not interact with the impostor view and LOD
      band dithers); the far block drops the cell only after the fade, its impostors held at p = 1 until
      the rebuilt block is prepared. Per placement: the VS finds the root's cell in a 12-entry list in the
      object record (packed into the instance LOD distance for the PS; ObjectGPUData is now at D3D12's
      2048-byte element limit). `ENGINE_CELL_HANDOVER=0` for A/B. Low route at 250 m/s: abrupt handovers
      47.8/km (31.2 within 2 cells) -> 0, frame unchanged (24.0 ms). Load-ahead along the travel direction
      was rejected by the user (only helps one case).
  - [x] Unload margin (user: "a buffer between load and unload"): `TerrainManager::unloadMarginCells`
        (default 1, inspector): cells load within loadedRadius and unload past loadedRadius + margin; far
        terrain and far block sync tests use the kept radius. Flight +0.9 ms (24.0 -> 24.9), walks unchanged
        (forest 12.47, meadow 10.78).
  - [x] Transition monitor (user: "keep track of loaded objects -> lod states and their changes"):
        TerrainEvents transition_* fields per frame (handovers started/faded/abrupt/active/nearest,
        uncovered cells, far block rebuilds/installs, cluster flips, near block rebuilds, camera speed, LOD
        fade width); `fly.py` prints a per-km transitions summary and the frames a tree LOD fade lasts.
  - [x] Anomaly check (`ENGINE_TRANSITION_LOG=1` + `.local/routes/transition_check.py`, 2026-11-27):
        handover restarts 16-25 per run (a near block re-preparing for a neighbour cancelled every member
        cell's fade: trees vanished and faded in again) -> 0 (a fade under way holds); handovers finished
        in the unload margin then unloaded 3-12 frames later -> handovers start only within loadedRadius;
        the camera's far block rebuilt on the main thread ~1 frame in 5 -> covered-cell rebuilds go to
        workers (impostors stay hidden via the handover list), main-thread far rebuilds 145 -> 64 per run.
  - [x] Terrain see-through while changing level (user, 2026-11-27): runtime level swaps split pixels
        by a world-position hash, so old and new chunk surfaces drew neither at ~1/4 of the pixels; they
        use the screen threshold now. Near/far handover overlapped (near in over the first half, far out
        over the second) instead of a split that thinned the forest mid-fade.
  - [x] (1) Far terrain set swap -> cross-fade (2026-11-27): incoming nodes fade in on [0, p), outgoing
        out on [p, 1) (Mesh::SetRuntimeVisibility fromEnd, screen threshold) over lodTransitionSeconds;
        no new swap during a fade; a cell unloads only under a node that is not fading in (refusing all
        cells during any fade stopped unloads entirely: uncover 53 -> 5/km, +2 ms). `ENGINE_FAR_FADE=0`.
        Far swaps 28 -> 12/km, all faded; frame 23.7 -> 25.0 ms on the low route at 250 m/s.
  - [x] Objects vanishing during LOD transitions (user, 2026-11-27): the renderer's per-block band skip
        (SceneRendering, "a band whose distance range misses the block's bounds") kept a fixed 6.5 m
        margin while the speed-scaled dissolve is wider, so a block's incoming/outgoing band was skipped
        mid-dissolve; the margin follows lodFadeExtraWidth. And the handover list hid near objects of every
        loaded, uncovered cell (bushes and rocks have no far impostor); now only cells within loadedRadius
        (where a handover starts). Needs the user's look check.
  - [x] Trees/bushes still disappearing and reappearing going low -> high (user, 2026-11-27): the
        handover list held 12 cells; flying wanted up to 15 at 100 m/s (drops in 12 % of frames) and 78 at
        250 m/s, and a dropped cell's near objects jumped to fully shown and back. Now a fixed 11 x 11 grid
        of 3-bit cells around the camera's cell (CellHandover; same 48 bytes): 0 frames with a cell outside
        it at 100 m/s; at 250 m/s only cells > 5 away (trailing unloads, behind the camera). Unready cells
        are listed only when a near block holds them; awaited cells only while still covered.
  - [x] (2) Geomorph volume chunk levels (2026-11-27): the mesh job also meshes the next coarser level
        and stores, per vertex, the height to it, its normal and the switch distance (uvChannels3.zw,
        uvChannels0.xy); Object.hlsl VSMain moves the vertex there over 70-97 % of the level's range.
        `ENGINE_TERRAIN_GEOMORPH=0` for A/B; `ENGINE_VOLUME_TERRAIN_LOG=1` prints coverage (most vertices
        morph; mean travel 1-6 cm, so chunk level shape changes were small). Frame time unchanged.
        Far nodes (split/merge) not yet.
  - [x] "A ton of lod/quality changes in trees/objects when cresting hills" (user, 2026-11-27): the
        speed-scaled band (25 % of the switch distance) put a ~50 m ring of forest mid-dissolve between
        LOD1 and LOD2 (~9,000 trees at peaks, all in view from a crest), each drawn half and half. Cap now
        12 %, and tree bands overlap their dissolve (incoming complete by mid-band, then outgoing leaves;
        ground cover keeps the even split). Trees mid-fade: median 7,442 -> 3,159, max 20,455 -> 11,764;
        flight 17.3 ms. Walks 2026-11-27: forest 11.90 / 12.03, meadow 10.65 / 10.86 ms (84 / 93 fps).
        Never run two engine instances at once: a capture overlapping the user's watch flight halved both
        (23.9 vs 17.3 ms).
  - [x] Frame batches (user: "capture a few batches of frame by frame changes"): `fly.py --capture-every
        500 --capture-burst 10` + `.local/routes/burst_report.py` (per pair, tiles changing beyond the
        motion; overlays and strips of the worst bursts). 22 bursts on the low route at 100 m/s: 15 under
        0.3 %; the worst (12.7 %) is the route passing through a tree crown, the next (2-5 %) bright near
        crowns in motion; no tree seen vanishing, reappearing or switching shape.
  - [x] (3) Forest clusters <-> single trees (2026-11-27): no block flips any more. A far block carries
        its trees on one layer and its clusters on a second ("Far clusters"), as bands 0 / 1 of one
        instanced LOD switch at farClusterDistance in plain world metres
        (TerrainObjectLayer::forcedLodBand, Mesh::instanceLodPlainDistance: no layer/view/placement/shadow
        scaling), so each tree and cluster dissolves by its own distance with the usual band; a block
        carries a form only while part of it can lie on that form's side (+-12 % band). Vista 12.6 ms,
        forest continuous across 3 km; low flight 18.0 ms; transition check clean. The monitor's
        "cluster flips" now counts forms added/dropped out of range (invisible).
  - [ ] Smoother exchanges, plan from `.md/lod_transition_research.md`: (1), (2), (3) done above; (2) geomorph volume chunk levels (coarse surface per vertex, morph in the VS), then far
        nodes; (3) cluster <-> tree flips -> cross-fade; (4) hysteresis on far node split/merge and chunk
        levels; (5) leaf thinning toward the impostor, impostor depth, time-based per-placement fades.
- [ ] F5b Ground handover at speed: far node -> textured cell (brown ground patches appearing in one frame,
      fly_1126/high frames 280-281). Needs the Step 2 continuity work and a fade on handover.
- [ ] F6 Look bug seen on the way: a near-black patch on a hillside (fly_1126/high seq_0160, left), stable
      across frames - a material/macro colour problem, not a pop.

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
- [x] Re-baseline HEAD: meadow walk, forest walk, walk + turning; two interleaved runs each; full stage split
      (GPU per stage, CPU serial per stage, frame histogram, peak memory). This is the 80 fps scorecard; keep
      the table here and update it after each step below.
      Done (2026-11-26): kept up to date in Step 4/5 scorecards; latest with DXC: forest 11.81-11.90, meadow 10.85-10.90 ms (Step 5, Shader compile).
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
- [x] Now CPU-bound: GPU ~9 ms, main thread ~15 ms. Against 11-23 (`base1123`): update 1.1 -> 2.9 ms,
      camera record 5.5 -> 7.7, scene prepare 1.4 -> 2.4, scene objects 1,568 -> 6,645, camera-visible
      items 320 -> 483. Objects by kind (`ENGINE_PLACEMENT_DUMP=<sample>` also prints `[ObjectDump]`):
      4,388 spline `Point`s (4,292 of them under the 60 `Trails/Trail NNN` splines), 483 grass tiles,
      299 distant-terrain nodes, 287 far object blocks, 226 volume chunks.
      Resolved (2026-11-26): main thread ~9 ms (update 1.8, prepare 1.0, camera 4.0, shadow 1.9); the forest walk is now GPU bound (Step 5).
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
      - [x] Camera record 5.5 -> 7.7 ms: ~525 terrain objects (226 volume chunks + ~300 distant-terrain
            nodes) against 49 cells + far tiles before; texture footprints 1.6 -> 2.3, material other
            2.1 -> 3.3. One draw per cell/level for volume chunks (Step 4) and fewer, larger far nodes.
            Measured 2026-11-26: ~200 fewer distant nodes (`ENGINE_FAR_VOLUME_DETAIL=1.5`) saved only
            ~3.5 us per visible item, so terrain object count is not the lever.
            Resolved (2026-11-26): camera record ~4.0 ms (async footprints, material records kept across frames, far aggregate).
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
            - [x] Shadow record 2.5 ms, of it 1.7 "scene preparation": the shadow view repeats the
                  camera's per-view draw preparation over the same items (share it: "Retained render
                  scene", engine_notes.md Next 3).
                  Superseded (2026-11-26): shadow record ~1.9-2.0 ms after the registration and cluster fixes; the rest is Step 5 P2.
            - [x] Placement registration once per frame (2026-11-26): the pool registration stamp was per
                  view, so the camera repeated every registration the shadow pass had made
                  (SceneInstanceBatching; `ENGINE_REGISTER_PER_VIEW=1` restores it). Interleaved: camera
                  tile preparation 0.92 -> 0.48 ms, camera record 5.7-5.8 -> 5.2-5.3, frame 12.95-13.08 ->
                  12.54-12.62 ms.
            - Per-view phases (`ENGINE_RENDER_PHASE_LOG=<n>`, 2026-11-26): shadow view 1-2 setup 0.29,
                  candidates 0.09, draws 0.11, LOD + batching + tiles 0.94 ms; camera 0.01 / 0.19 / 1.82
                  (footprints in it) / 1.17. The shadow pass does not repeat the material work; its cost is
                  batching and tile preparation.
            - [x] Submission ~0.8, draw loop ~0.85, other ~1.1 ms, footprints ~1.1 (near chunks).
                  Superseded (2026-11-26): profiled in detail under Step 5 P1 ([RenderLoop]); the remaining work is P1/P2.
            - [x] `Scene::FindGameCamera` scanned every object (49 terrain cells call it each frame);
                  cached now (structure revision + a check over the camera registry). No measurable
                  frame change on the walks (the explorer is early in the object list).
      - GPU at Vista (2026-11-26, GPU-bound, 17.0-17.3 ms): foliage depth pre-pass 5.2 + impostor colour
        4.0 + terrain 2.5 + shadow 1.65 + leaves 1.1 + cull 1.1 ms; ~390k far-tree impostors drawn
        (single-level far blocks to ~8 km). Leaving impostors out of the depth pre-pass
        (`ENGINE_IMPOSTOR_DEPTH_PREPASS=0`) is far worse: impostor colour 4 -> 20 ms, frame 29-30 ms (heavy
        impostor shader, deep overlap); the pre-pass stays.
      - [x] Impostor views (2026-11-26): views under 5 % dropped, one view past 600 m (zero-weight quads
            collapse and are not rasterized): Vista depth pre-pass 5.1 -> 4.7, impostor colour 4.0 -> 3.8 ms.
      - Measured with `ENGINE_GPU_WORKLOAD_COUNTERS=1` at Vista: the depth pre-pass runs 48 M pixel
        invocations (~24 per screen pixel) for the far forest. Early depth works (PSShadowMain only clips),
        but each species is one instanced draw over the whole landscape, so species overlap each other;
        nearest-first tile order inside a draw (`ENGINE_SORT_BLOCK_TILES=1`, off) changed nothing.
      - [x] Far forest representation (user chose the recommendation, 2026-11-26): (b) cluster impostors
            past 3 km. `TerrainManager.farClusterDistance` 0 -> 3000 in the landscape scene and in
            `tools/build_landscape_demo.py` (inspector: Terrain Manager > "Forest cluster distance (m)").
            Vista (interleaved, `.local/routes/vista_ab.py`): frame 16.45 / 16.42 -> 12.82 / 12.05 ms,
            foliage depth 4.65 -> 2.2, impostor colour 3.93 -> 1.58 ms; walks use ~0.5 GB less. Look
            (`.local/routes/vclus_cmp.png`): the far band reads slightly darker and more even, still forest,
            no seam at 3 km. Costs found and fixed on the way:
            - Clustered blocks lie wholly past the far casters' reach (2.2 km), yet were prepared for the
              shadow view every frame (+0.33 ms at 23 m/s): their layers now get `farShadowDistance` 0.
            - Registration: each placement source keeps a handle into `PersistentInstancePool` (no lookup
              when unchanged), and the pool no longer keeps a reference per source per frame (an expired
              source's range is retired to the frame slot instead). Shadow record 2.15-2.23 -> 1.97-2.04 ms.
            - A deterministic 105-165 ms hitch at frame 4817 of the full walk: the object slot count passed
              2048 and the per-view object buffers grew, each set sized for 64 portal views (~68 MB of upload
              heap at 4096 slots) though the landscape has no portals. Buffers now hold portal views only
              when the scene has a SpatialManipulator (`SceneRenderer::m_objectPortalViews`; the portal view
              budget is capped to it; `ENGINE_PORTAL_SLOTS_ALWAYS=1` restores, A/B; portal demo scenes
              unchanged). Growth 30-146 ms -> ~3 ms; full walk peak memory 4.4 -> 3.6 GB.
            Full 534 m walk (5300 frames): median 13.10 -> 11.22 ms, p99 22.1 -> 19.0, > 50 ms 0.
            Remaining: at 23 m/s clusters still cost ~0.25 ms CPU (meadow 12.45 -> 12.7 ms: the cluster
            prefabs' extra parts in camera tile preparation, 0.42 -> 0.60 ms).
            Option (a), a cheaper single-view impostor between the near trees and the clusters, is left
            for if the switch shows while moving.
      - Main thread 2026-11-26 (meadow 1000 frames, overlay on): update 2.1 + prepare 1.7 + camera 6.8 +
        shadow 2.5 = ~13 ms; GPU ~9 ms; frame 14.05-14.2 ms (~71 fps). 12.5 ms needs ~1.6 ms more.
      - **Scorecard 2026-11-26 (1000 frames, 6 m/s, overlay on, two runs): meadow 12.33 / 12.78 ms,
        forest 12.94 / 12.67 ms (~77-81 fps): at the 80 fps target on these walks.** Not yet: walk +
        turning, 23 m/s, the full 534 m traversal (p99 24.5 ms, crossings 35-37 ms), and headroom (the
        GPU is ~9-11 ms on these walks, ~17 ms at Vista).
        - Turning (`--capture-args "--route-look-sway 60 --route-look-period 240"`, 6 m/s): meadow 13.01 /
          12.94, forest 13.05 / 13.05 ms (~77 fps), p99 24.5-28.6.
        - 23 m/s: meadow 14.93 / 15.15, forest 15.18 / 15.18 ms (~66 fps), p99 31-35, 2-3 frames > 50 ms
          per 1060. Against 6 m/s (meadow): update +1.2 (continuous streaming), camera + shadow record
          +0.9 (the run reaches 400 m in, busier scenery), GPU +1.
        - Steady update (~1.5-2 ms): VolumeTerrain x49 0.6-1.2, TerrainManager ~0.5, layers 0.3.
        - Camera record now ~5.2 ms: footprints 1.1, tile preparation 0.9 (per-part block range lists and
          per-source pool registration each frame), draw loop 0.85, submission 0.83, other 1.1.
        - Biggest structural lever left: the shadow view repeats the camera's per-view preparation
          (shadow record 2.3-2.7 ms, 1.7 of it preparation) -> share it ("Retained render scene").
        - [x] Texture footprints on a worker (2026-11-26, `ENGINE_ASYNC_FOOTPRINTS=0` off): the per-draw
              mip-demand pass takes a snapshot of the prepared draws and runs beside the rest of the
              camera view, joined before the view returns. Meadow (interleaved): camera record 5.1 -> 4.0,
              frame 12.27 / 12.30 -> 11.23 / 11.24 ms; resident textures unchanged.
        - [x] Settled prefab re-checks event-driven (2026-11-26, `ENGINE_PREFAB_SETTLED_FAST_PATH=0` off):
              once every layer had built, the round-robin re-check still spent its whole 0.75 ms budget
              every frame (775 layers, ~170 visited a frame, each re-probing its prefab sources one per
              visit). Now one background sweep re-stats every prefab source a second and advances a shared
              epoch on any change (`TerrainObjectLayer::PollPrefabSources`); a verified layer whose epoch,
              configuration, component revisions and placements are unchanged only reschedules, with the
              full check (LOD chains, generated objects) every 10-15 s; and the frame loop skips idle layers
              without calling them (`IsPrefabPreparationDue`). Interleaved, two runs each: prefab stage
              0.80 -> 0.11-0.14 ms, scene prepare 1.65-1.70 -> 0.97-1.09; frame meadow 11.14 / 11.15 ->
              10.83 / 10.70, forest 12.09 / 12.11 -> 11.96 / 11.91 ms.
        - [x] Placement pool registration (2026-11-26): the shadow view (first of the frame) re-registers
              every resident source, ~3,550 with the far forest, though none changed: each paid a node
              allocation in a per-frame `unordered_set` of live keys plus a `std::map` lookup. Now a per-source
              live-frame stamp and a hashed source table (`PersistentInstancePool`). Shadow tile inputs
              0.66-0.76 -> 0.34-0.35 ms (`ENGINE_RENDER_PHASE_LOG` now splits phase 6: records, batching,
              tile inputs with registration and range counts).
        - [x] Retired volume chunk meshes released on a worker (2026-11-26, `ENGINE_VOLUME_ASYNC_RELEASE=0`
              off): each replaced chunk mesh took ~1 ms to destroy on the main thread (committed D3D12
              buffers); at 23 m/s the 1 ms-a-frame release budget ran every frame with a backlog up to 360
              (`ENGINE_VOLUME_TERRAIN_LOG=1` prints `[VolumeRetire]`). 23 m/s, interleaved, two runs: update
              3.49-3.53 -> 3.08-3.12 ms median; frame mean meadow 13.62 / 13.90 -> 13.22 / 13.40, forest
              14.17 / 14.35 -> 14.07 / 13.94 ms.
        - [x] Layers and batch appearances opted out of frame updates (2026-11-26): neither has an
              `Update`, but `RequiresUpdate()` defaults to true, so 776 TerrainObjectLayer and 525
              TerrainBatchAppearance objects ran empty updates every frame. Update objects 1482 -> 181,
              update loop 1.2 -> 0.7 ms.
        - **Scorecard 2026-11-26 evening (1000 frames, overlay on, two runs): 6 m/s meadow 10.59 / 10.32,
          forest 11.90 / 11.91 ms (84-97 fps); 23 m/s meadow 12.50 / 12.54, forest 13.21 / 13.18 ms
          (76-80 fps).** Main thread at 6 m/s: update 1.8, prepare 1.0-1.1, camera 3.9-4.1, shadow
          1.8-1.9, UI 0.4. The forest walk is now near GPU-bound (opaque 5.3 incl. terrain 2.4 + leaves
          1.5, instance cull 1.55, foliage depth 1.3, shadow 1.0, clouds 0.6 ms).
        - 23 m/s before the last three items: meadow 12.6-12.7, forest 13.1-13.5 ms median (was 14.9-15.2). Update
              ~3.1 (loop 1.2: TerrainManager 0.5, 776 layers + 525 appearances ~0.27 of near no-op calls,
              ~0.3 loop overhead; grass spatial scripts ~0.6, mean 1.0), prepare 1.5-1.7, camera 4.3,
              shadow 2.15. `ENGINE_UPDATE_TYPE_LOG=<ms>` now also prints loop, parallel and script times.
- [x] Frames over 50 ms: 9-10 per walk (11-23: 0). Find them (cell crossings, far-volume swaps, chunk
      installs) with per-frame stage logs.
      Done (2026-11-26): full 534 m walk 0 frames > 50 ms (crossings, chunk release, portal-slot growth fixed; Step 5 cluster item).
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
      - [x] Crossing work spread over frames (2026-11-26): distant node installs by a time budget
            (`TerrainManager.farVolumeInstallMilliseconds`, 1.5 ms, inspector; was a count of 6), and the
            prefab build budget while layers build is a scene setting (`prefabBuildBudgetMs`, Properties >
            Visibility, default 3, landscape 1.5). Walk (534 m): median 13.47 -> 13.10, p99 24.5 -> 22.1,
            > 33 ms 10 -> 5, > 50 ms 2 -> 0. Remaining slow frames: crossings 30-45 ms (cell commit and
            restore 2-3.4 within its 2 ms budget + one item, far blocks ~2.5) and two 44 ms frames whose
            stages add up to only ~5 ms over normal (outside the engine's stages: present or OS).
      - Frames 1963-64 of that walk (81, 136 ms): every GPU stage inflated together, an outside stall.
- [ ] Check why far blocks draw past `farCanopyDistance` (`block.layer->viewDistance` is set to it) and decide
      which one is meant to hold: either the canopy paint takes over there (and needs to read as forest), or
      the blocks are the far forest and the canopy distance moves out.

### Step 1: terrain flicker and pops (evidence first)
Resolved (user, 2026-11-26: "looks fine, move on"): no pops on the current build (evidence below). The open
items (geomorphing, per-node far fades, hand-off) stay as the fix list if pops return.
- [x] Evidence: still camera with wind frozen (`ENGINE_WIND_TIME`; any change between frames is flicker) at
      MeadowSlope and the meadow trail, and a per-frame burst of a slow walk; log far-node swaps
      (`ENGINE_FAR_VOLUME_LOG`), chunk level and border-level changes and cell show/hide
      (`ENGINE_VOLUME_TERRAIN_LOG`) per frame; diff consecutive frames and tie every changed region to one
      event. Debug view colouring terrain by chunk level / far node size.
      Done (2026-11-26, DXC build, level distances 160/320/640):
      - Tools: per-frame terrain events in frames.jsonl (`terrain_chunk_level_changes`, `_seam_changes`,
        `_step_swaps`, `_far_swaps`, `_far_nodes`, `_cell_handovers`; Components/Terrain/TerrainEvents.h) and
        `ENGINE_TERRAIN_LEVEL_DEBUG=1` (flat colour: chunk levels 0-3 red/orange/yellow/green, distant nodes
        light to dark blue by size, cell height meshes grey; TerrainBatchAppearance::debugLevel). Bursts:
        `--capture-every N --capture-burst M` (seq_<frame>.ppm).
      - Meadow walk, 100 m at 6 m/s: 8 volume steps (each 6-94 chunk level changes + 60-124 seam changes in
        one frame), 7 distant-node swaps (~300 nodes each, the whole set), 14 cell hand-overs.
      - None shows as a one-frame pop (300-frame burst; step detector and the level view): level changes
        cross-fade over ~20 frames at the band edges (on-screen level changes 1 % -> 2 % of pixels for frames
        170-190 around the step at 188), mostly beyond ~100 m; the steps at 251 and 283 (94 level changes)
        change nothing on screen (under grass or off view). Distant-node swap frames change the horizon band
        less than a typical frame (0.6-0.75x the median).
      - Still camera at MeadowSlope (60 frames, wind frozen): ~1.8 % of ground pixels change > 6/255 a
        frame, evenly as fine grain over the grass (TAA jitter and dither), no patches.
      - So the "quality changing right in front of the camera" is not reproduced on this build; the raised
        level distances (64/128/256 -> 160/320/640, 2026-11-25) likely removed it. The items below stay
        for if it is seen again (record where: position and view, or a capture).
- [x] Hysteresis: chunk levels and far-node splits switch to the coarser level at bound x ~1.15 and back at
      the bound, so a camera near a boundary does not flip.
      Covered (2026-11-26): levels are chosen again only after the camera moves `lodStepMetres` (16 m) from the last step's position (VolumeTerrain LevelSteps), so a camera at a boundary cannot flip.
- [x] Paint independent of the mesh level: weights per pixel from a per-cell paint texture (Object.hlsl
      already has the paint-map path, flag 524288) instead of per vertex, so a level change moves only the
      shape and never the rock/grass/trail boundaries.
      Already in place (2026-11-26 check): volume chunks take their cell's paint map per pixel (SceneRendererDrawPreparation PrepareTerrainMaterial, flag 524288); the steep layer blends per pixel from the normal.
- [ ] Geomorphing (CDLOD): each level's vertices also carry the parent level's position, blended by distance,
      so a switch is continuous; replaces the dithered two-mesh cross-fade that shimmers (and smears in TAA).
- [ ] Far nodes: fade per node (old out, new in over ~0.3 s) or swap per subtree as it is ready, not the whole
      set at once; same hysteresis as above.
- [ ] Loaded cell ↔ far node hand-off changes geometry only: needs Step 2's shared material in the band where
      they meet.
- [ ] Recheck after each fix with the same still and burst captures; record pops per 100 m walked.
      Baseline (2026-11-26): 0 one-frame pops per 100 m on the meadow walk; ~20-frame cross-fades at the
      level band edges at each volume step (8 per 100 m).

### Approach (user, 2026-11-26: "low quality terrain close to the camera"; "distance alone is one-dimensional")
How large engines do it (general practice, not researched here): (1) mesh detail by **projected screen-space
error** per patch (CDLOD-style quadtrees, Unreal landscape LOD, Unity terrain pixel error), with
**geomorphing**, instead of fixed distances: flat ground stays coarse near, ridges and silhouettes stay fine
far, and field of view and resolution count; (2) **materials independent of the mesh**: paint weights and
normals per pixel from textures, so a level change moves only the silhouette; (3) material quality in
**bands** (near full layers + anti-tiling + detail normals; mid the same layers at 2-3 scales + macro
variation; far a colour/normal map), or a **virtual texture** that renders the blended material at the
texel density each patch needs (Frostbite, Far Cry 4/5, Unreal RVT). Order here: textured distant terrain
(below, Step 2), mid-range tier (Step 3), screen-space-error levels + geomorphing (Step 1), per-pixel paint
(Step 1); a terrain virtual texture later replaces most of these.
- [x] Near volume level distances are `TerrainManager` properties now (2026-11-26; inspector "Volume full /
      half / quarter detail to", saved; `ApplyVolumeLevelDistances` updates loaded cells); were hard-coded
      64 / 128 / 256 m. Landscape 160 / 320 / 640 m: the coarse band of 4-8 m triangles on the near slope
      at MeadowSlope is gone (`.local/terrainq/ms_levels_cmp.png`); walks unchanged (meadow 12.1 / 12.9,
      forest 12.6 / 12.8 ms; terrain GPU +-0.1). The lit view barely changed: the flat look is material.
- [ ] Screen-space-error volume levels: each chunk's geometric error (max vertical deviation between its
      level and level 0, measured while meshing) projected to pixels picks the level, with hysteresis;
      replaces the distances above. Then geomorph between levels (Step 1).

### Step 2: near → far material continuity ("in-between quality")
- [x] Textured distant terrain without new data (2026-11-26). Distant nodes get the loaded cells' layer
      materials and tiling (TerrainBatchAppearance; `m_farLayerMaterials`, handed to nodes installed before a
      cell loaded) and their own flag (4194304, not the near 2048 path: compact vertices have no weights).
      Object.hlsl `ApplyDistantLayerDetail` infers the layers per pixel from the macro colour against each
      layer's mean and the slope (steep layer, `terrainSteep` from `volumeSteepLayer`), and applies (a) the
      two strongest layers' grain at a quarter of the near repeat (to ~2.5 km; steep faces from the side)
      and (b) macro variation, the strongest layer's own texture at 4x and 16x that scale, as brightness
      ratio and normal detail (to ~8 km). Lesson: a single small-scale sample averages to the layer mean in
      the distance's mips and showed nothing (checked with a magenta debug of the path). Result
      (`.local/terrainq/far4_ms.png`, `far4_vista.png`): the distant meadow plain below MeadowSlope has
      grass grain and patches instead of flat green, mountain faces show broken rock. Cost: walks within
      noise (terrain GPU +0.1 ms on the meadow), Vista 17.0 vs 17.3 ms. User's case: a bare dirt hill past
      the loaded cells read as a smooth tan blob. Not done: the lighting of distant nodes is still the
      simpler ambient + sun model (no roughness or AO), and Vulkan's prebuilt SPIR-V needs a rebuild.
- [x] Measure first: luminance, hue and contrast against distance along the meadow walk view and from
      MeadowSlope (1440p tiles), and a debug view showing which material band each pixel is in; mark every
      visible step.
      Done (2026-11-26, level debug view as a terrain mask, 2560x1440 at FieldWalk and MeadowSlope): grass covers
      nearly all ground near the camera, so near -> far material changes show only on distant open meadows.
      There the loaded-chunk -> distant-node hand-off is a slightly darker green on the chunk side, no hard edge
      (`.local/routes/s2_field_crop.png`: normal over level view). At MeadowSlope the hand-off runs along the
      slope's crest (hidden). The row profile there mixes shaded slope and lit plain, so it says little.
      The visible distant difference is grass, not material: the meadows past the blades read flat (Grass to
      the horizon, G1-G2).
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
- [x] Macro variation in the near layered path past 20-60 m (2026-11-26): the strongest layer's texture
      at 4x and 16x its scale as a brightness ratio, matching the distant nodes. Subtle on the near slope;              
      the flat look there is mostly the ground-cover colour past the grass reach (below).
- [ ] Past the grass reach (~60 m) the ground takes the cover colour with the layer texture only as
      brightness detail: give the cover colour the same macro variation and grain, or let the layer show
      more there.
- [ ] Anti-tiling past 40 m: keep a cheap stochastic sample there (two hashed-offset/rotated samples per 4-8 m
      cell blended, or hex-tiling at a lower tier to ~150 m); turn `terrainVariation` back on where hex-tiling
      has ended. Measure the cost per tier.
- [ ] Repeat that grows with distance: 2 m near → 8 m → 32 m in octaves, blended between two scales, so texel
      density stays matched and the pattern frequency is not visible as a grid.
- [ ] Large-scale variation from ~30 m: patches of hue and brightness (20-200 m noise, plus the macro colour
      map's 16 m texels as a multiplier), like the references' meadows, instead of only from 256 m.
- [ ] Check: 1440p tiles at MeadowSlope, FieldWalk and the meadow walk before/after; repeat visible = peaks in
      the image autocorrelation across the 40-300 m band.

### Step 5: past the target - the serial main thread, then motion vectors (2026-11-26)
Research and numbers: [frame_pipeline_research.md](frame_pipeline_research.md). The walks are main-thread bound
(update 1.8 + prepare 1.0 + camera 4.0 + shadow 1.9 + UI 0.4 ms); render scale 0.67 saves only ~0.5-1.5 ms even at
Vista (GPU bound by vertices and overlap, not pixels), so a vendor upscaler is not a speed lever here.
- [ ] P1 Parallel per-draw preparation inside a view (camera phase 5: 0.8-1.0 ms, one loop over render
      objects). Split it into a serial resolve pass (pipelines, material records, terrain material state,
      detail-cost groups) and a parallel fill of the prepared draws, in render-object order. Then tile inputs
      and material records. Target: camera record -1 ms.
      - Profile (2026-11-26, `ENGINE_RENDER_PHASE_LOG` now prints `[RenderLoop]` sections): camera loop
        ~0.75 ms for ~570 draws: filter 0.08-0.10, setup 0.17-0.19, material records 0.35-0.41, terrain
        material + texture LOD 0.17-0.18, shadow params 0.02, probes + lights 0.02, pipeline + rest 0.08. The
        shadow view's loop is 0.02 ms (most of its candidates leave at the filter). Memory-bound per draw
        (each writes a 2,000-byte ObjectGPUData), so threads, not micro-cuts, are the lever.
      - [x] Material records kept across frames (`ENGINE_MATERIAL_RECORDS_ACROSS_FRAMES=0` off): ~500 of ~640
        camera draws missed the per-frame record cache every frame (generated batches each own a Material
        copy). Records now stay valid while revision, texture fields and their GPU textures, the scene ambient
        and the draw's environment seeds hold (records that read a reflection environment stay per frame);
        the cached record is applied straight into the draw (no 2 KB copies). Misses 500 -> 0; material
        0.38 -> 0.30 ms; camera record -0.1 ms (interleaved, two runs each: meadow 4.13-4.20 -> 4.03-4.06,
        forest 4.01-4.03 -> 3.95-3.99); frame within noise.
      - Re-check before the parallel fill (2026-11-26): the main thread is ~9.0 of the meadow's 10.5 ms
        (update 1.8, prepare 1.1, record 6.1) and ~8.8 of the forest's 11.9 ms: the forest walk is now GPU
        bound, so CPU cuts help the meadow and 23 m/s only (the material change above moved camera record,
        not the frame). Forest GPU (`ENGINE_GPU_WORKLOAD_COUNTERS=1`): leaves 1.66 ms for 3.3 M triangles and
        4.1 M vertex invocations but only 0.78 M pixels shaded (vertex bound: ~4 triangles per shaded pixel);
        foliage depth pre-pass 1.29 ms with 38 M pixel invocations (~18 per screen pixel); instance cull
        1.72; terrain 2.38; grass 1.57; bark 0.75 for 1.1 M triangles.
      - Hazards found for the parallel fill (besides the list below): `ResolveMaterialRecord` and the probe
        sampling assert the owner thread; `PrepareTerrainMaterial` lazily creates GPU resources (layer
        arrays, paint textures, macro colour map, trail data); `ResolveObjectPipeline` may create pipelines.
        Plan: workers fill draws into per-item slots; terrain-surface draws and material-record misses run on
        the owner; views with probes, detail-cost collection or heat modes stay serial.
      - [ ] Next: the parallel fill. Shared side effects to resolve first: `slot` (outer counter),
        `preparedDraws.emplace_back` order, `ResolveObjectPipeline` and `m_objectParallaxDepthPipelines`,
        the material record cache, `PrepareTerrainMaterial`'s `TerrainMaterialState`, `detailCostGroups`,
        the performance/detail heat vectors, `materialPipelineMs` and other timing sums.
            Deferred (2026-11-26): the forest walk is GPU bound; this helps the meadow and 23 m/s only (~0.5 ms).
- [ ] Forest GPU first (user, 2026-11-26; the forest walk is GPU bound). Measured on the walk
      (`ENGINE_GPU_WORKLOAD_COUNTERS=1`, now also terrain, shadow and impostor stages):
      - GPU occlusion (depth pyramid in the instance cull) is already worth 4 ms (`ENGINE_GPU_OCCLUSION=0`:
        frame 13.1 -> 17.1 with counters on); the leaf triangles left belong to visible trees.
      - Near layers cap LOD chains at three bands (LOD0 < 60 m, LOD1 to 220 m, then the impostor); fewer
        leaf triangles means moving those distances or screen-size selection (settings).
      - Terrain 2.3-2.6 ms for only ~380 k vertices but 1.0-1.26 M pixels: pixel-shader bound (~2.3 ns a
        pixel). Not the vertex format. Hex tiling off -0.14 ms, relief off ~-0.1 ms, PCF already 3x3: the
        cost is the layered material itself (up to 6 layers from texture arrays, biplanar on slopes, macro and
        distant detail, ground cover, trails) plus lighting.
      - [ ] Terrain runtime virtual texture (user: "continue with runtime virtual texture", 2026-11-26).
            Design:
            Parked (2026-11-26) (user): working but only 0.1-0.45 ms terrain GPU, softer near ground, ~300 MB; off by default (TerrainManager `virtualTexture`).
            - Fill: the terrain draws themselves, rendered top-down (orthographic) into the levels with a
              pixel-shader entry that outputs the blended material instead of lit colour (as UE's RVT
              output). This reuses every per-draw input (paint maps, layer arrays, steep layer, trails,
              ground cover) instead of re-implementing them in a compute pass; depth keeps the topmost
              surface where volume terrain overhangs.
            - Layout: a clipmap of L levels, 1024^2 each, three RGBA8 arrays (albedo sRGB + height or AO;
              normal XY + roughness + occlusion), slice = level. Sizing: from eye height ~85 % of ground
              pixels lie within 20 m (a pixel there is ~1-20 mm), so level 0 must be ~4 mm/texel (4 m) and
              levels double out to ~256 m: L = 7-8, ~100 MB. A coarser level 0 only helps the distant ~15 %.
            - Update: each level's window snaps to its texel grid around the camera and redraws only the
              strips that scrolled in (toroidal addressing); full refill when layer arrays, paint or trails
              change. Budget the texels per frame.
            - Use: top-projection pixels (not steep/biplanar, not within the relief range) read two levels
              by footprint and blend; ground-cover gusts and lighting stay per pixel. DX12 only (other
              backends keep the layered path; Vulkan bindings 28-31 are the layer arrays).
            Stages:
            - [x] 1 Offscreen colour rendering (2026-11-26): `IGraphicsTextureFactory::CreateRenderTargetArray`
                  (RGBA8 Texture2DArray, one RTV per slice), `IGraphicsContext::BeginColorPass` /
                  `EndColorPass` / `ClearColorPassRectangle` (1-4 targets on one slice + optional depth,
                  restores the framebuffer like the depth-only pass; recorded in command packets, skipped
                  on replay where unsupported), `IPipelineStateBuilder::SetRenderTargetFormats`. DX12 only.
                  Root signature t31-t33 are free for the three arrays (logical texture slots 24-26).
            - [x] 2 `TerrainVirtualTexture` (Rendering/SceneRenderer/SceneTerrainVirtualTexture.*, 2026-11-26):
                  levels, camera windows snapped to 1/16 of the extent, strip scheduling with toroidal
                  split, the fill (terrain draws with VS/PSTerrainVirtualTexture into three RGBA8 arrays),
                  TerrainManager properties (`virtualTexture`, levels, resolution, texel mm; inspector,
                  off by default), `ENGINE_TERRAIN_VIRTUAL_TEXTURE=0/1`, `ENGINE_TERRAIN_VT_LOG=1`,
                  `ENGINE_TERRAIN_VT_DEBUG=1` (magenta = served, blue = eligible but missed, yellow =
                  ineligible). Incremental fill on the forest walk: 2-4 strips, 65-130 k texels, 0.01-0.02 ms
                  CPU a frame. Eligibility fixed twice: biplanar starts at |n.y| 0.80, and rock chunks mark
                  every vertex with the rock slice (test the rock blend, uvChannels4.w, not the slice).
            - Fixed (2026-11-26): (1) dark ground = stale levels filled before their terrain streamed in or
                  came into view; terrain draws not drawn last frame now redraw their footprint in every
                  level (refills clear depth only, keeping neighbours drawn earlier). (2) Flicker and dark
                  grass = texture slots 16-23 (the view's light cookies and IES profiles, bound once per
                  view): terrain draws bound through slot 26, and the submission loop rebinds every slot up
                  to the highest count so far from the next draw's array, clearing the sun's cookie. The fill
                  now binds slots 0-15 only and every prepared draw carries the view's photometry textures
                  in 16-23. Flicker measured on 40-frame turning sequences (pixels that flip and flip back):
                  max 2.8 % vs 2.9 % off (18 % with the bug).
            - Result (2026-11-26, interleaved, two runs each; 2048 x 6 at 4 mm vs 2048 x 7 at 2 mm vs off):
                  terrain GPU forest 2.26-2.31 -> 2.10-2.21 (4 mm) / 2.10-2.15 (2 mm), meadow 2.64-2.74 ->
                  2.23-2.29 / 2.30-2.35 ms; frame within noise (forest 11.89-12.01 -> 11.64-11.75 / 11.72-
                  11.95, meadow 10.60-10.73 -> 10.42-10.65 / 10.53-10.93). The near path is visibly softer
                  at 4 mm. Smaller than hoped: what remains of the terrain pixel cost is lighting and the
                  ground-cover shading (shells, wind), which stay per pixel. Off by default.
            - [ ] Next: sharpness near the feet (2 mm level 0, or the layered path within a few metres),
                  then decide whether the ~0.1-0.45 ms is worth the memory (~300-350 MB) and the softer look.
                  Parked (2026-11-26) with the virtual texture.
            - [x] 3 Object.hlsl: sample the clipmap for top-projection pixels; A/B and look captures.
                  Done (2026-11-26): implemented (served/missed debug views, eligibility fixes); see the result and parked note above.
            - [ ] 4 Invalidation (paint, trails, array residency), budget, editor preview.
                  Parked (2026-11-26); new or newly visible terrain already refills (above).
      - [ ] Foliage depth pre-pass: 38 M pixel invocations (~18 per pixel) for 1.29 ms; leaves 1.66 ms.
- [ ] P2 Shadow and camera views recorded on separate threads: hoist the cascade selection the camera reads,
      move per-view mutable renderer state into a view state, make the shared caches (pipelines, material
      records, placement pool registration, upload caches, timings) safe or per view. Target: -2 ms.
- [ ] P3 Retained render scene (render items own what they draw), then the update of frame N+1 overlapping the
      recording of frame N. Target: -1.5-3 ms (more at 23 m/s).
- [ ] P4 Motion vectors: a velocity target from object, terrain, grass and foliage shaders, with previous-frame
      transforms and wind time; our TAA uses them. UI drawn after the upscale (Foliage Phase 2 follow-up).
- [ ] P5 Upscaler setting (built-in / FSR 3.1 / DLSS via Streamline) and optional frame generation (DLSS FG on
      RTX 40+, FSR FG), as project graphics settings. Frame generation is the one option that raises displayed
      fps while the CPU is the limit (~95 -> ~170 on the walks), at about a frame of latency.

### Step 4: performance to 12.5 ms (after Step 0's fix; each measured interleaved)
- [x] Camera record (7.2-7.7 ms, the largest serial item): texture footprints (~1.6 ms) on a worker or every
      few frames; the CPU occlusion buffer started earlier or last frame's (~0.8 ms wait); attribute the
      ~1.2 ms "unattributed".
      Done (2026-11-26): footprints on a worker (async), CPU occlusion off for the landscape; camera record ~4.0 ms.
- [x] Volume terrain draws: compact volume vertex (264-byte mesh vertex today) and one draw per cell and level
      instead of per chunk (~6x the height grid's draws); far nodes 0.56 M vertices at detail 2. Target
      terrain GPU ≤ 1.2 ms and less camera record.
      Not needed (2026-11-26): terrain is pixel-shader bound (0.38 M vertices, 1.0-1.26 M pixels; Step 5 Forest GPU).
- [x] Retained render scene ("Next" 3 in engine_notes.md): `PrepareFrameItems` copies every item each frame.
      Moved (2026-11-26) to Step 5 P3.
- [x] Cell crossings: `update_post_physics_ms` 17-22 ms on crossing frames (scene commit of the new cells'
      objects) is the p99.
      Done (2026-11-26): crossing work spread and retired (Step 0 crossing items); full walk p99 19 ms.
- [x] GPU "opaque other" (~4 ms on earlier walks): break it down.
      Done (2026-11-26): split into instance cull, impostor and other timers (Step 0); workload counters now also for terrain, shadow, impostor.
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
- [x] Confirm which surface draws the meadow plain from MeadowSlope (far node or loaded cell; debug colour).
      Done (2026-11-26, `ENGINE_TERRAIN_LEVEL_DEBUG=1`): the plain is distant nodes; only the slope in front is loaded chunks.
- [x] Far nodes carry the grass share and the mean grass height per vertex (from the surface painter and the
      blade generator's height rules), plus scene time and the view direction in their draw data.
      Done differently (2026-11-26): the grass share rides in the macro normal map's alpha (built from the paint on the layers the grass fields grow on, `TerrainManager::GetDistantCover().layerMask`, part of `ArchiveMapKey` so the map rebuilds when they publish); the cover colour, root shade and layer mask come from the loaded cells' `Terrain::groundCover`; distant draws get the wind time (SceneRendering, groundCoverColor.w). No vertex format change. Mean grass height is still open (G3).
- [ ] Keep `grassRadius` 50: the radius is not the lever for distant grass (meadow walk table, "Terrain and
      grass detail at distance").

### G2: wind waves on distant grass (small; fixes "no swaying past the hill")
- [x] Run `GrassGroundWind` on far nodes, same field, direction and speed as the blades, so gust fronts carry
      on across the hand-off from blades to tufts to ground.
      Done (2026-11-26): distant nodes take the cover colour (GrassPatchVariation) times GrassGroundWind, weighted by the grass share and away from the painted canopy (Object.hlsl distant path). TerrainManager `distantGrass` (inspector, on). MeadowSlope: the plain now matches the grass-covered slope in front instead of pale yellow-green paint, gust bands visible (`.local/routes/g2_cmp.png`: off over on); frame 10.18 -> 9.98 ms, terrain GPU 2.46 -> 2.38 (noise).
- [ ] Lean sheen instead of a flat tint: brightness and hue follow the lean against the view direction
      (strength now `.34 * lean + .12 * |gust|`).
- [x] Long-range wind visible (user, 2026-11-26: "wind effects long range aren't very detectable"): the 18 m
      fronts fall under a pixel and barely move on screen past a few hundred metres. `GrassGroundWind` now
      fades them by pixel footprint and adds gust patches (140 m along the wind, 4x the front speed) that
      group the fronts near and carry the wind far, with a stronger sheen at grazing angles. Needs a look
      check in a moving capture.
- [ ] Wind tunables as properties (editor-first rule): `GrassWindPatchSize`/`PatchSpeed` (and the existing
      `GrassWind*` shader constants) are code constants; move to scene wind settings.
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
      - [x] Landscape demo walk at 0.67 (the 1080p FieldWalk load did not finish in 10 minutes on this
            machine at native scale on 2026-11-23; stopped).
            Done (2026-11-26): render scale 0.67 measured (forest 11.75 -> 10.3, meadow 10.4 -> 9.8 ms, Vista 12.1 -> 11.7; frame_pipeline_research.md).
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
- [x] Profile and cut CPU frame cost for the vista (camera command recording, scene prepare)
      Done (2026-11-26): see Step 0/4/5 (main thread ~9 ms); Vista is GPU bound, now ~12 ms with cluster impostors.
  - Why serial: `SimulationLoop` (Game/Application/GameApplication.cpp) runs Update, then PrepareRenderFrame, then
    Render (shadow + camera recording) on one scene-owner thread while the window thread waits; only replay is on the
    render thread. Overlapping update N+1 with recording N needs a scene snapshot boundary; bigger change, do after the
    per-stage cuts below.
  - [x] Shader compile time itself: Object.hlsl ~21 s per entry in fxc (the fog include added ~2 s). Look at what
    fxc unrolls/flattens (lighting loops, terrain layers); cold-cache startup is still ~65 s
        Done (2026-11-26): DX12 compiles with DXC (all shaders ~7 s) plus parallel warm-up (items below).
    - Any Object.hlsl edit misses the cache for every entry that includes it (D3DShaderCache keys the
      preprocessed source), and the entries compile one after another (DX12ShaderCompiler, flags
      ENABLE_STRICTNESS | DEBUG | OPTIMIZATION_LEVEL3). Options, in order (user asked for todos, 2026-11-26):
    - [x] Compile in parallel (2026-11-26): `D3DShaderCache::WarmRecordedRequests` compiles every
          request recorded in requests.txt for this working directory on all cores at the first
          CompileFromFile, so the one-at-a-time requests that follow load from the cache
          (`ENGINE_SHADER_WARM=0` off). The build already had this as a post-build step
          (Engine/Tools/ShaderPrecompile.cpp, after Editor and Game), which builds of only
          GameBackendCapture skip; that is why every shader edit cost minutes in the capture loop.
          Measured: cold launch after a real Object.hlsl change 539 s one at a time -> 256 s (2.1x).
          The floor is the slowest variant: Water.hlsl PSMain (a material wrapping Object.hlsl's
          surface shader) takes 134 s alone, 237 s while 32 threads compile. Only DXC or a smaller
          surface shader goes below it. (Comment-only edits never miss: the key is the preprocessed
          text.)
    - [x] Development optimisation level: already measured (D3DShaderCache.h, 2026-11-25): compiling
          every recorded variant in parallel took 261 s at level 3, 307 s at level 1 and 191 s
          unoptimized. Not worth a setting.
    - [ ] (Low value since DXC: all shaders compile in ~7 s.) Compile only what is used: the terrain virtual-texture entries only when a TerrainManager has
          `virtualTexture` on; likewise other optional pipelines (debug, parallax depth) on first use.
    - [x] DXC (Shader Model 6) for DX12 (2026-11-26): `D3DShaderCache` compiles 6.x targets with
          dxcompiler.dll (loaded at run time: next to the executable, else the newest Windows SDK's,
          whose dxil.dll signs the output); `D3D12ShaderCompiler` asks for vs/ps/cs_6_0
          (`ENGINE_SHADER_DXC=0` keeps fxc 5.0). DX11 stays on fxc; Vulkan has its own SPIR-V path.
          The warm-up compiles recorded 5.0 requests as their 6.0 twins in a DX12 process. All 40
          recorded shaders compile under DXC unchanged. Cold launch after an Object.hlsl change: 539 s
          (fxc, one at a time) / 256 s (fxc, parallel) -> 39 s total with the shaders done in 7 s.
          Same image (mean difference 0.97/255 at MeadowSlope, clouds aside) and faster on the GPU:
          interleaved, two runs each, forest 12.55-12.66 -> 11.81-11.90 ms, meadow 11.15-11.31 ->
          10.85-10.90 ms; grass 1.70-1.74 -> 1.18-1.19 (forest) and 2.48-2.56 -> 1.78-1.84 (meadow),
          instance cull 1.72 -> 1.57, meadow terrain 2.75 -> 2.50 ms.
    - [ ] (Low value since DXC.) Split Object.hlsl so entries include only what they use (shadow, foliage depth and grass
          entries do not need the terrain material or lighting), so an edit recompiles fewer entries.
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
