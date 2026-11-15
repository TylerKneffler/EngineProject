# Landscape demo

Open `Engine/Core/Assets/Scenes/terrain/landscape_demo.scene`.

A 5.2 by 5.2 km streamed map (41 x 41 cells of 128 m) with mountain ranges,
oak forests, open meadows and five lakes. Lake beds are sand with gravel
shores, carved 3-16 m below the water level (`LandscapeDriver.waterLevel`,
-40 m).

## What it uses

- **Terrain layers** (the CC0 PBR sets): sand (lake beds), gravel (shores),
  meadow grass, forest soil, rock, snow. Hex-tiling, height-lerp transitions,
  cliff side projections and near-camera relief all apply.
- **Paint rules** (`LandscapeDriver`, per vertex at load):
  - Rock comes from a slope class stored in the height archive, computed over
    the whole map so it matches across cell borders. Rock also covers
    everything above 58 m.
  - Snow lies above about 102 m.
  - Forest soil follows a world-space forest mask between the shore and 46 m.
- **Forests:** the "Landscape forest" object layer is generated per cell by
  the driver. Up to 220 oak placements per cell, dense inside the forest
  mask with a few lone trees in the meadows. Each cell grows one stand of
  three species (one large, one medium, one small, picked per cell from the
  eight oaks), which keeps the instanced draws per cell low while the map as
  a whole mixes all eight. LODs: full trees to 60 m, LOD1 to 220 m, then the
  textured impostors out to 900 m. The forest sits in the project's `Trees`
  layer, so the detail budget scales those distances
  (see `project-layers-detail-budget.md`). Large trees sit in forest interiors and
  small ones at the edges; nothing grows on slopes over 30 degrees.
- **Grass:** the manager's "Procedural meadow grass" layer, limited to the
  meadow paint layer with the new `GrassField::proceduralLayerMask`
  (26 m radius).
- **View range:** eleven by eleven loaded cells (about 700 m each way) at
  full detail. Beyond them, far terrain tiles and far tree blocks
  (`TerrainManager::farObjectRadius`, impostors only, batched into 1 km
  blocks) reach the map edge.
- **Texture LOD** is off for this scene. The distance mip bias copied from
  the meadow scenes blurred every tree beyond about 30 m.
- **Sun shadow bias:** the normal bias is 0.06; the meadow scenes use 0.002.
  The low sun grazes the meadows, and the smaller bias banded them with
  shadow acne from about 20 m out. The engine now also scales the shadow
  normal offset with each cascade's extent (`Object.hlsl`,
  `EvaluateDirectionalShadow`), because far cascades have larger texels.
- **Far terrain:** beyond the loaded cells, far tiles are
  colored from a per-cell-corner color field. The field is the near paint
  averaged over 128 m, with canopy color over forests. Distant forests, rock,
  snow and lake beds therefore match what the near cells show.

## Water

`LandscapeDriver` places one flat plane at `waterLevel` over the whole map
(runtime object "Lake water", project layer `Water`). Terrain above the level
hides it, so only the lake basins show water. To turn it off, clear
`enableWater`.

- **Material:** `Assets/Materials/Water/LakeWater.material`. It is
  alpha-blended, with a dark teal base and roughness 0.06, and casts no
  shadows.
- **Shader:** `Assets/Shaders/Water.hlsl`, a custom material shader built on
  the engine surface path, so lighting, the sun glint and shadows still
  apply.
  - **Waves:** procedural and animated, with no textures. A faint long swell
    plus three rotated octaves of gradient noise make the wind ripples.
    Large, drifting gust patches scale the ripples, so no stretch of water
    repeats the next.
  - **Distance:** each wave fades once it is smaller than a few pixels, so
    distant water does not shimmer. Slopes are steepened with distance so
    that the far water still varies.
  - **Reflection and opacity:** Schlick Fresnel blends in a procedural sky
    gradient. Steep views show the lake bed through the alpha; grazing and
    distant views turn opaque.
  - **Not reflected:** terrain and trees, since there are no screen-space
    reflections.
- **Time:** the material sets `shaderUsesTime`, a new Material field, so that
  its draws receive scene time (`ObjectData.grassAnimation.x`). Other
  materials keep their per-frame object data unchanged.
- **Flight:** the explorer cannot fly below `flightMinimumHeight`, which is
  1.2 m above the water. Before, flying over a lake dropped to the lake bed.
- **Shader compile:** on DX11, the first time water comes into view costs a
  one-off runtime shader compile (about 0.5 s).

## Regenerating

```powershell
python tools/build_landscape_demo.py --preview .local/landscape_preview.png
```

The script writes `Assets/TerrainData/Landscape/world.heights` (4 MB),
`world.fargrid` and the scene, copying the explorer and sun from
`oak_meadow_expanded.scene`. Shapes are set at the top of the script: lake
centres and radii, the start position, and the mountain mask. Re-running
replaces the generated files. Generated cell meshes cache under
`.local/cache/landscape_demo`.

## Engine changes made for it

- `TerrainManager::SetObjectLayerGenerator` fills named object layers that
  have no saved placements (it runs on streaming workers).
  `SetObjectLayerSetup` lets a scene script configure layer objects on
  restore.
- `TerrainHeightArchive::ReadCellCodes` reads the raw two-bit code per
  sample.
- `FarTerrainPalette::gridColors` is an optional far-tile color field. When
  it is present, far-tile seam blending defers to it.
- `GrassField::proceduralLayerMask` restricts procedural grass to chosen
  paint layers.
- Streaming: prefetched cells waiting outside the current window no longer
  hold `maxPreparedCells` slots, and they rank after every cell of the
  window. Before, a still viewer at a large `loadedRadius` never loaded the
  window corners.
- Rendering: instanced LOD bands whose distance range misses a block's
  bounds are skipped at draw preparation (exact, since bands select per
  pixel by distance). Distant forest blocks now record only their impostor
  band.
- Oak impostor materials use metallic-roughness shading with the pack's
  billboard normal maps; they were BlinnPhong with white specular and
  rendered as pale, flat silhouettes.
- Fixed: the manager's meadow `GrassField` settings passed bools that
  `JsonValue` turned into numbers, so procedural grass never generated.

## Controls

The explorer flies: WASD moves along the view heading, Space rises, Shift
descends, Ctrl boosts (x4). It eases in and out and never dips below 1.5 m
above the ground.

## Measured view ranges (1080p, RTX 4090, DX12, uncapped)

Raising `loadedRadius` scales cost with the cell count, because every loaded
cell is fully interactive (colliders, grass scripts, every tree's LOD
objects):

| loadedRadius | Cells | Median   | FPS | Notes |
|--------------|-------|----------|-----|-------|
| 5            | 121   | 7.0 ms   | 143 |       |
| 7            | 225   | 9.0 ms   | 111 | detail budget already at its minimum |
| 10           | 441   | 18.3 ms  | 55  |       |
| 14           | 841   | 35.6 ms  | 28  |       |
| 20           | 859   | 53.2 ms  | 19  | 859 of 1681 cells; the memory budget stops loading |

Far objects give the long view instead:

| Setup                          | Median  | FPS | Instances |
|--------------------------------|---------|-----|-----------|
| loadedRadius 5, far objects 10 | 6.5 ms  | 154 | 15k       |
| loadedRadius 5, far objects 20 | 7.1 ms  | 142 | 50k       |

Flying at boost speed (100 m/s) holds about 9 ms median but hitches (p95
21–28 ms). Each newly loaded cell costs about 3 ms of main-thread scene
installation; at most one cell installs per update beyond the 2 ms budget
(`maxCommitMilliseconds`). The rest of each hitch comes from the update and
post-physics steps for the new cell objects. These are the next targets.

## Known limits

- Trees beyond the loaded cells are impostor-only far blocks. Grass reaches
  only the near cells.
- Mountainsides facing away from the sun read very dark: fast terrain
  lighting has only the flat scene ambient, because HDRI sky lighting is
  effectively off engine-wide (see below).
- Mountains are moderate (to about 120 m above the lowest lake bed), because
  archive heights are stored in 1/256 m within +-128 m.
- The start sits on a meadow pad; walking speed is 6 m/s (sprint x4).

## Open issue: HDRI sky lighting

`Texture::Prepare` frees CPU pixels after upload unless
`RetainPixelsForCpu()` was called, and nothing calls it for the skybox. So
`EnvironmentMap::Acquire` gets no pixels, and sky irradiance (SH) and
reflections are zero in every scene. Retaining the skybox pixels fixes it.
However, the current content was tuned without sky light: with it, the oak
foliage turns icy white. Fixing this needs a material retuning pass, so it
is left as it is for now.
