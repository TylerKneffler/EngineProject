# Grass: realism, coverage, distance

Notes from a review of the grass system (2026-11-19). Three problems to fix:

1. **Pop-in.** Grass appears in visible squares at the edge of its range as the camera moves.
2. **Weak coverage.** The flat terrain shows clearly between grass clumps.
3. **Flat distance.** Past the grass radius the terrain looks like a painted floor, and the
   grass edge reads as a change in colour, pattern and lighting.

Line numbers below were correct at commit `0206428` and will drift.

---

## 1. How grass works today

| Piece | Where | Notes |
|---|---|---|
| Streaming / tiles | [GrassField.cpp](../Engine/Core/Assets/Grass/GrassField.cpp) | 4 m tiles around the viewer. Each tile is one instanced Mesh with all species and all 3 LODs. CPU prepares placements only; blade transforms run on the GPU. |
| Procedural placement | `GrassField::BeginBuild` (≈L582-705) | Short + tall tussocks, accent drifts (flowers), shrub thickets (value noise). Masked by terrain paint layer. |
| Meadow preset | [MeadowGrass.cpp](../Engine/Core/Assets/Scripts/Grass/MeadowGrass.cpp) | Defaults for new fields; procedural layers use `meadow_tussock` / `field_tussock`. |
| Landscape demo tuning | [LandscapeDriver.cpp:799-834](../Engine/Core/Assets/Scripts/Landscape/LandscapeDriver.cpp#L799-L834) | `grassRadius = 26` → layer `viewDistance = 36`, LOD1 7 m, LOD2 13 m, 2.6 short + 0.4 tall tussocks/m², accents 0.18/m², shrubs 0.7/m². |
| Meshes | [generate_meadow_grass.py](../tools/generate_meadow_grass.py), [generate_meadow_accents.py](../tools/generate_meadow_accents.py) | Ribbon blades ~1.7 cm wide. Tussock = crown of blades + low sward (84 blades, r 0.6 m, h 0.22 m). LOD2 keeps 35% of sward, widened 1.9x. |
| Wind | [GrassWind.hlsli](../Engine/Core/Shaders/GrassWind.hlsli) | Root-pinned rotation bend, advected Perlin gust fronts + per-blade flutter. Good; keep. |
| Interaction | [GrassFootBend.hlsli](../Engine/Core/Shaders/GrassFootBend.hlsli) | Capsule push-aside. Keep. |
| Surface shading | [Object.hlsl:266-290](../Engine/Core/Shaders/Object/Object.hlsl#L266-L290) `GrassColorVariation` | Dry/lush Perlin patches (period 11 m) + per-clump brightness/hue jitter, keyed to placement root. |
| Fades | [Object.hlsl](../Engine/Core/Shaders/Object/Object.hlsl) `ApplyGeometryFade` | Dithered clip for LOD crossfades and tile visibility. |
| Terrain under grass | [grass.material](../Engine/Core/Assets/Materials/Terrain/PBR/grass.material) | Photo texture `Grass004`, base colour [0.2, 0.26, 0.11]. No link to grass colours. |
| Range scaling | `GrassField::DetailRadiusScale` + [ProjectLayers.h](../Engine/Core/Model/ProjectLayers.h) | Adaptive detail scales grass reach 0.5x–2x at runtime; "Grass" layer budget weight 1.25 (shed first). |

## 2. Diagnosis

### Pop-in
- **The fade is per tile, not per plant.** [GrassField.cpp:917-927](../Engine/Core/Assets/Grass/GrassField.cpp#L917-L927)
  takes the distance to the tile's nearest edge and dithers the whole 4 m tile over `distanceFadeWidth = 2 m`.
  A 2 m band at the edge of a ~36 m radius is too narrow, so tiles show as squares.
- **Tiles are built late.** One tile is built per refresh, within a shared 2 ms budget
  ([GrassField.cpp:1024-1026](../Engine/Core/Assets/Grass/GrassField.cpp#L1024-L1026)). Tiles only exist once
  they are inside the radius, so a fast-moving camera gets late tiles, which then also wait out
  `fadeDuration = 0.35 s`.
- **The radius moves.** Adaptive detail can shrink grass reach to 0.5x (≈18 m) on a low-spec PC. The scale is
  smoothed and quantised (12% steps), but the edge still visibly slides when the budget changes.

### Coverage
- Clumps at 2.6/m² with thin blades leave ground visible between them, especially from above.
- Thinning starts very close (LOD1 7 m, LOD2 13 m), and LOD2 widening (1.9x on 35% of the sward) does not
  keep coverage constant.
- Gaps show the `Grass004` photo texture, whose colour and pattern differ from the vertex-coloured grass,
  so they read as "ground showing through" rather than shadowed undergrowth.
- Grass shadows are off (`castShadows = false`), so nothing darkens the base of the field.

### Flat at a distance
- Past the radius there is only the tiled terrain texture. The grass's colour pattern (dry/lush, clump jitter)
  stops where the blades stop, and so does the soft lighting of a blade field.

## 3. Research references

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

## 4. Implementation plan

Ordered by payoff per effort. Phases 1 and 2 are the recommended first pass: together they address all three
problems without raising grass cost. Keep wind, foot bend and placement determinism unchanged throughout.

### Phase 1: Per-plant distance fade (pop-in) — implemented, visual check pending
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
- [x] Check: grass_test.scene capture shows no tile squares or ring at the edge. Edge band still
      noticeable; tune `distanceFadeFraction`.

### Phase 2: Terrain matches the grass (coverage + distance) — implemented, visual check pending
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
      `groundTintStrength` (0.75) and `groundRootShade` (0.55), category "Grass | Ground".
- [ ] Far tiles beyond the loaded cells use the macro colour palette (`LandscapeDriver` `farColors`,
      `mean[Grass] × .85`), not this tint. Match that palette's grass entry to the cover colour.
- [ ] Check: top-down and grazing captures near the grass edge; the edge should be hard to locate.
      Tune `groundTintStrength` / `groundRootShade` from captures.

### Phase 3: Density falloff with width compensation (coverage at mid range)
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

### Phase 4: Lighting as one surface
- [ ] Blend blade normals toward the terrain normal, more with distance (≈0.3 near → 0.8 far).
- [ ] Darken toward the base of each blade (height-based AO in the grass pixel shader; the vertex shader
      already knows height for wind).
- [ ] Optional: cheap translucency (back-lit blades) for sun-facing views.

### Phase 5: Build tiles ahead
- [ ] Create and build tiles in a ring beyond the radius (`retentionRadius` exists) so they are resident
      before they reach the fade band.
- [ ] Prioritise tiles in the direction of travel (dot of tile offset with viewer velocity) over pure distance.
- [ ] Consider damping how fast `DetailRadiusScale` may move the grass radius, or excluding the fade band
      from it, so the edge does not slide visibly.

### Phase 6: Grass texture past the radius (flat distance)
- [x] **6a:** terrain shader (ground-cover block, weighted by `beyond`): two octaves of fine
      brightness grain (1.7 and 6.3 /m, each faded as it nears the pixel footprint so it cannot
      flicker) and a grazing-angle sheen, so open ground past the blades reads as short grass.
- [ ] (original 6a note) terrain shader adds grazing-angle sheen and small-scale brightness/normal noise in
      grass-painted areas. Expected to remove most of the "painted floor" look.
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

### Phase 6½: Antialiasing (grass edges stair-step and shimmer)
The live renderer has **no antialiasing**: MSAA exists only in the DX11 video-export renderer
(`DX11GameRenderer`, `--msaa`), and `SceneSettings::temporalUpscaling` is serialized but unimplemented.
Grass blades are 1–2 px wide at a few metres, so their edges alias and shimmer under wind.
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
  - **Vulkan:** not yet (renders without MSAA).
  - Checked on grass_test.scene: DX11 and DX12 4x give clean blade edges; AO runs under MSAA on both;
    no DX12 device errors. With auto exposure on, DX12 MSAA captures at 120 warm-up frames looked
    darker: a start-up adaptation transient (converges to the same level by 600 frames; identical
    with `ENGINE_AUTO_EXPOSURE=0`).
  - **Not yet exercised:** live MSAA switching in the editor's DX12/DX11 views (captures use the game
    renderer).
  - Diagnostics for captures: `ENGINE_MSAA`, `ENGINE_FXAA`, `ENGINE_GRASS_MIN_WIDTH`,
    `ENGINE_AUTO_EXPOSURE` override the project settings in the game/capture tool.
- [x] **TAA** (`AntiAliasingSettings::taa`, `taaFeedback` 0.75–0.97, `taaSharpen` 0–1; Preferences >
      Effects; off by default; mutually exclusive with MSAA in the UI, MSAA wins if both are set).
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
  - Vulkan: not supported yet (no jitter is applied there).
  - Override for captures: `ENGINE_TAA=0|1`.

### Coverage test (render mode)
- [x] `SceneRenderMode::Coverage` (6): terrain red, everything drawn on it green (reuses the Performance
      heat colours via `morphParams.z`). Editor: Scene view render-mode list > Coverage. Captures:
      `GameBackendCapture --render-mode 6`. Measure with a red/green pixel count (terrain share of the
      ground pixels, split into near/mid/far screen bands).

### Phase 8: Larger, thicker foliage
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
- [ ] Bushes share the grass radius (~36 m); larger bushes should reach further (their own range or a
      props/foliage layer).
- Coverage now (grass_test.scene, DX12 960×540; terrain share of ground pixels, overall | near /
  mid / far): raised pose **26.5 %** | 23 / 15 / 44 %, eye level **14.8 %** | 22 / 8 / 13 %
  (started at ~42–57 %). Grass GPU 3.6–3.8 ms with 17k plants (detailed grass, bushes, far ring).

### Phase 7: Mesh/coverage tuning (optional)
- [x] 2026-11-19: blade meshes reworked (`tools/generate_meadow_grass.py`): leaf outline (narrow sheath,
      widest ~20% up, parallel margins, long tapered tip) instead of a root-to-tip triangle; V-folded
      cross-section with a sunken, paler midrib (3 columns at LOD0/1); 5/4/3 segments (was 4/3/2);
      ±12% brightness per blade; seed stalks are 3-sided tubes at LOD0/1. Triangles: meadow_tussock
      798/340/88 → 1824/709/126, field_tussock 580/290/101 → 1390/671/149.
- [ ] Clumping (Voronoi cells with shared height/colour/facing) to make the field read as real grass while
      gaps look natural.

## 5. Updates

Record progress here (date, phase, what changed, capture used to check it).

- 2026-11-19: Review and plan written.
- 2026-11-19: Phases 1 and 2 implemented (see checkboxes). `devoptimized` build passes; Object.hlsl
  (VSMain, PSMain, PSShadowMain), GrassFootBend.hlsl and ObjectOutline.hlsl compile with DXC for DX and
  Vulkan; IndexedInstanceGeometry, InstanceTilePreparation, TerrainEditingTools, DrawBatchPreparation,
  DetailBudget and D3DShaderCache tests pass.
  - **Testing pitfall:** `GameBackendCapture` is `EXCLUDE_FROM_ALL`, so `cmake --build --preset
    devoptimized` does not rebuild it. Build it with `--target GameBackendCapture` after engine changes.
    The first captures of this work ran a 2026-11-18 binary (1792-byte object records) against the new
    shaders (1840), which garbled every object (sky only, giant stretched triangles) and froze the PC.
    After rebuilding it, lighting_showcase renders correctly on DX11 with these changes.
  - The DX12 access violation on landscape_demo (also seen on the stashed baseline) was with that stale
    binary too; re-test before treating it as a separate bug. The machine was on the Intel UHD iGPU,
    where DX11 needs ~5 min to start landscape_demo.
  - The `debug` preset fails on `bc7enc` (`/O2` with `/RTC1`); use `devoptimized`.
  - Landscape captures with the rebuilt tool (DX11, Intel UHD): start pose renders correctly. First
    tint pass made the meadow a flat saturated lime; tuned to cover colour × 0.8 with 25%
    desaturation, `groundTintStrength` 0.5, more texture detail kept (80% near / 50% far). Result:
    olive meadow close to the original, with the dry/lush patches visible.
  - Grass fade still not seen: `--camera-position 0,-38,40` landed under the terrain (capture overrides
    skip the flight controller's ground clearance), and a pose over the lake near the shore
    (220,-38,680 → 190,-39,600) froze the PC during warm-up, with no crash dump. Cause unknown; the same
    build completed the start pose and drew 5474 grass instances from under the terrain.
    Only landscape_demo contains grass; a small test scene (one terrain + procedural GrassField) would
    allow checking the fade without loading the landscape.
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
- 2026-11-19 (foliage work, see [foliage.md](foliage.md)): **LOD bug: grass tiles measured LOD distance
  from height 0.** Each tile object sat at `{x * span, 0, z * span}` with its placements' absolute ground
  height inside the instance transforms, so the tile origin that `InstanceCull.hlsl` (shared-LOD group
  test, `lodData.sphere` = tile record origin) and `ApplyGeometryFade` (object origin for fade modes 2-3)
  measure from was at y = 0. On the test cell (ground ~-30 m) every plant was >= 28 m away, i.e. LOD2
  (coarse bushes = "lots of squares", thinned grass) even at the viewer's feet; on the landscape the
  error is the terrain height (hundreds of metres). Raising the layer's LOD distances to 30/35 m changed
  nothing, which is how it was found. Fix: `TileState::originHeight` (ground at the tile centre) is the
  tile object's y, and both build paths (procedural `AdvanceBuild`, authored `BeginBuild`) subtract it
  from placements and bounds. World positions are unchanged. Grass now draws the detail it was tuned
  for: grass GPU on foliage_test 2.0 → 3.3 ms (ForestFloor), 1.3 → 2.3 ms (Meadow); the detail budget
  balances it.
- 2026-11-19: shrub/bush leaves (`generate_meadow_grass.py`): `leaf_card` is now an ovate leaf per detail
  level (10 / 6 / 2 triangles; raised midrib, back-curl) instead of a 4-vertex diamond; `grow_bush`
  leaves get a random roll (they lay in parallel tiers), ±25 % value jitter and occasional yellowing;
  coarse LODs keep more, smaller leaves (`LEAF_KEEP` 1/.6/.35, `LEAF_GROW` 1/1.25/1.6, was 1/.46/.2 and
  1/1.45/2.15: 26 cm diamonds at LOD2); no crown core at LOD0 (it read as an egg), smaller at LOD1/2.
  meadow_bush 8780/4292/1898 → 32844/11954/2718 tris, meadow_bush_tall 7544/3730/1794 → 28152/10562/2438.
