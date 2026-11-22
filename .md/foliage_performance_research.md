# Foliage performance: how other engines do it

Research of 2026-11-22 for the landscape demo: how shipped engines, games and recent demos render dense vegetation
fast, and what applies to this engine. Collected by web search. Most sources are talk abstracts, slides, developer
blog posts or press summaries rather than the talks themselves, so numbers are the sources' own and are not verified
here. Each section says what kind of source it rests on.

The short version is in [todo.md](todo.md) ("Research references (foliage performance)"); this file holds the
details and the references.

## Contents

1. [Where our frame goes now](#1-where-our-frame-goes-now)
2. [The reference: Chlorophyll Engine](#2-the-reference-chlorophyll-engine)
3. [Unreal Engine 5.7: Nanite Foliage](#3-unreal-engine-57-nanite-foliage)
4. [Grass](#4-grass)
5. [Placement and instance management on the GPU](#5-placement-and-instance-management-on-the-gpu)
6. [Alpha-tested leaves, overdraw and the depth pre-pass](#6-alpha-tested-leaves-overdraw-and-the-depth-pre-pass)
7. [Culling](#7-culling)
8. [Mesh shaders, clusters and work graphs](#8-mesh-shaders-clusters-and-work-graphs)
9. [Far forest: impostors and voxels](#9-far-forest-impostors-and-voxels)
10. [Shadows](#10-shadows)
11. [Recommendations for this engine](#11-recommendations-for-this-engine)
12. [Reference list](#12-reference-list)

---

## 1. Where our frame goes now

Measured on the RTX 4090 at 1920x1080 after shared prefab parts (Next 1, `ENGINE_SHARED_PREFAB_PARTS=1`),
interleaved 400-frame walks, 2026-11-22:

| Walk | Frame median | Main-thread CPU (update + prepare + camera + shadow record) | GPU, largest parts |
|---|---|---|---|
| Forest | 15.4-15.7 ms | 1.4 + 1.5 + 5.6 + 2.4 ≈ 10.9 ms | ~15 ms: opaque 8.0 (foliage depth pre-pass 2.5-4.4 of it), shadows 2.9, grass 2.6 |
| Field | 18.5-18.6 ms | 1.7 + 1.6 + 5.7 + 2.7 ≈ 11.7 ms | ~17.5 ms: grass 9.9, opaque 4.8 |
| Walk + turning | 16.3-17.4 ms | 1.7 + 1.7 + 6.1 + 2.8 ≈ 12.3 ms | opaque 8.2, grass 3.3, shadows 2.9 |

The frame is GPU-bound now. The questions for this research are therefore:

- **Grass** (9.9 ms in the field): how others draw large meadows.
- **Leaves** (opaque 8 ms in the forest, 2.5-4.4 of it the foliage depth pre-pass): alpha-tested cards and overdraw.
- **Shadows** (2.9 ms in the forest).
- **Far forest**: look and memory more than time today (Next 5).

What we already have, for comparison: GPU per-placement culling and level choice (`InstanceCull.hlsl`), GPU Hi-Z
occlusion, a persistent placement pool, one shared object per prefab part across all cells (Next 1), indirect draws
(fixed count 1 per batch on DX12), cluster impostors for the far forest, alpha-tested leaf cards on the oaks,
generated grass tufts as instanced meshes on CPU-built tiles.

---

## 2. The reference: Chlorophyll Engine

*Sources: wccftech article of 2026-11-04 (read in full), hwupgrade.it article, Hacker News link to the video,
GitHub issues of other projects that use it as a target. No developer write-up was found.*

The demo video this project's look targets ("Chlorophyll Engine 10M Trees - 15km² Test") is by a developer known as
**Toni Macaroni** (X: [@ToniMacaroniy](https://x.com/ToniMacaroniy), YouTube:
[@tonimacaroniy](https://www.youtube.com/@tonimacaroniy), GitHub: [ToniMacaroni](https://github.com/ToniMacaroni)).
Neither article names the engine's internals beyond the points below; the engine is not public.

**Engine**
- A "largely handwritten custom engine", about seven months of work (per an earlier X post).
- An earlier project by the same developer (August 2025, 80.lv): a Godot forest scene at ~170 fps uncapped on an
  RTX 3080, with custom vegetation systems and Houdini-assisted terrain.

**Geometry and levels of detail**
- **Clustered levels of detail (CLOD)**: "Nanite-like virtual geometry". Meshes are split into clusters that are
  simplified and selected per cluster, not per object.
- **Voxels for distant vegetation, "rather than traditional impostors".**
- The farthest objects are simplified with **proxies that drop per-instance data** (a stand of trees is no longer a
  list of trees).
- Scale quoted: one example with "around one million instances and four million triangles"; an image caption says
  "3 million trees, ~4 million polys each"; the video title says 10M trees on 15 km².

**Lighting**
- Screen-space techniques with **voxel-based global illumination**, no hardware ray tracing (wccftech compares it to
  the SVOGI of Kingdom Come: Deliverance II; that comparison is the writer's).
- Shadows, ambient occlusion, culling and grass are not described anywhere found.

**Frame rate** (developer-reported, settings and upscaling not stated; one caption says "No DLSS 5")
- ~130 fps average, ~80 fps in dense forest, up to ~200 fps from the air, RTX 3080 at 2560x1440.

**What other projects infer** (a Meridian issue on GitHub, rated medium confidence there): far trees lit as volumes
with sun visibility.

---

## 3. Unreal Engine 5.7: Nanite Foliage

*Sources: Epic documentation (Nanite Foliage, Nanite Assemblies), UE 5.7 release post, 80.lv, PC Gamer on the
Witcher 4 demo, Epic forum threads (user reports).*

Experimental in 5.7. Three parts:

- **Nanite voxels**: past a distance, foliage turns into near-pixel-sized aggregate voxels that keep the detail,
  animation and material response of the triangles they replace. Distant trees become a voxel version of
  themselves; no hand-made impostor or LOD chain.
- **Nanite assemblies**: a tree is built from reusable parts (branches, leaf clusters) instanced inside the asset.
  Epic says this is what makes a large forest fit at all: without it there would not be enough disk or streaming
  memory for a forest like the Witcher 4 demo's.
- **Nanite skinning**: wind and bending as skinned animation of the assemblies, on the GPU.

Results and reports:
- The Witcher 4 tech demo (State of Unreal, June 2025) ran at 60 fps on a base PS5 with ray tracing; PC Gamer
  credits Nanite Foliage for most of the dense forest and its frame rate. CD Projekt says the demo is a tools
  showcase, not the final game.
- One user reported 62 → 119 fps rendering 77,376 trees of 20 million polygons each (anecdotal).
- Known problems (forum): holes when a mesh becomes small on screen and turns into voxels (5.7.4); ray-traced
  shadows of voxelised foliage look wrong; for virtual shadow maps with voxels, users advise **shadow caching** for
  large foliage scenes. Epic's documentation warns the settings may change or be removed.

Why it matters here: both the reference engine and Epic replace impostors with voxels for distant vegetation, and
both avoid alpha-tested cards near the camera by drawing real (clustered) leaf geometry.

---

## 4. Grass

### Ghost of Tsushima (Sucker Punch, GDC 2021, "Procedural Grass in Ghost of Tsushima", Eric Wohllaib)

*Sources: GDC Vault listing (talk not accessible), reimplementations (GodotGrass, Unity Grass by Cain Rademan),
secondary blog posts.*

- Every blade is **generated on the GPU** and has its own procedural shape and animation; fields of grass are the
  art direction's centrepiece (Jin rides through them).
- Blades are grouped in **clumps** from a procedural **Voronoi** cell pattern; each cell gets a clump id that indexes
  a list of artist-defined clump types (height, width, bend, facing, colour).
- Blades facing away from the camera (seen edge-on) are **widened in view space**, so a field does not thin out
  when seen side-on.
- Blades are bent by height procedurally, plus wind.
- Cost (secondary source, unverified): ~83,000 blades on screen in ~2.5 ms.
- Sources disagree on the exact pipeline (geometry shader vs vertex shader expanding a V-shaped blade model).

### Ghost of Yōtei (Sucker Punch, PlayStation Blog tech deep dive, 2025, Adrian Bentley)

*Sources: the blog post (fragments in search results), GamingBolt, 80.lv, Automaton interview.*

- Longer sight lines were a design goal, which needed better grass, terrain and mountains at distance.
- **Instance management moved to the GPU**, with aggressive culling before the G-buffer; reported to allow about
  **double the grass and objects** of the predecessor. Distant mountains are baked into textures.
- Most grass, flowers and small plants can be cut by the player.
- On PS5 Pro, PSSR reconstructs fine foliage more cleanly than their older upscaler (helped by conservative
  rasterisation for tiny particles).

### AMD: procedural grass with mesh shaders and work graphs (GPUOpen, GDC 2024)

*Sources: GPUOpen blog posts and preprint.*

- Grass patches are generated in **16x16 blocks**; **one mesh-shader workgroup produces one patch** of grass.
- With work graphs, each biome tile's thread decides what to generate (grass, flowers, insects) and launches the
  mesh nodes that draw it; nothing is stored for the whole world.
- AMD reports ExecuteIndirect 1.64x slower than work graphs with mesh nodes in their demo (RX 7900 XTX,
  pre-release driver; their own test).

### Common techniques in public grass implementations

*Sources: developer blogs and sample projects (giordi91, j111m, Bliss, Jahrmann & Wimmer 2017, ensapra).*

- **LOD in the cull**: the compute cull computes visibility and a level from distance bands, and writes one indirect
  draw per level.
- **Levels by blade geometry**: e.g. 9, 5 and 1 triangles per blade, all levels in one vertex buffer; or fewer and
  wider blades with distance.
- Expanding blades in a compute shader avoids per-level draws but costs memory and bandwidth.
- Frame time grows steeply with blade count; frustum (and occlusion) culling is the largest win; halving count by
  distance without widening looks bad.
- Jahrmann & Wimmer, *Responsive Real-Time Grass Rendering for General 3D Scenes* (I3D 2017): blades as Bézier
  curves, culled per blade (orientation, view frustum, distance, occlusion) in compute, then tessellated.

### Compared with ours

Our grass is generated tufts and tall-grass crowns (up to ~1,800 triangles each) instanced on tiles built on the CPU,
with per-plant levels. It costs 9.9 ms in the field. The approaches above draw individual blades produced on the GPU
from a hash, choose detail per blade, and widen blades at distance: see "Next" 2 in `todo.md`.

---

## 5. Placement and instance management on the GPU

### Horizon Zero Dawn (Guerrilla, GDC 2017, "GPU-Based Run-Time Procedural Placement", Jaap van Muijden)

*Sources: Guerrilla's published slides (PDF), 80.lv articles.*

- **All vegetation is placed procedurally**, moved from a CPU system to compute shaders that populate the world
  around the player at run time.
- Data-driven, **deterministic and locally stable** (the same place always grows the same plants), fast to iterate,
  art-directable.
- **Ecotopes** describe the environment; they decide asset types, distribution and attributes. World data is stored
  as 2D maps, about **4 MB per km²**.
- Scale: **500+ asset types, 100,000+ objects** in the scene at an average GPU cost of **~250 µs**.
- The pipeline is instantiated 64 times, each instance emitting one layer.

### Assassin's Creed Unity (Ubisoft, SIGGRAPH 2015, "GPU-Driven Rendering Pipelines", Haar & Aaltonen)

*Sources: the published course slides; secondary write-ups.*

- Mesh **clusters**: any number of meshes in one draw call, culled on the GPU by cluster bounds.
- Depth pyramid built for occlusion: ~100 µs downsampling plus ~50 µs for the hierarchy on PS4.
- Motivation: ~10x the instances of earlier games with the CPU as the scarcest resource.
- Reported results were modest for geometry alone (20-40 % of triangles culled, ~10 % faster geometry); the gain was
  CPU. Aaltonen later called it the first shipped game with a GPU-driven renderer.

### Compared with ours

Next 1 took the per-cell objects out (one shared object per prefab part, blocks of placements per cell); the GPU
already culls per placement. What Horizon adds is **placement generated on the GPU** from maps, which is the plan for
ground cover in "Next" 2 and could later replace CPU-generated far cells.

---

## 6. Alpha-tested leaves, overdraw and the depth pre-pass

### Horizon Forbidden West (Guerrilla, GDC 2022, "Adventures with Deferred Texturing in Horizon Forbidden West", James McLaren)

*Sources: GDC Vault listing, Guerrilla's article, Beyond3D forum summary.*

- To speed up **foliage and alpha-tested geometry**, the game uses a "loosely tiled" **deferred texturing** system
  (a visibility buffer):
  1. a visibility pass writes which triangle covers each pixel (cheap; alpha test only);
  2. analysis and tiled G-buffer generation run as compute jobs;
  3. shading of the visible pixels only.
- The compute work runs asynchronously, overlapping G-buffer and cascaded shadow rendering, so its cost is mostly
  hidden.
- Related: Eidos-Montréal's Dawn Engine "Deferred+" (decoupling geometry from materials and lighting); UE5 Nanite
  made visibility buffers common again.

### Measurements and opinions on overdraw

*Sources: Unreal forum tests, an old gamedev.net thread, a 2026 blog post (unverified references).*

- Unreal 4.17 test: masked (alpha-tested) foliage cost ~3 fps over opaque in that scene; translucent was much worse;
  high-triangle opaque leaves ran about as fast as masked cards. Overdraw was the main cost.
- Another Unreal test: at the same triangle count, cards with less overdraw (tighter to the alpha) were much faster;
  at distance, real geometry doubled the triangle count and lost. Advice: cut overdraw near, keep far levels simple.
- Pre-pass practice: the depth pre-pass writes only fragments that pass the alpha test; engines commonly sort
  foliage front to back before the pre-pass (claim from one 2026 blog; its references could not be checked).
- Emil Persson, *Particle trimming*: fitting card geometry to the alpha roughly halves alpha-tested overdraw.

### Compared with ours

The oaks use alpha-tested leaf cards with a foliage depth pre-pass (2.5-4.4 ms in the forest walk) and then a shading
pass. Cheap steps: tighter cards, front-to-back order inside a batch, and measuring the pre-pass on and off per pose.
The structural change is Forbidden West's visibility buffer for foliage; the alternative (Nanite Foliage, the
reference engine) is real leaf geometry with cluster LODs and no alpha test.

---

## 7. Culling

*Sources: Haar & Aaltonen 2015, VTK's two-pass occlusion culler documentation, a Vulkanised 2026 talk (Qualcomm),
gamedev.net threads, the HROC paper (Eurographics).*

- **Two-pass Hi-Z occlusion**: draw what was visible last frame, build a depth pyramid from it, cull everything else
  against the pyramid, draw what passes. Reprojecting last frame's depth instead needs hole filling and misses
  disocclusions (HROC notes the two-pass scheme fixes those).
- On tiled (mobile) GPUs, splitting the pass around a compute dispatch costs > 0.5 ms; not an issue on desktop.
- For small scenes, building the depth pyramid every frame can cost more than CPU frustum culling saves (VTK).
- Alan Wake 2 culls with **meshlet granularity** in mesh shaders, at single-pixel precision.

Ours: GPU per-placement frustum and Hi-Z occlusion exist; per-block CPU frustum culling came with Next 1. Whether our
Hi-Z is single-pass (previous frame) or two-pass was not checked in this research.

---

## 8. Mesh shaders, clusters and work graphs

### Alan Wake 2 (Remedy, Northlight; GDC and REAC 2024, Digital Dragons 2024)

*Sources: Remedy's blog posts, wccftech interview, PC Gamer, Tom's Hardware.*

- The geometry pipeline was rewritten from material definition to rasterisation: **GPU-driven with mesh shaders**,
  culling down to meshlets.
- Forests use a shader-based vegetation system on **GPU skinning**: almost **300,000 bones a frame** at Cauldron
  Lake (GDC talk "Large Scale GPU-Based Skinning", Kiya Kandar).
- The game requires mesh-shader hardware, which excluded most older GPUs at launch.

### Work graphs and mesh nodes (AMD, Microsoft)

*Sources: GPUOpen, DirectX developer blog, Kuth et al. (ACM TOG 2024), press.*

- Work graphs let the GPU schedule its own work, including draws ("mesh nodes" launch mesh-shader pipelines), so no
  output memory is reserved for the whole world.
- AMD's tree generation study replaced ~35 GiB of stored trees with a few KiB of generator data (outlets quote
  slightly different numbers).
- Limits: mesh nodes started on DirectX 12 (Vulkan later); amplification shaders cannot be combined with mesh nodes;
  driver support is recent.

### Other cluster/virtual-geometry forests

- **BroadLeaf** (Tencent America, Yixin Hu; 80.lv): automatic LOD generation and a GPU-driven pipeline for large
  forests, compared against Nanite.
- **GodotCon Amsterdam 2026, "The long road to a million trees"**: 200-800k trees in frame without cards or manual
  LODs, borrowing the voxel approach for distant objects.
- **Real-Time GPU Tree Generation** (Eurographics 2025, GPUOpen): trees generated procedurally on the GPU, 7.74 ms
  median frame time for generation and rendering in its test scene.
- Several WebGPU/three.js projects (threejs-nanites: 1M trees and 1.5M bushes with Nanite-style cluster LOD;
  webgpu-landscape-rendering: millions of GPU-culled trees with impostors and long-range shadows).

---

## 9. Far forest: impostors and voxels

### Octahedral impostors (Ryan Brucks, shaderbits.com 2018; Epic Impostor Baker; Fortnite)

*Sources: Epic documentation, a Unity forum post about Fortnite, Godot and three.js implementations, Unigine and
Simplygon documentation.*

- A tree is rendered from many directions into an atlas (octahedral or hemi-octahedral layout) with colour, alpha,
  **normals and depth**; at run time the nearest views are blended, and depth lets impostors intersect and
  composite properly. Normals let them relight.
- Fortnite (Battle Royale, before Nanite): **hemispherical impostors for all trees**; each impostor uses a 2048
  colour/alpha atlas plus a 1024 normal/depth atlas. A community post says impostors removed ~500k triangles in one
  wooded area, several ms on some hardware. The cost is texture memory.
- For trees, hemispheres are better than full spheres (more resolution for side views).
- Baked normals for vegetation come out rounded (Unigine notes), which is usually what foliage wants.

### Voxels

- Chlorophyll Engine and UE 5.7 Nanite Foliage both replace impostors with voxel representations for distant
  vegetation (sections 2 and 3). Benefits claimed: no view popping or atlas memory per tree, keeps material response;
  for the reference engine, the voxels also feed its voxel GI.
- Known issue in UE 5.7: holes where meshes go voxel.

Ours: cluster impostors for far forest (several trees per impostor), flat front-lit with no shadow between crowns and
no blue shift with distance ("Far forest texture and haze" in `todo.md`; "Next" 5).

---

## 10. Shadows

*Sources: Insomniac (SIGGRAPH 2012), CRYENGINE documentation, Cyberpunk 2077 (SIGGRAPH 2021 talk), Epic's Fortnite
virtual shadow map blog, Unreal forum reports.*

- **Scrolling cached cascades** (Insomniac): static geometry goes into a cached shadow map that scrolls with the
  camera; only the edges exposed by the scroll are re-rendered. Assumes the camera moves a lot but slowly. Objects
  count as static after not moving for a while (e.g. five seconds).
- **Cached cascades** (CRYENGINE): far cascades are drawn once and then only sampled; re-centred when the camera
  nears their border; dynamic objects are left out of the cache and drawn into the normal cascades.
- **Cyberpunk 2077**: near and dynamic objects in per-frame cascades, the rest through a separate cached path.
- **Foliage is the hard case** (Fortnite, virtual shadow maps): animated trees and a moving sun break caching. Their
  answer: draw animated casters into the cache in a **base pose** and accept slightly wrong shadows, which reads like
  baked lighting. A light direction change throws the cache out (small sun movement is often tolerated).
- Unreal users report foliage in cascaded shadows as a main cost.

Ours: 2.9 ms of shadow GPU in the forest; the sun is fixed in the demo, which makes far-cascade caching easier than
in Fortnite.

---

## 11. Recommendations for this engine

Ordered by expected gain on the measured costs in section 1.

| # | Change | Targets | Basis | Size |
|---|---|---|---|---|
| 1 | **Grass as GPU-generated blades** ("Next" 2): per-tile blades from a hash in compute, Voronoi clumps, per-blade level, fewer and wider blades with distance, widen edge-on blades. Shrubs, ferns, bushes stay meshes. | Field: grass 9.9 ms → target ≤ 4 ms | Tsushima, Yōtei, AMD, Horizon placement | Large |
| 2 | **Measure the foliage depth pre-pass and leaf overdraw**, then the cheap fixes: tighter leaf cards, front-to-back order inside batches. If it is still large: a visibility buffer for alpha-tested foliage. | Forest: 2.5-4.4 ms pre-pass, part of opaque 8.0 | Forbidden West, Persson, Unreal tests | Small → large |
| 3 | **Cached far shadow cascades**: scroll the far cascades, draw trees there in their rest pose (no wind), keep the near cascade per frame. | Forest: shadows 2.9 ms | Insomniac, CRYENGINE, Cyberpunk, Fortnite | Medium |
| 4 | **Far forest as voxels or octahedral impostors with normal and depth** ("Next" 5). | Look (crown texture, shading, haze) and memory more than time | Chlorophyll, UE 5.7, Fortnite | Medium-large |
| 5 | **Cluster LODs and mesh shaders for trees**: real leaf geometry without alpha test, culled per cluster. | Leaves and the pre-pass at the root | Chlorophyll, Nanite, Alan Wake 2 | Largest; only after 1-3 |
| 6 | Work graphs | — | AMD | Watch; too young |

Notes:
- 1 is already the planned next step ("Next" 2) and is the largest single cost.
- 2 starts with measurement because the pre-pass cost varies 2.5-4.4 ms between walks; the cheap fixes may be
  enough at our tree counts.
- 3 is easier for us than for Fortnite because the demo's sun does not move.
- 5 is what the reference engine is built on, but it changes the asset pipeline (cluster building, LOD generation)
  and needs mesh shaders (the RTX 4090 has them on DX12 and Vulkan; DX11 would keep today's path).

---

## 12. Reference list

### Chlorophyll Engine
- wccftech, *Custom Forest Engine Delivers Stunning Visuals at 130 FPS on an RTX 3080 at 1440p, Without Hardware Ray
  Tracing* (2026-11-04):
  https://wccftech.com/custom-forest-engine-delivers-stunning-visuals-at-130-fps-on-an-rtx-3080-at-1440p-without-hardware-ray-tracing/
- hwupgrade.it, *Chlorophyll Engine fa paura ai tripla A: 10 milioni di alberi su 15 km²*:
  https://gaming.hwupgrade.it/news/videogames/chlorophyll-engine-fa-paura-ai-tripla-a-10-milioni-di-alberi-su-15-km%EF%BF%BD-con-una-grafica-pazzesca_162431.html
- Hacker News, *Chlorophyll Engine 10M Trees* (video link): https://news.ycombinator.com/item?id=50552358
- Reddit, *Custom Engine Forest Showcase (130 fps average on RTX 3080 1440p)*:
  https://www.reddit.com/r/pcmasterrace/comments/1xnlucj/custom_engine_forest_showcase_130_fps_average_on/
- Developer: https://x.com/ToniMacaroniy, https://www.youtube.com/@tonimacaroniy, https://github.com/ToniMacaroni
- 80.lv, earlier Godot forest demo by the same developer:
  https://80.lv/articles/godot-developer-showcased-impressive-forest-scene-demo
- Meridian issue (inference about distant forest lighting): https://github.com/zye04/Meridian/issues/1

### Unreal Engine
- Nanite Foliage documentation: https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage
- Nanite Assemblies documentation: https://dev.epicgames.com/documentation/unreal-engine/nanite-assemblies?lang=en-US
- Unreal Engine 5.7 release: https://www.unrealengine.com/news/unreal-engine-5-7-is-now-available
- 80.lv, Nanite Foliage with voxel representation:
  https://80.lv/articles/get-a-glimpse-of-nanite-foliage-with-voxel-representation-in-ue5-7
- Forum: VSM performance with Nanite voxelize (5.7.2):
  https://forums.unrealengine.com/t/ue-5-7-2-vsm-performance-with-nanite-voxelize-and-r-shadow-nanitelodbias/2706943
- Forum: Nanite voxelization holes: https://forums.unrealengine.com/t/nanite-voxelization-holes-in-the-mesh/2723578
- PC Gamer, Witcher 4 and Nanite Foliage:
  https://www.pcgamer.com/hardware/the-witcher-4s-leafy-glory-is-all-down-to-epics-nanite-foliage-and-largely-so-is-the-fact-it-can-run-ray-traced-at-60-fps-on-a-lowly-ps5
- PlayStation Blog, Witcher 4 tech demo: https://blog.playstation.com/?p=405351
- Impostor Baker plugin documentation:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/impostor-baker-plugin-in-unreal-engine
- Virtual Shadow Maps in Fortnite Battle Royale Chapter 4:
  https://www.unrealengine.com/en-US/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4

### Grass
- GDC Vault, *Procedural Grass in Ghost of Tsushima*: https://gdcvault.com/play/1027214/contactUs
- GodotGrass (Tsushima-style reimplementation): https://github.com/2Retr0/GodotGrass
- Unity Grass (Cain Rademan): https://cainrademan.github.io/Unity-Grass/
- Hashnode, *What We Can Learn From Grass in Ghost of Tsushima*:
  https://hashnode.com/posts/what-we-can-learn-from-grass-in-ghost-of-tsushima-renders/69e130b2ffbb787634eb9ef4
- PlayStation Blog, *Ghost of Yōtei – tech deep dive*: https://blog.playstation.com/?p=411201
- GamingBolt on Ghost of Yōtei:
  https://gamingbolt.com/ghost-of-yotei-devs-explain-how-they-made-its-vast-reactive-world-run-fast-on-ps5
- GPUOpen, procedural grass rendering with mesh shaders:
  https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/
- Grass LOD write-ups: https://giordi91.github.io/post/grass, https://j111m.github.io/posts/Grass/,
  https://github.com/IwakuraRein/Bliss
- Jahrmann & Wimmer, *Responsive Real-Time Grass Rendering for General 3D Scenes* (I3D 2017):
  https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf

### Placement and GPU-driven rendering
- Guerrilla, *GPU-Based Procedural Placement in Horizon Zero Dawn* (slides):
  https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf
- Haar & Aaltonen, *GPU-Driven Rendering Pipelines* (SIGGRAPH 2015):
  https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
- vkguide, GPU-driven engines overview: https://vkguide.dev/docs/gpudriven/gpu_driven_engines/

### Alpha-tested foliage and overdraw
- Guerrilla, *Adventures with Deferred Texturing in Horizon Forbidden West*:
  https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west
- Eidos-Montréal, *Deferred+: Next-Gen Culling and Rendering for Dawn Engine*:
  https://eidosmontreal.com/news/deferred-next-gen-culling-and-rendering-for-dawn-engine/
- Unreal forum, opaque vs masked vs translucent foliage test (4.17):
  https://forums.unrealengine.com/t/test-of-instanced-foliage-and-overdraw-with-opaque-masked-and-translucent-in-4-17/100351
- Unreal forum, foliage overdraw testing: https://forums.unrealengine.com/t/foliage-overdraw-testing/80905
- Cinevva, *How modern engines beat foliage overdraw* (2026; references unverified):
  https://app.cinevva.com/blog/2026-05-11-foliage-overdraw
- Wyman & McGuire, *Hashed Alpha Testing* (NVIDIA, 2017):
  https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf

### Culling
- VTK, hierarchical two-pass occlusion culling:
  https://docs.vtk.org/en/latest/release_details/9.4/webgpu-occlusion-culler.html
- Vulkanised 2026, Lucas Silva (Qualcomm):
  https://vulkan.org/user/pages/09.events/vulkanised-2026/1415-Lucas-Silva-DevshGraphicsProgramming.pptx.pdf

### Mesh shaders and work graphs
- Remedy, *How Northlight makes Alan Wake 2 shine*: https://remedygames.com/article/how-northlight-makes-alan-wake-2-shine
- wccftech, Alan Wake 2 mesh shaders interview:
  https://wccftech.com/alan-wake-2-dev-talks-integration-of-mesh-shaders-dlss-3-5-explains-work-graphs-importance-in-future-games/
- Remedy, GDC 2024 talks: https://www.remedygames.com/article/explore-remedys-gdc2024-talks-on-creating-alan-wake-2
- GDC Vault, *Large Scale GPU-Based Skinning*: https://gdcvault.com/play/1034310/Large-Scale-GPU-Based-Skinning
- GPUOpen, work graphs mesh nodes, procedural generation:
  https://gpuopen.com/learn/work_graphs_mesh_nodes/work_graphs_mesh_nodes-procedural_generation/
- Microsoft DirectX blog, mesh nodes in work graphs: https://devblogs.microsoft.com/directx/d3d12-mesh-nodes-in-work-graphs/
- GPUOpen, *Real-Time GPU Tree Generation* (Eurographics 2025): https://gpuopen.com/download/Real-Time_GPU_Tree_Generation.pdf
- 80.lv, BroadLeaf: https://80.lv/articles/broadleaf-developing-a-real-time-solution-for-rendering-trees/
- GodotCon Amsterdam 2026, *The long road to a million trees*: https://talks.godotengine.org/godotcon-ams-2026/talk/DL7SJJ/
- GodotFest 2025, *Plants, Polygons and Pixels* (Karl Bittner):
  https://media.ccc.de/v/godotfest2025-plants-polygons-and-pixels-large-scale-vegetation-rendering-in-godot
- threejs-nanites: https://github.com/ahzs645/threejs-nanites

### Impostors
- Godot Octahedral Impostors: https://github.com/SIsilicon/Godot-Octahedral-Impostors
- Octahedral impostors write-up: https://smailikhappy.github.io/posts/Octahedral_imposters/
- Unigine impostors creator: https://developer.unigine.com/docs/future/editor2/tools/impostors_creator
- Unity forum, HLOD and streaming octahedral impostors (Fortnite numbers):
  https://discussions.unity.com/t/hlod-streaming-octahedral-impostors-using-megacity-subscenes/742223

### Shadows
- Insomniac, *CSM Scrolling* (SIGGRAPH 2012):
  https://advances.realtimerendering.com/s2012/insomniac/Acton-CSM_Scrolling(Siggraph2012).pdf
- CRYENGINE, cached shadows:
  https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/21267738
- Dybisz, *Shadows Optimizations in Cyberpunk 2077* (SIGGRAPH 2021):
  https://history.siggraph.org/wp-content/uploads/2022/06/2021-Talks-Dybisz_Shadows-Optimizations-in-Cyberpunk-2077.pdf
