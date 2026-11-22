# Far terrain, paths and always-correct actors: research and options

Research of 2026-11-22. Two related problems in the landscape demo:

1. **Far detail looks wrong.** Paths and roads read as spots and dashes, especially at distance; far objects and
   far terrain do not agree (trees and rocks placed on full-detail heights sit above or below the coarser terrain
   drawn far away).
2. **Gameplay visibility must be right.** Other players, NPCs and enemies have to render correctly at any distance:
   - concealment by grass and bushes should not depend on whether that grass is drawn at the viewer's distance or
     settings (the PUBG/Arma problem);
   - cover objects must not vanish at distance or on lower settings;
   - an actor walking on terrain must not float above or sink into terrain drawn at a lower level of detail;
   - culling must never hide an actor that is in view, and an actor behind cover must not show through it.

Sources are a web search; most are talk slides, wiki pages, forum threads or press articles (noted per item).
The engine-side facts in section 1 come from the code and `.md/todo.md`.

## Contents

1. [How this engine does it today](#1-how-this-engine-does-it-today)
2. [Paths and roads on terrain](#2-paths-and-roads-on-terrain)
3. [Terrain level of detail and objects on it](#3-terrain-level-of-detail-and-objects-on-it)
4. [Concealment: grass and bushes at distance](#4-concealment-grass-and-bushes-at-distance)
5. [Cover and settings: what may differ between players](#5-cover-and-settings-what-may-differ-between-players)
6. [Important actors: always rendered, rendered right](#6-important-actors-always-rendered-rendered-right)
7. [Options and recommended order](#7-options-and-recommended-order)
8. [References](#8-references)

---

## 1. How this engine does it today

| Area | Today | Problem it causes |
|---|---|---|
| Trails | `TrailMask` in `LandscapeDriver.cpp`: the 0.5 contour of a broad noise, ~4 m either side of the track, written into the **Gravel paint weight at terrain vertices** (4 m grid in loaded cells). Vegetation placement keeps clear of the mask. | A 4-8 m track sampled on a 4 m grid aliases into blobs; far tiles carry the paint at their own, much coarser spacing, so a trail becomes **spots** with distance. Heights are not carved. |
| Terrain levels | Loaded cells (128 m, 32x32 quads at 4 m) out to `loadedRadius` 3 (~384 m), strided levels inside; beyond, far tiles at one quad per cell (128 m) with coarse parents, selected by screen error (`farTileScreenErrorPixels` 6); far stamp cells (cliffs) at stride 4 out to 8 cells. | Vertical error of the far levels can be metres; everything placed on full-detail heights (trees, rocks, far object blocks, and any actor) can **float or sink** against the far terrain. |
| Far objects | Far object blocks (last LOD level, impostors / forest cluster impostors) from cells beyond the loaded window; placements use full-detail heights. | Trees on far terrain can hover over valleys or sink on ridges where the far terrain is smoothed. |
| Grass reach | Detailed grass ~`grassRadius`+10 m, shrubs/bushes 80 m, far grass ring to 200 m (tufts), nothing beyond; the detail budget can scale reach. | A prone actor in grass is hidden up close and **fully visible** past 200 m (or past 80 m for bushes), and reach depends on the budget. |
| Bushes, props | Bushes reach 80 m (grass-field species), props layer bushes stop drawing ~25 m (cause not found, open todo). | Cover that one viewer sees and another does not. |
| Culling | GPU per-placement frustum + Hi-Z occlusion for instanced vegetation; CPU occlusion buffer (shrunk occluders) for objects; detail budget scales distances. | No notion of "important": an actor is culled and LOD'd like scenery. |

---

## 2. Paths and roads on terrain

### What others do

- **Serious Sam 4** (Croteam, GDC 2019, *Four Million Acres, Seriously: GPU-Based Procedural Terrains*; slides on
  GPUOpen): roads, rivers, trails and streets are **splines**. Along them vegetation fades out at a transition
  border and the surface is **forced to its own material** (gravel, dirt, asphalt). Terrain is 128x128 km generated
  on the GPU from templates, ~128-200 MB of data, tiles of 33x33 vertices; rendering layers: bare terrain, forest
  impostors, near trees, misc plants and crops, grass blades as geometry.
- **Frostbite / Battlefield 3** (GDC 2012, Widmark; SIGGRAPH 2007, Andersson): terrain materials are procedural
  shaders **splatted into a procedural virtual texture** (cached, clipmap-style indirection, ~32 samples per metre
  practical limit); **spline and quad decals** on top; rivers and lakes as free-floating decals. Battlefield 2042
  levels have hundreds of interdependent overlays (dunes, roads, scrub) (GDC 2023, Keable).
- **Unreal runtime virtual texturing (RVT)** (Epic documentation; forum reports): road splines and decals render
  **top-down into the landscape's virtual texture**, so the road becomes part of the terrain's texture at whatever
  resolution the virtual texture gives at that distance. Caveat from users: an RVT is camera-independent, so
  distance-based blends must stay in the landscape material, not in what is written to the RVT.
- **Far Cry 4/5** (GDC 2015 adaptive virtual texturing: 10x10 km at 10 texels/cm; GDC 2018 terrain rendering:
  GPU compute pipeline for terrain LOD, culling, stitching). Road specifics not found in the material read.
- **Ghost Recon Wildlands** (GDC 2017 terrain tools; 80.lv): road network from a terrain-aware **pathfinder**;
  roads **carve the terrain**, and roads re-fit when the terrain is edited.

### What follows for us

The paint-weight trail is the wrong representation: its width is close to the vertex spacing, and every coarser
level samples it more sparsely. A path must be **resolution-independent**:

- **Paths as polylines/splines** (from the trail generator: trace the 0.5 contour into polylines once per cell,
  or author them), with width, material and edge softness.
- **Shaded analytically**: the terrain pixel shader evaluates distance to the path (per-cell segment lists, or a
  low-res signed-distance texture per cell refined analytically) and blends the path material per pixel at every
  level, near and far. Or splat paths into a **terrain virtual texture / macro texture** at each level's resolution
  (more work; also serves decals, tyre tracks, far detail).
- **Carve heights** along the path (a few cm to tens of cm, cross-slope flattening) in the height data, so the path
  reads as a worn track and far levels inherit it.
- Placement exclusion keeps working from the same polylines (already done via the mask).
- For far tiles, the macro colour already exists: write paths into it at its resolution as a minimum.

---

## 3. Terrain level of detail and objects on it

### What others do and run into

- **Floating foliage on lower terrain levels is a known Unreal problem** (forum threads and bug reports UE-60061,
  UE-91110 and others): grass and foliage placed on one level of the landscape float when the landscape draws a
  lower level (higher LOD distribution or LOD bias). Suggested fixes: keep LOD distribution low, collision mip 0,
  sink pivots below the surface. No general fix.
- **Geomorphing** (Unigine docs; CDLOD): vertices move smoothly to the next level's positions before the switch, so
  the rendered surface is continuous, but it is no longer the full-detail surface.
- **Physics vs render mismatch** (Unigine docs: collision uses the most detailed level within a physics range;
  community reports from three.js/Rapier and Bullet users): when the rendered surface is morphed or coarser, things
  standing on the collision surface visibly float or sink.

### Options for us

| Option | How | Cost |
|---|---|---|
| **A. Snap drawn objects to the drawn terrain** | Every placement (trees, rocks, far blocks) and every actor gets its **vertical draw offset from the rendered terrain level** at its root: the vertex/instance shader samples the same height function the terrain uses at that level (far tiles: the coarse grid; loaded cells: the strided level) and moves the whole object by (rendered − true) height. Physics stays on full detail. | Cheap per instance (one height lookup); needs the per-level height available to the object shaders. |
| **B. Refine terrain around important things** | Terrain selection forces full detail (or a small vertical error) within a radius of each important actor and each important object at any distance (a "detail island" in the far tiles). | A few extra tiles per actor; no mismatch at all near actors. |
| **C. Bound the vertical error** | Choose terrain levels by **vertical error in metres** projected to the screen (as now), but cap it at, e.g., 0.3 m within engagement range, whatever the screen error says. | More terrain triangles at mid range. |
| **D. Geomorph far levels** | Smooth transitions between levels so nothing pops. Does not remove the mismatch by itself. | Terrain vertex shader. |

A is the general fix for scenery (it is what makes far forest sit on far hills). B is the fix for actors: a
player at 600 m is drawn on correct terrain, so feet, prone bodies and cover read right. C is a safety net.

---

## 4. Concealment: grass and bushes at distance

### What others do

- **Arma 2/3, DayZ** (Bohemia wiki and feedback tracker): beyond the grass draw distance, units are **sunk into the
  ground** by an amount that depends on **how much grass covers their spot** ("grass layer"), to simulate being
  hidden without drawing grass far away. Players call it imperfect: low-contrast terrain makes a half-sunk figure
  stand out, and PvP players asked to disable it because of hit-box and line-of-sight mismatches. A proposal on
  the tracker: use **transparency** instead of sinking. DayZ's clutter distance scales with a graphics setting, and
  its documentation warns that past the clutter distance the game cannot hide players in tall clutter.
- **PUBG** (community reports; no official statement): grass stops ~100-200 m from the viewer, so a prone player
  who sees grass around themselves is plainly visible to a distant viewer; bushes still render. Scopes do not
  extend grass. Competitive players run low foliage to see more.
- **Hell Let Loose, Arma Reforger** communities raise the same issue.

### Options for us

| Option | How | Notes |
|---|---|---|
| **A. Grass islands around actors** | Generate the ground cover (from the same deterministic hash and paint) in a small radius around every important actor **at any distance**, at a coarse level (few, wider blades or tufts sized for the distance). The viewer then sees the real grass that hides the actor. | Most faithful; cost scales with actor count, not world size. Fits the GPU blade generator ("Next" 2): a tile list that includes tiles around actors. |
| **B. Coverage-aware actor shading** | Beyond grass reach, compute grass coverage at the actor (density x height from paint/hash) and dissolve or darken the actor's lower part up to the grass height with terrain-matched colour (Arma-like, with transparency or dithering instead of sinking). | Cheap; approximate; needs a terrain-matched colour to not stand out. |
| **C. Gameplay visibility from data, not pixels** | AI perception and any server-side spotting use the same coverage function (grass height and density at a point, bush volumes), independent of rendering. | Needed anyway for NPCs and anti-cheat; rendering should agree with it. |

Recommended: **C as the rule, A as the rendering** that matches it, B as a fallback for extreme distances.

---

## 5. Cover and settings: what may differ between players

- **Arc Raiders** (PC Gamer, Icy Veins): the foliage setting removes knee-high bushes and shrubbery at low settings
  (big bushes you can hide in stay); players see this as an advantage for low settings. Suggested fix: simplify,
  do not remove.
- **Hunt: Showdown** (one forum report): lowest LOD settings let players see through distant cover (houses, bushes,
  trees) because the far levels are too coarse.
- **Arma** (feedback tracker): bushes that disappear at some distances and reappear at others.

Rule for this engine:
- Split vegetation into **gameplay-relevant** (anything that can hide a standing, crouching or prone actor: bushes,
  tall grass fields, shrubs, rocks, logs, trees) and **cosmetic** (fine grass, flowers, litter).
- Gameplay-relevant content has a **fixed reach** (at least the engagement range) and **fixed presence** on every
  setting: settings and the detail budget may change its *level* (cheaper meshes, impostors, cards) but never
  remove it. Cosmetic content may thin and shorten with settings.
- Every level of a cover object must keep its **silhouette and opacity** (no see-through far levels): the far
  level of a bush is a solid card cluster or impostor, not nothing.
- Today: scanned bushes end at 80 m; props-layer bushes stop at ~25 m (open bug); the far ring is tufts only. All
  three break the rule.

---

## 6. Important actors: always rendered, rendered right

A render class for players, NPCs, enemies and other gameplay-critical objects:

| Rule | Why |
|---|---|
| Never culled by distance, detail budget or settings; only by the frustum. | A visible enemy must be visible. |
| Occlusion culling only against **solid, conservative** occluders (terrain, buildings, rock), never against vegetation, and with the current frame's depth when possible (two-pass Hi-Z), so a disocclusion never hides them for a frame. | Vegetation occlusion is partial; last-frame occlusion pops. |
| A minimum on-screen size / LOD floor (never below a silhouette-preserving level; a far impostor of a person is a few pixels and must still be there). | Readable at range. |
| Drawn on **correct terrain** (section 3, option B) and **inside correct cover** (section 4, option A). | No floating, no "naked in the field". |
| Shadows always cast (contact and cascade), consistent with the near look. | Shadows give actors away as much as bodies. |
| Same lighting and fog as the scene at that distance (no special highlighting unless it is a game rule). | Fairness. |
| Line of sight / hit tests against the same cover data the renderer uses (collision for solid cover, the coverage function for vegetation). | What you see is what you can shoot. |

In this engine, actors would be ordinary objects outside the instanced vegetation path; the work is in the rules
above: a flag ("important") honoured by culling, the detail budget and terrain level selection.

---

## 7. Options and recommended order

| # | Feature | Fixes | Size |
|---|---|---|---|
| 1 | **Paths as polylines, shaded per pixel at every terrain level** (plus height carving; far tiles' macro colour gets them too) | Spotty trails near and far | Medium |
| 2 | **Objects drawn at the rendered terrain height** (per-instance vertical offset from the drawn terrain level) | Floating/sinking trees, rocks and far blocks on far terrain | Medium |
| 3 | **Gameplay-relevant vegetation with fixed reach and presence** (bushes, tall fields, rocks, logs to the engagement range with solid far levels; the props-layer 25 m bug fixed) | Cover that disappears with distance or settings | Medium |
| 4 | **Important actor render class** (never budget-culled, conservative occlusion, LOD floor, shadows) | Actors culled or degraded like scenery | Small-medium |
| 5 | **Terrain refined around important actors** (detail islands in far tiles) | Actors floating at distance | Medium |
| 6 | **Grass islands around actors** (with the GPU blade generator) + a shared **coverage function** for AI and hit tests | Prone players visible beyond grass reach; render and gameplay disagree | Medium, after "Next" 2 |
| 7 | Terrain virtual texture for far detail (paths, decals, macro colour at each level's resolution) | Far terrain detail in general | Large; later |

1 and 2 are what make the landscape look right today; 3 is the first gameplay rule and also fixes looks (bushes
popping); 4-6 need actors in the demo to test (a test pawn walking and lying in grass at 50-800 m from the
camera, captured from fixed viewpoints).

---

## 8. References

### Paths, roads and terrain texturing
- Croteam, *Four Million Acres, Seriously: GPU-Based Procedural Terrains in Serious Sam 4* (GDC 2019), slides:
  https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd2-4-million-acres-serious-sam-4.pdf
- PC Games Hardware on the same talk:
  https://www.pcgameshardware.de/Serious-Sam-4-Spiel-32391/Specials/GDC-2019-Spielwelt-Map-prozedurale-Inhalte-1281809/
- DICE, *Terrain in Battlefield 3: A Modern, Complete and Scalable System* (GDC 2012):
  https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc12-terrain-in-battlefield3.pdf
- Andersson, *Terrain Rendering in Frostbite Using Procedural Shader Splatting* (SIGGRAPH 2007):
  https://www.advances.realtimerendering.com/s2007/Andersson-TerrainRendering(Siggraph07)-CourseNotes.pdf
- EA, Frostbite terrain procedural tools (Battlefield 2042 overlays): https://ea.com/news/procedural-terrain-in-ea-sports-pga-tour
- Epic, Runtime Virtual Texturing quick start:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/runtimevirtual-texturing-quick-start-in-unreal-engine
- Epic, Virtual texturing: https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-texturing-in-unreal-engine
- Unreal forum, spline decals and RVT distance blending: https://forums.unrealengine.com/t/spline-decals/83966/175
- Unreal forum, projecting spline meshes into RVT:
  https://forums.unrealengine.com/t/is-it-possible-to-project-a-spline-mesh-into-a-runtime-virtual-texture/145702
- Ubisoft, *Terrain Rendering in Far Cry 5* (GDC 2018): https://gdcvault.com/play/1025480/Terrain-Rendering-in-Far-Cry
- Ubisoft, *The Challenges of Rendering an Open World in Far Cry 5* (SIGGRAPH 2018 Advances):
  https://www.advances.realtimerendering.com/s2018/The%20Challenges%20of%20Rendering%20an%20Open%20World%20in%20Far%20Cry%205%20(With%20Notes).pdf
- Ubisoft, *Adaptive Virtual Texture Rendering in Far Cry 4* (GDC 2015): https://gdcvault.com/play/1021760/contactUs
- Ubisoft, *Ghost Recon Wildlands: Terrain Tools and Technology* (GDC 2017): https://gdcvault.com/play/1024029/
- 80.lv, procedural world building in Ghost Recon Wildlands:
  https://80.lv/articles/procedural-world-building-in-ghost-recon-wildlands/

### Terrain levels and objects on them
- Unreal forum, foliage floats when Landscape LOD Distribution is increased:
  https://forums.unrealengine.com/t/landscape-grass-output-foliage-floats-when-landscape-lod-distribution-is-increased/2728244
- Unreal forum, floating foliage on a hill (5.5):
  https://forums.unrealengine.com/t/floating-foliage-on-a-hill-how-to-fix-version-5-5-screenshots/2143756
- Unreal issue UE-60061: https://issues.unrealengine.com/issue/UE-60061
- Unreal forum, landscape LODBias causes grass floating:
  https://forums.unrealengine.com/t/landscapes-lodbias-value-change-causes-grass-floating/439052/3
- Unigine, terrain optimisation (geomorphing): https://developer.unigine.com/en/docs/2.2/objects/objects/terrain/terrain_optimization/?rlang=cpp
- Frostbite, adaptive terrain tessellation:
  https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/adaptive-terrain-tessellation.pdf
- gamedev.net, large terrain rendering, reducing popping:
  https://gamedev.net/forums/topic/688460-large-terrain-rendering-reducing-popping-and-calculating-normals/

### Concealment and fairness
- Bohemia feedback tracker, camouflage by transparency instead of sinking: https://feedback.bistudio.com/T64162
- Bohemia forums, foliage cover and drawing distance:
  https://forums.bistudio.com/topic/124908-foliage-cover-and-drawing-distance/?page=2
- DayZ grass-clutter configuration: https://community.bistudio.com/wiki/DayZ:Grass-clutter_configuration
- Arma 3 vegetation P3D: https://community.bistudio.com/wiki/Arma_3_Vegetation_P3D
- Arma Reforger discussion: https://steamcommunity.com/app/1874880/discussions/0/3419934814485988640
- PUBG discussion (grass distance and prone players): https://steamcommunity.com/app/578080/discussions/1/1621724915784318361
- PC Gamer, Arc Raiders foliage differs by graphics setting:
  https://www.pcgamer.com/games/third-person-shooter/psa-look-out-sweaty-pvpers-arc-raiders-foliage-looks-a-little-different-depending-on-your-graphics-settings
- Icy Veins, do low settings give a PvP advantage in Arc Raiders:
  https://wp-prod.icy-veins.com/do-low-settings-give-a-huge-pvp-advantage-in-arc-raiders/
- Hunt: Showdown discussion (seeing through distant cover at low LOD):
  https://steamcommunity.com/app/594650/discussions/8/3820784984097403274

### Culling
- Haar & Aaltonen, *GPU-Driven Rendering Pipelines* (SIGGRAPH 2015):
  https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
- VTK, hierarchical two-pass occlusion culling:
  https://docs.vtk.org/en/latest/release_details/9.4/webgpu-occlusion-culler.html
