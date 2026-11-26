# Engine work notes

Moved out of todo.md on 2026-11-24: plans, findings, reverted attempts and indexes. Open work is in todo.md.

## Former todo.md introduction

One plan for the landscape demo: look, foliage, grass and performance. `grass.md` and `foliage.md` were merged into
this file on 2026-11-19; their phases are the "Foliage" and "Grass" sections below, and code comments that say
`.md/todo.md Foliage Phase N` point there. Finished work is in [todo_history.md](todo_history.md), oldest first; completed items were last moved there on 2026-11-22.

**Goal:** the landscape demo meets the expectations below (the look of the Chlorophyll Engine frames in
`.local/reference-images/chlorophyll-video/`) and holds 60+ fps at 1920x1080 on the RTX 4090 **while walking and
turning**. 100+ fps on typical poses stays the aim. **Current target (user, 2026-11-25): a steady 80 fps average
(12.5 ms median) while moving and looking around**, with terrain that does not pop or flicker; plan in todo.md
("Now: 80 fps while moving"). The earlier reference sets in `.local/reference-images/` (eight
images, and six in `first-set/`) are variants to reach afterwards: water with reeds, fog, forest interiors.

## Static objects (2026-11-20)

`Object::isStatic` ("Static" checkbox under Enabled/Layer in Properties; scene attribute `static="true"`, written
only when set; children are static through their parent). Code: `Engine/Core/Scene/StaticObjects.{h,cpp}`, called
from `Game::Start` and `Game::Update`.

## Next, in order

**Fair visibility (user, 2026-11-24).** A player far away must be hidden by foliage by the same amount
whether seen from near or far, zoomed in or not, on any machine, and both ways round. Review of what
breaks it, most first:

**Top focus (user, 2026-11-23): seamless transitions.** Seen while walking: trees flicker as they change level
right in front of the camera, and their shadows with them; nearby grass disappears and reappears; bushes appear
in front of the camera; shadows switch at once from far to near quality. Wanted: foliage, terrain and objects
should seem to move closer or come into view, never just appear, flicker or vanish. Suspects, to confirm by
measurement first: the detail budget at its floor (0.64 of authored distances) pulling every switch close and
moving it frame to frame (`DetailRadiusScale`, grass radius); occlusion from last frame's depth (a revealed
object or grass patch missing for a frame); near-block hand-offs at cell crossings; stepped shadow cascades
(1/8-range jumps, plants in a rest pose past the nearest cascade, no blend between cascades); the forest trail's
low crown at eye height. Plan: (1) evidence: still-camera captures with wind frozen (any change between frames
is flicker) and per-frame sequences of slow walks, with per-frame counts of level switches, appearances and
cascade refreshes; (2) fix per system (distances that do not move, fades over distance and over a few frames
for new objects, cascade blending); (3) the same captures after each fix, frame time measured with them.

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
6. **Level chosen by size on screen.** With 1 in place, every placement picks its level from its projected size
   against a pixel-error target (`placementCoverageLod`, `Core/Rendering/ScreenCoverage.h`), the same for
   trees, props and ground cover. The detail budget becomes an overload guard instead of the policy; as the
   policy it has sat at its floor (0.64 of authored distances) on every walk, which is why trees and props
   switch close to the camera.
7. **Depth before shading for all opaque geometry.** Extend the foliage pre-pass (which saves 4-11 ms of leaf
   shading, 2026-11-21) to bark, props and terrain. That makes contact shadows and screen-space canopy occlusion
   possible: dappled light, darker forest interiors (Foliage Phase 3).

**Look and content, on top of 1-7** (each measured on the walks):

**Tuning, after 1-3** (settings, not structure; revisit once the frame has slack):

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
- **Shaders (DX12):** compiled with DXC to shader model 6.0 (DXIL) since 2026-11-26; fxc 5.0 only with
  `ENGINE_SHADER_DXC=0`, for DX11, or without dxcompiler.dll. Shader edits recompile in seconds; the
  cache key is the preprocessed text (comment-only edits never miss).
- **Antialiasing:** Vulkan has no MSAA and no TAA; temporal upscaling is DX12 only (game renderer). TAA and the
  upscaler reproject with depth and the camera only: no per-pixel motion vectors, which FSR, DLSS and frame
  generation all need (frame_pipeline_research.md; todo.md Step 5 P4-P5).
- **Editor:** terrain generation window; asset-generator tool not exercised in a running editor; AO in editor
  viewports; optional 3D LUT for grading.
- **Not measured:** cost of the 9-tap AO lookup; cliff pieces per cell; the detail model against measured GPU cost
  while turning.

## To investigate

---
