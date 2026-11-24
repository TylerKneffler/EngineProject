# Grass notes

Moved out of todo.md on 2026-11-24: how grass works, diagnosis, references and testing notes. Open work is in todo.md.

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

### Grass Phase 2: Terrain matches the grass (coverage + distance)

### Grass Phase 3: Density falloff with width compensation (coverage at mid range)

### Grass Phase 4: Lighting as one surface

### Grass Phase 5: Build tiles ahead

### Grass Phase 6: Grass texture past the radius (flat distance)

### Grass Phase 6½: Antialiasing (grass edges stair-step and shimmer)
At the review the live renderer had no antialiasing (MSAA only in the DX11 video-export renderer), and grass
blades are 1–2 px wide at a few metres, so their edges aliased and shimmered under wind. Now: FXAA, MSAA
(DX11, DX12) and TAA (DX11, DX12; on by default). Temporal upscaling from a render scale exists on DX12 (Foliage
Phase 2).

### Grass Phase 8: Larger, thicker foliage
- Coverage results on grass_test.scene (DX12 960×540, terrain share of ground pixels, near / mid / far
  screen bands; far is mostly open terrain past the grass radius):

  | Pose | Before (grass only) | + bushes | + thicker cover | Grass GPU |
  |---|---|---|---|---|
  | Raised (4 m up) | 42 / 71 / – % | 47 / 34 / 90 % | **23 / 16 / 87 %** | 2.4 → 3.4 ms |
  | Eye level | 36 / 34 / – % | 53 / 25 / 47 % | **21 / 25 / –** % (22 % overall) | 2.6 → 3.7 ms |

  Near and mid gaps roughly halved. Remaining terrain is (1) past the ~36 m grass radius (Phase 6
  impostors / longer range) and (2) small gaps between tussocks up close.
- Coverage now (grass_test.scene, DX12 960×540; terrain share of ground pixels, overall | near /
  mid / far): raised pose **26.5 %** | 23 / 15 / 44 %, eye level **14.8 %** | 22 / 8 / 13 %
  (started at ~42–57 %). Grass GPU 3.6–3.8 ms with 17k plants (detailed grass, bushes, far ring).

### Grass Phase 7: Mesh/coverage tuning (optional)

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
