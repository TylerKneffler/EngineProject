# Landscape notes

Moved out of todo.md on 2026-11-24: expectations, status and look notes. Open work is in todo.md.

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

**Terrain and forest edge pass (2026-11-20, second machine: RTX A2000 laptop; captured and reworked the same
evening on the RTX 4090).** The first seven items were written on the laptop without a landscape capture, from
the generator's previews and counts on the archive; the capture and what it changed follow them.
Smoke test of the new build only: lighting_showcase on DX11 at 640x360 renders correctly.

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

      | Route | Speed | Scale | Frame | p99 | > 50 ms | GPU | CPU serial | camera / shadow record |
      |---|---|---|---|---|---|---|---|---|
      | Forest | 6 | 1 | 13.0 / 12.5 | 25 | 0 / 0 | 11.8 / 10.7 | 11.1 / 10.7 | 6.0 / 2.5 |
      | Forest | 6 | 0.67 | 12.8 / 13.1 | 23 | 0 / 0 | 9.8 / 9.8 | 11.0 / 11.3 | 6.0 / 2.5 |
      | Forest | 23 | 1 | 14.0 / 13.6 | 44 / 37 | 8 / 4 | 10.6 | 12.2 / 11.9 | 6.1 / 2.8 |
      | Forest | 23 | 0.67 | 13.4 / 13.2 | 36 / 37 | 3 / 5 | 8.7 / 8.8 | 11.8 / 11.7 | 6.0 / 2.7 |
      | Meadow | 6 | 1 | 11.8 / 12.1 | 26 / 25 | 0 / 0 | 8.7 / 8.9 | 10.4 / 10.6 | 5.6 / 2.4 |
      | Meadow | 6 | 0.67 | 11.7 / 11.7 | 26 / 25 | 0 / 0 | 7.5 | 10.2 | 5.4 / 2.3 |
      | Meadow | 23 | 1 | 14.2 / 14.3 | 39 / 44 | 4 / 8 | 8.7 / 9.0 | 12.5 | 6.4 / 2.9 |
      | Meadow | 23 | 0.67 | 13.4 / 13.8 | 37 / 39 | 3 / 4 | 8.0 | 11.8 / 12.2 | 6.1 / 2.8 |

      GPU = opaque (which holds foliage depth, leaves, bark and "opaque other") + terrain + grass + shadow +
      sky + clouds + AO + transparent. CPU serial = update + prepare + camera record + shadow record.
      * The main thread is the limit everywhere: CPU serial 10.2-12.5 ms against GPU 7.5-11.8. Upscaling cuts
        the GPU 1-2 ms (forest 6 m/s: foliage depth 1.31 -> 0.91, leaves 1.42 -> 1.05, terrain 0.78 -> 0.59,
        clouds 0.60 -> 0.33) but the frame only 0-0.8 ms, within run-to-run noise at 6 m/s. Not worth turning
        on until the CPU side drops; then it is ~2 ms of GPU in hand.
      * "Opaque other" is 4.0-4.2 ms of GPU at either scale: not pixel work (the hoisted cull pass and the
        rest of the opaque pass outside the named stages). The largest single GPU item; break it down.
      * Camera record 5.4-6.4 ms (forest 6 m/s medians): texture footprints 1.62 (every draw's mip needs for
        the camera and the cascades, each frame), instance tile preparation 1.07 (220 GPU cull batches),
        startup 0.97 (0.82 waiting for the CPU occlusion buffer, built 0.84 on a worker), submission 0.70,
        draw loop 0.59, unattributed 1.19; 352 visible items, 320 draws (~17 us a draw: overhead, not draw
        count). Off the critical path cheaply: footprints on a worker or every few frames, the occlusion buffer
        started earlier or last frame's (~2.4 ms together).
      * Against this morning's runs (not interleaved, so indicative only): forest 6 m/s 14.4-14.7 -> 12.5-13.0,
        meadow 12.9-13.1 -> 11.8-12.1; at 23 m/s forest p99 66 -> 37-44 and 21 -> 4-8 frames over 50 ms,
        meadow p99 70 -> 39-44, 16 -> 4-8.

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
- Uncommitted: everything since `4687647e`.

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

Looking around (`GameBackendCapture --look-yaw <pixels/frame>` feeds synthetic mouse-look through the normal input
path; pin detail with `ENGINE_DETAIL_SCALE=2.5` for comparable runs):

Vista 2026-11-16 end of day (adaptive detail, median/p95/p99 ms): static 10.0/12.5/14.6 (was 12.0/14.8/19.1);
fast turn `--look-yaw 20` 10.3/14.2/16.8 (was 15.2/24.8/30.3); slow turn `--look-yaw 6` 10.9/14.7/17.7
(was 12.0/17.8/20.6). Pinned detail 2.5 static: 10.2-10.35. Single runs vary by up to ~1 ms on this machine:
compare stage timers or interleaved A/B pairs, not single frame medians

Vista after CPU work (warm shader cache, same command, two runs): frame 12.6 / 12.8 ms (~79 fps, was 20.0);
update 2.3, prepare 4.3, record 5.7 (camera 4.0, shadow 1.4); GPU ~5.8 ms.

Vista 2026-11-16 round 5 (`--frames 900`; detail_scale 2.48 vs 2.03 above, so not directly comparable):
before validation/footprint changes 13.2 ms; after 11.8 ms (~85 fps), p95 14.1. Update 2.2, prepare 3.3,
record 5.9 (camera 4.5, shadow 1.1); GPU opaque 5.4.

### 1. Aerial perspective + height fog
- Light shafts (shadowed in-scatter) are done (todo_history.md). Open:

### 2. Terrain scale + erosion (Tools/build_landscape_demo.py)

### 3. Distant terrain color + forest coverage

### 4. Lighting + post

### 5. Sky computed from the sun
- Physical aerial perspective, volumetric clouds, the cloud-shadow cookie and the half-resolution cloud
  march are done (todo_history.md). Open:

### 7. Demo composition
