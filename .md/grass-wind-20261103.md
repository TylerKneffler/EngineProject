# Grass wind shader

## Research and selection

Compared these techniques before implementing:

| Technique | Benefit | Tradeoff in this engine |
| --- | --- | --- |
| Root-weighted vertex waves | Fits resident, instanced mesh geometry; no per-frame placement updates | Needs height weighting and spatial phase variation to avoid rigid, synchronized motion |
| Main bending plus detail flutter | Separates broad gusts from small local movement | Extra vertex math; full tree branch metadata is unnecessary for grass |
| Procedural blade curves and mesh shaders | Direct control over blade shape and tessellation | Replaces the current authored species meshes and requires a larger renderer change |

Primary references:

- [GPU Gems: Rendering Countless Blades of Waving Grass](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-7-rendering-countless-blades-waving-grass): compares cluster, vertex and grass-object animation; discusses fixed lower vertices and per-object phase variation.
- [GPU Gems 3: Vegetation Procedural Animation and Shading in Crysis](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis): main bending and detail animation.
- [AMD GPUOpen: Procedural grass rendering](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/): procedural blade geometry, distance-dependent detail and spatially varying wind waves.

Initially selected analytic vertex bending with traveling gust waves and
smaller crosswind flutter. Updated the gust field to seeded, two-octave 2D
gradient Perlin noise at the user's request. This implementation uses no noise
texture or simulation buffer. It is an approximation of wind, not a physical
blade simulation.

## Implementation

All wind parameters now live together at the top of
`Engine/Core/Shaders/GrassWind.hlsli`:

| Shared constant | Default |
| --- | --- |
| GrassWindStrength | 1.0 (shader clamps to 0?1) |
| GrassWindMaximumBendDegrees | 85 degrees |
| GrassWindRootStiffnessHeight | 0.18 metres |
| GrassWindSpeed | 0.45 |
| GrassWindDirectionDegrees | 35 (+X is 0 degrees, +Z is 90 degrees) |
| GrassWindWaveSize | 32 metres |
| GrassWindSeed | 18731 |

GrassField no longer exposes, serializes, hashes, or propagates wind settings.
Old saved per-layer wind attributes are ignored when loading. Meshes carry no
wind parameter overrides. The renderer supplies scene time and grass
classification only. Non-grass records keep both zero, preserving unchanged
object-upload caching. Every grass layer therefore samples the same wind field.

Integer-hashed lattice gradients and quintic interpolation implement Perlin
noise. Two octaves, weighted 0.90/0.10, emphasize broad swells over local detail.
The coordinate field travels downwind at 2 metres per second times wind speed
and is stretched across the wind to form broad fronts. The detail octave also
drives crosswind flutter. Larger wave sizes produce broader swells.

The shared object vertex shader calls `GrassWind.hlsli`. A persistent
placement's world-space root anchors the field independently of patch yaw,
LOD, tile and camera direction. Height-dependent rotation `maximumAngle*h/(h+0.18)` pins
roots with zero bend slope. Tips move sideways and lower toward the ground
without lengthening the root-to-tip segment. Normals and tangents follow the
deformation derivative. Shadow
geometry uses the same shared vertex deformation if casting is enabled.

Signed Perlin gusts swing from negative to positive strength, passing through
upright between opposite leans. Peak strength still reaches the bend limit.
At maximum gust, a 1-metre tip leans about 72 degrees and a 2-metre tip about
78 degrees. Tile bounds and individual GPU-culling bounds reserve 1.5 times
source height for the lateral shift and tip lowering. The CPU/HLSL object record contains a float4 for scene time and grass
classification; its size assertion and outline buffer layout match. Animation
does not update persistent placement transforms or cached tile metadata.

`grass_flat_test.scene` retains grass casting off and light shadows on.

## Validation

- Debug Editor and GrassPerformanceTests builds passed.
- Full GrassPerformance regression suite passed.
- DX11 and DX12 fade/instancing/shadow render checks passed (process exit 0).
- Shared seeded wind animates in the 128-pixel fixture after a 4.7-second time
  advance and matches GPU/fallback instancing at the same time.
- A regression checks that all five former wind fields are absent from
  GrassField's serialized inspector data.
- Paused-time captures were identical; clearing grass classification restored
  the static non-grass capture exactly. Unchanged placement new/edited upload bytes stayed
  zero during animation.
- Existing five-distance GPU/fallback LOD comparisons and shadow regressions
  passed. These checks do not replace an artistic review of the full meadow
  in motion or establish a measured performance delta.

Shared-settings validation logs: `.local/grass-shared-wind-test-build.log`,
`.local/grass-shared-wind-editor-build.log`, `.local/grass-shared-wind-dx11.log`,
`.local/grass-shared-wind-dx12.log`, `.local/grass-shared-wind-regression.log`.

The running editor locked Editor.exe and its PDB. Built EditorSharedWind.exe
with separate linker outputs instead of closing or replacing the live process.

## Strong wind update

Raised shared strength to 1.0 and changed horizontal displacement into
height-dependent bending with tip lowering. The shared 85-degree maximum is
clamped in the shader; CPU tile preparation and GPU placement culling reserve
a conservative 1.5-height motion envelope. Parameters remain shader-only.

Validation logs: `.local/grass-high-wind-build.log`,
`.local/grass-high-wind-editor-build.log`, `.local/grass-high-wind-dx11.log`,
`.local/grass-high-wind-dx12.log`, `.local/grass-high-wind-regression.log`.

## Long, slow waves

Increased shared wave size to 32 metres and reduced speed to 0.45 (0.9 m/s
advection, roughly 36 seconds per primary noise cell). Broadened the fronts
across the wind, reduced the detail octave's frequency and drift, and lowered
crosswind flutter from 0.12 to 0.025. Strong bending and the existing culling
envelope remain unchanged. Settings remain shared shader constants.

Checks: `.local/grass-slow-waves-dx11.log`,
`.local/grass-slow-waves-dx12.log`, `.local/grass-slow-waves-shader-build.log`.

## Bidirectional flowing waves

Removed the positive gust bias that held every blade downwind. Gusts now use
`clamp(2.4 * (0.90*broad + 0.10*detail), -1, 1)` so the advected Perlin field
can bend blades in either direction and move them back through upright.
Gentle crosswind variation changes the sway heading without introducing rapid
flutter. The 32-metre wave size, 0.45 speed and strong peak bend are retained.

Checks: `.local/grass-flowing-waves-dx11.log`,
`.local/grass-flowing-waves-dx12.log`, `.local/grass-flowing-waves-shader-build.log`.
