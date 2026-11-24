# Foliage notes

Moved out of todo.md on 2026-11-24: references, budgets and phase notes (see also foliage_performance_research.md). Open work is in todo.md.

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
3. **Cached far shadow cascades** (2.9 ms in the forest): scroll the far cascades and draw trees there in a
   still pose (Fortnite's base pose); keep the near cascade per frame. Our sun is fixed.
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

### Foliage Phase 7: World detail (2026-11-19, user request: paths, streams, small things, cliffs/overhangs)
Asset inventory: the repo's only textured, LOD'd vegetation is the oak pack (17 models, 8 used), nine pack
bushes, twelve scanned rocks and one log (`Assets/Other` also holds an unused `grass_patches_-_circle` and the
`real_bush` source). Everything else is generated. The terrain shader has six fixed paint layers.
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

### Foliage Phase 3: Shadows and sky visibility (dappled light)

### Foliage Phase 4: Forest floor and undergrowth (RDR2 / GTA VI shots)

### Foliage Phase 5: Trees

### Foliage Phase 6: Air and camera

---
