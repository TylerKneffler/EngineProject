# Foliage: reference look at 60+ fps

Goal (2026-11-19): forests, undergrowth and grass that read like the reference shots (RDR2 forest floor,
GTA VI undergrowth and coastal palms, mangrove channel, backlit pine) **without dropping below 60 fps**
at 1920x1080 on the RTX 4090 (16.7 ms at the heaviest pose; keep aiming for 100+ on typical poses).

Companion to [grass.md](grass.md), which covers blade coverage, fades and the far grass ring. This doc covers
light, trees, undergrowth, atmosphere and the performance needed to pay for them. Line numbers are at
`88efe11` and will drift.

---

## 1. What the references have that we don't

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

**Performance is already short:** the fully settled vista measures **42.8 ms** (Phase 0, 2026-11-19), and it is
CPU-bound: draw recording (25 ms) + scene prepare (11 ms) + update (5 ms) run back to back, while the GPU needs
~13 ms. Grass is 3.6-3.8 ms on grass_test. So better-looking foliage has to come with savings, not just new
cost, and the vista needs CPU work before GPU work.

## 2. Frame budget (16.7 ms, 1920x1080, DX12)

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

## 3. Research references

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

## 3b. Open list (kept current; details in the phases below)

**Did not work / reverted**
- Glossier oak leaves (roughness 0.6): white sun glints on crowns; reverted.
- Raising the meadow ground tint (0.5 → 0.8): no effect on the pale ground; reverted.
- Shifting the dissolve dither per frame: left diagonal hatching; replaced by rotating thresholds.
- Generated cliff blocks and crags: an outcrop, not a cliff (see Phase 7).
- Single-run CPU timings: machine-wide swings of ±30 % hid every CPU change; only profiler shares or
  interleaved A/B pairs are usable.

**Not done**
- 60 fps: ground views are 25-27 ms, the vista ~22-28 ms, CPU-bound (update + prepare + record run
  serially). GPU is 11-13 ms.
- Streams and rivers; vines on trees; more than six terrain paint layers (mud, moss, litter, scree).
- Scanned cliffs are in (five Poly Haven rock faces); scanned undergrowth (ferns, shrubs, stumps, dry
  branches are available there) is not imported yet.
- Bounce lighting, contact shadows, soft shadow penumbra, froxel fog, depth of field.
- Oak-pack impostors not re-baked with the new leaf lighting; Vulkan has no TAA.
- Pale ground between grass blades in the test meadow; blade-mottling cost not measured.
- Standalone (non-instanced) prefab trees: 64 ms shadow submission.
- Nothing is committed: all of this work is uncommitted in the working tree.

**To investigate**
- [ ] **Diagonal stripes on plant materials** (user screenshot, 2026-11-19): regular dark diagonal bands across
      the broad leaves of generated ground plants (forest herbs / ferns / bushes, drawn by the grass
      shader `PSGrassMain`). Suspects, in order: (1) shadow acne - these leaves take the sun shadow with a
      one-tap lookup biased along the leaf's geometric normal, and large flat leaves at grazing sun angles
      band like this (the same pattern showed on bushes in early captures, e.g. `bush_crop.png`); (2) the
      new canopy-shade taps (`CanopySkyVisibility`, five unfiltered bilinear shadow taps) banding on
      surfaces near shadow edges; (3) the placement LOD dissolve dither. Test: `ENGINE_TAA=0/1`, shadows
      off on the grass layer, and canopy shade off, one at a time.
- [ ] Mossy rock scans read as large orange sandstone boulders in the forest (little moss in the texture
      at 2k, warm under AgX, scale too big): retint / shrink, or pick a different scan.
- [ ] Scanned props raised draw counts (ForestFloor 292 → 367 submitted, 1371 → 1825 candidates) and the
      ground views now measure ~31 ms (CPU record ~19 ms; single runs): the per-asset CPU cost again.

**Next, in order**
1. Import scanned plants (fern_02, shrub_02-04, nettle_plant, tree_stump_01/02, dry_branches_medium_01,
   rock_moss_set_01/02) with `fetch_polyhaven_props.py` (alpha-masked ones need card-friendly LODs) and
   replace the generated bushes; finish the cliff follow-ups.
2. CPU pipeline toward 60 fps.
3. Streams: carve channels into the height archive, sloped water surfaces.

## 4. Implementation plan

Ordered by payoff per cost. Phase 1 is shader and asset-cook work with almost no frame cost and should
close most of the visual gap on trees. Phase 2 buys the headroom for Phases 3-6.

### Phase 0: Baseline and look targets
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
- [ ] Save crops of the reference shots next to each pose's capture in `.local/foliage/` for A/B.

### Phase 1: Leaves lit as volumes (biggest visual win, ~free)
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
- [ ] Impostors and forest clusters: not re-baked, so they still carry the old flat leaf lighting.
- [ ] **Sky ambient for grass and meadow plants**: `PSGrassMain` / `PSFoliageMain` still use the flat
      `ambientUnlit` instead of SH irradiance on the (pelt/crown) normal. Changes meadow brightness
      everywhere; needs its own tuning pass with grass.md.
- [ ] `wetness` scene value (lower roughness, darker albedo) for the GTA VI rain look.
- [x] Check (foliage_test, `.local/foliage/{base,p1e}_*.png`): Backlit crowns have dark interiors and lit,
      glowing shells toward the sun (before: evenly lit flat green with white glints); looking up from
      ForestFloor the canopy has depth instead of uniform bright green; Meadow crowns read as rounded.
      Leaf GPU change +0.02-0.08 ms (ForestFloor 0.72 → 0.77, Backlit 0.56 → 0.64, Meadow 0.31 → 0.33);
      other stage differences are run-to-run noise. Tuning runs: p1a = translucency only (uncooked: whole
      crown glows evenly), p1b = cooked, p1c = + gloss, p1e = + darker undersides / halved ambient.
- [x] Vista (`.local/foliage/p1g_Vista`): far forest unchanged (impostors/clusters dominate, no visible
      near/far mismatch at this pose); GPU leaves 2.01 → 1.59 ms, GPU stage sum 13.4 → ~11 ms, i.e. no
      regression. Frame time can't be compared today: four runs with no relevant change measured
      42.8 / 30.7 / 27.7 / 21.9 ms, all stages moving together (CPU load on the machine).
      Final test-pose numbers (p1): ForestFloor 7.5 ms / leaves 0.76, Backlit 7.7 / 0.59, Meadow 4.6 / 0.33.

### Phase 1b: Blocky bushes and the landscape demo (2026-11-19, user report: "bushes look blocky")
- [x] **Cause: grass LOD measured from height 0.** GrassField tile objects sat at y = 0 with ground heights
      inside the placements, so LOD selection and fades measured distance from a point tens (test cell) to
      hundreds (landscape) of metres above or below the plants: nearly every grass plant and bush drew LOD2
      at the viewer's feet. Fixed with `TileState::originHeight` (details in [grass.md](grass.md) Updates).
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

### Phase 1c: Tree variety (2026-11-19, user report: "a lot of the same trees")
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
- [ ] Oak pack impostors (`*_Impostor` from the pack's Billboard_Mat) are not re-baked.
- [ ] More models without the per-asset CPU cost: needs batches that merge across assets (one instanced
      draw over several meshes sharing a material), or per-instance shape variation beyond scale and lean.
- [ ] Whitish highlights on crowns next to the sun in Backlit (seen before this change too).

### Phase 1d: Less cartoon (2026-11-19, user report: "style is very cartoonish")
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
- [ ] foliage_test Meadow: ground between blades still reads paler than the grass. Not the tint strength
      (0.5 → 0.8 changed nothing; reverted). Look at the cover colour itself (mean of blade colours incl. dry
      tips) and the texture-detail factor in the terrain ground-cover block.
- [ ] Blade mottling cost not isolated: grass GPU on the Meadow pose read 2.4-4.1 ms across runs with
      identical instance counts (other GPU load); needs an on/off pair.
- [ ] Vista: meadows and forest are now close in tone; consider a slightly lighter/yellower far meadow.
- [ ] Generated bushes still read as flat cards up close: textured (photographic) leaf atlases are the next
      step for plants seen at arm's length.

### Phase 7: World detail (2026-11-19, user request: paths, streams, small things, cliffs/overhangs)
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
      shapes have hit their limit. Next: scanned cliff/rock-face assets (downloads approved 2026-11-19),
      and ledges/terraces carved into the height archive (`build_landscape_demo.py`).
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
- Cost (single runs, DX12): Trail 27.1 ms, ForestFloor 26.0 ms, both CPU-bound as before (record ~16.5 ms,
  GPU 11.7-13 ms).

### Phase 2: Pay for it (performance)
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
- [ ] **CPU draw recording on the vista** (Phase 0: camera record 18 ms, shadow record 6 ms, scene prepare
      11 ms, run serially). Break camera recording down per stage first; candidates are moving more far
      forest onto GPU-driven instance draws (InstanceCull) and overlapping prepare with recording.
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
- [ ] **Far blocks follow the detail budget** (todo.md open item): far forest is outside it and is what
      makes the vista GPU-bound.
- [ ] Optional headroom only, not the route to the budget: **temporal upscaling**
      (`SceneSettings::temporalUpscaling` is serialized but unimplemented). Render at ~0.67-0.77 scale with
      the existing TAA jitter/history and upscale in the resolve; pixel-bound foliage cost drops ~1.7x.
- [ ] Check: vista ≤ 16.7 ms with Phase 1 on; record the stage split against the budget table.
      Latest (single run, DX12): vista 28.4 ms (CPU record 16.2, GPU 11.0); ForestEdge 27.8 (record 16.5,
      GPU 11.5). **The GPU is now within budget at ground level; the CPU (update + prepare + record ~27 ms,
      serial) is the blocker.** Every CPU stage moved ±30 % together between runs today (machine load), so
      compare CPU changes with interleaved A/B pairs only. The forest floor adds 133 candidate draws and
      ~110 active grass tiles on the vista.

### Phase 3: Shadows and sky visibility (dappled light)
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
- [ ] **Screen-space contact shadows**: blocked by the forward renderer: surfaces shade before the frame's
      depth exists (only the foliage pre-pass and last frame's AO copy), so a screen march would read stale,
      reprojected depth. Revisit with a depth pre-pass for all opaque geometry.

### Phase 4: Forest floor and undergrowth (RDR2 / GTA VI shots)
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
- [ ] Grass clumping (grass.md Phase 7 open item) and a dead/dry blade species mixed by the dry/lush noise.
- [ ] Option: CC0 scanned plants (e.g. Poly Haven) for hero undergrowth. Procedural generators will cap
      close-up fidelity; scans are what the references use.

### Phase 5: Trees
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

### Phase 6: Air and camera
- [ ] Froxel volumetric fog (sun shafts through canopy, RDR2/pine shots). Re-use the cascade shadow; ~160x90x64
      froxels, temporally reprojected; budget ≤ 1 ms. Replaces the per-pixel shaft march in the forward shaders.
- [ ] Depth of field (shots 2, 4, 5): half-res gather bokeh, off by default, for cinematic/photo mode.
- [x] **AgX** tone mapping (`ToneMappingOperator::AgX` = scene 3 → PostProcess.hlsl operator 6; polynomial
      fit of Troy Sobotka's AgX; editor Output Tone Mapping combo). Lifted shadows, softer highlights and less
      saturated sky/greens than ACES (`.local/foliage/agx_*.png` vs `aces_*.png`). ACES stays the default;
      a per-scene choice.

## 5. Updates

Record progress here (date, phase, what changed, capture used to check it).

- 2026-11-19: Gap analysis and plan written.
- 2026-11-19: Phase 0 done (foliage_test scene, baselines; the vista is CPU-bound; standalone prefab trees
  hit a 64 ms shadow-submission path). Phase 1 leaf lighting in (crown cook, `foliageTranslucency`);
  checked on foliage_test and the vista (DX12), and the Backlit pose on DX11 and Vulkan
  (`.local/foliage/p1_Backlit_{dx11,vulkan}.png`, same look). Leaf meshes in the tree are now cooked;
  re-cooking needs the originals from git.
- 2026-11-19: Phase 1b: fixed grass/bush LOD measured from height 0 (every grass plant was LOD2 near the
  viewer), rebuilt bush leaves, added landscape viewpoints ForestEdge / ForestFloor / Backlit.
- 2026-11-19: Phase 2 start: indexed woodland meshes, prefab/occlusion/draw-record CPU cuts (landscape
  ForestEdge 27.1 → 24.1 ms); found ground views geometry-bound by occluded forest → Hi-Z next.
- 2026-11-19: Phase 1c: landscape forests mix two species per size band per cell, wider scale, slight lean.
- 2026-11-19: Phase 4 forest floor (litter, herbs, ferns, seedlings on the forest-soil paint) and Phase 3
  canopy sky visibility + exposure floor: forest interiors now read dark with dappled sun, like the RDR2 shot.
- 2026-11-19: GPU occlusion pyramid from real depth (ForestEdge GPU 15.8 → 11.5 ms), 7 far cluster stands,
  AgX tone mapping. Checked on DX12, DX11, Vulkan. The CPU path is now the 60 fps blocker.
- 2026-11-19: tree wind, cloud shadows (sun cookie scroll), CPU: prefab profile refresh 1 s and transform
  membership stamps. Contact shadows recorded as blocked by the forward renderer.
- 2026-11-19: diagonal tree stripes = dissolve dither (IGN) → Bayer; leaf shadow bias from the card facet;
  tree-wind shading at rest position.
- 2026-11-19: Phase 1d colour pass (plant palette, blade mottling, terrain match, AgX on the landscape).
- 2026-11-19: TAA default on + per-frame dither rotation (dissolve pattern gone under TAA).
- 2026-11-19: Phase 7 start: trails, cliff bands + overhang/ledge shapes, scree, fallen branches and
  ground ivy. Streams, tree vines, extra terrain layers and scanned assets recorded as open.
- 2026-11-19: crags + steep-slope shading fix; open list (section 3b) added.
- 2026-11-19: scanned Poly Haven rock faces cooked and placed as cliffs.
- 2026-11-19: scanned stumps, mossy rocks, dry branches and ferns imported and placed; stripe
  investigation todo added.
