# Surface detail: height-mapped materials plan

Goal: realistic, depth-reading surfaces (ground, walls, rock, terrain) on flat or
low-poly meshes, within the large-scene budget in
[large-scene-rendering.md](large-scene-rendering.md) (120 FPS at 1080p).

## 1. Current state (audited 2026-11-13)

### Materials (`Engine/Core/Compoonents/Materials/Material.h`)
- One metallic-roughness PBR material: base color, metallic-roughness, normal,
  occlusion, emissive, **height**, SSS thickness, per-map UV set selection.
- Parallax params exist: `heightScale` (0–0.2), `heightMinSteps`, `heightMaxSteps`.
  Packed into `ObjectData.parallaxParams` in
  `SceneRendererDrawPreparation.cpp:267`.
- Terrain layer fields: `terrainHeightBlendStrength`, `terrainMacroColor`.

### Shader (`Engine/Core/Shaders/Object/Object.hlsl`)
- `ParallaxOcclusionUv` (line ~704): linear march + one secant step, step count
  lerped by view angle, gated by `materialDetail` (texture-LOD distance fade).
  Offset is applied to every material UV. Only runs when the mesh has tangents.
- Terrain (flag `2048`): up to 6 layers, weights in vertex UV channels
  (`uvChannels2/3`), each layer sampled from **separate `Texture2D` bindings**
  (base color reuses slots t0–t5, normals t27–32, roughness t33–38, height t39–44)
  through `if (layer == N)` chains. Height-weighted blending, a 2-sample rotated
  "stochastic" anti-tiling blend, and a macro-color fade at 512–2048 m.
- Fast runtime terrain lighting (flag `8192`) is Lambert + one shadow tap.
- Parallax materials are excluded from the depth-prepass / depth-equal path.

### Texture pipeline (`Texture.cpp`, `IGraphicsTexture.h`)
- Formats: **only `Rgba8` and `Rgba32Float`**. No BC1/4/5/7. KTX2 Basis files are
  transcoded to RGBA32 on load, so their compression is discarded.
- Mips: CPU box filter, sRGB-aware. No normal-map-aware mips, no roughness
  adjustment for normal variance.
- Mip streaming with footprint feedback exists (`RequestMipFootprint`).

### Samplers
- DX11: anisotropic ×8.
- **DX12 static sampler and Vulkan sampler: trilinear only, no anisotropy.**
  This is the single largest quality loss on ground planes at grazing angles.

### Content
- Terrain textures: 512² procedurally generated **albedo only**
  (`Textures/Terrain/generate_terrain_textures.py`). No normal/height/roughness.
- One full PBR+height demo set (`Textures/PBRDemo/CopperSlate_*`, 1254², RGB PNG).

### Defects found during review
1. **POM samples the wrong mip.** `heightMap.SampleLevel(..., textureBias)` passes
   a *bias* as an absolute mip index. Near the camera that is mip 0 for every
   step, regardless of screen footprint: aliasing/shimmer at distance and up to
   40+ full-resolution fetches per pixel (cache thrash).
2. **Height convention unverified.** The march treats the sampled value as
   *depth* (stops when `layerDepth >= sample`), i.e. white = deep. Most authored
   height maps (incl. ambientCG/Poly Haven "Displacement") are white = high,
   which inverts relief. Confirm visually on CopperSlate and make it explicit.
3. Samplers lack anisotropy on DX12/Vulkan (above).
4. 24 separate terrain texture bindings + branchy layer selects limit layer count
   to 6 and prevent dynamic indexing.

## 2. Technique survey (what to adopt, in cost order)

| Technique | What it gives | Cost | Verdict |
|---|---|---|---|
| Anisotropic filtering 8–16× | Sharp ground at grazing angles | Very low | **Do now** |
| BC7 albedo / BC5 normal / BC4 height | 4×–8× less memory and bandwidth than RGBA8 | Lower than today | **Do** |
| Normal-variance → roughness mips (Toksvig / LEAN-style) | Kills specular sparkle on bumpy surfaces at distance | Offline only | **Do** |
| Detail normal blending (RNM/UDN) | Micro detail on large surfaces | 1 fetch | Do |
| Parallax occlusion mapping, gradient-correct | Depth, self-occlusion | 8–32 fetches near, 0 far | **Fix + upgrade** |
| POM contact refinement / binary search | Fewer steps for same quality | Small | Do |
| POM soft self-shadowing (main light) | Crevices read as real depth | +8–16 fetches near | Do, distance-gated |
| POM pixel depth offset | Correct intersections with props/shadows | Breaks early-Z for that draw | Opt-in only |
| Silhouette POM / prism POM | Correct edges | High, special geometry | Skip for now |
| Hex-tiling (Mikkelsen 2022) | Removes visible repetition | 3 fetches per map | Replace current stochastic blend |
| Biplanar / triplanar on steep slopes | No stretching on cliffs | 2–3× fetches where slope > threshold | Do, slope-gated |
| Texture arrays + per-pixel top-N layers | Constant cost, many layers | Lower than today | **Do for terrain** |
| Runtime virtual texture / clipmap cache for terrain | Blend once, sample ~3 maps per pixel | Update cost amortised | Phase 4 |
| Tessellation / compute displacement near camera | Real silhouettes, shadows | Vertex heavy; needs RVT height | Phase 5 |

Notes from research:
- POM (Tatarchuk) is per-pixel ray marching in tangent space; it needs explicit
  gradients inside dynamic loops (`SampleGrad`), and depth bias handling to
  avoid aliasing.
- Mikkelsen's hex-tiling samples 3 randomly offset hex tiles and blends with a
  cheap contrast ramp; normals can also be randomly rotated per tile.
- Far Cry 4's adaptive virtual texturing and Unigine's landscape bake blended
  albedo/normal/height into a virtual texture so the per-pixel layer cost is
  paid only on page updates.
- Common packing: BC5 normal (XY, Z reconstructed), BC7 albedo (optionally with
  height or roughness in A), BC4 height when it is marched repeatedly so each POM
  step fetches only one channel.

## 3. Implementation plan

Each phase is independently shippable. All shader changes land in `Object.hlsl`
(shared by DX11/DX12/Vulkan); C++ changes must be mirrored across the three
backends.

### Phase 0 — Quick wins and fixes (1–2 days) — implemented 2026-11-13

Status: items 1–4 done. CopperSlate's height map was confirmed white = raised
(copper inlays white, cracks black), so the old POM rendered it inverted; the
new default fixes that without a material change. Visual check in-editor still
pending.
1. **Anisotropic sampler.**
   - `DX12GraphicsProvider.cpp:127`: `D3D12_FILTER_ANISOTROPIC`, `MaxAnisotropy = 16`.
   - `VulkanGraphicsTexture.cpp:156`: `anisotropyEnable = VK_TRUE`,
     `maxAnisotropy = min(16, limits.maxSamplerAnisotropy)`; enable
     `samplerAnisotropy` in device features.
   - DX11: raise 8 → 16 to match. Expose as a quality setting.
2. **Gradient-correct POM.** Compute `ddx/ddy(heightUv)` once before the loop and
   use `heightMap.SampleGrad(sampler, uv, dx * biasScale, dy * biasScale)` for
   every step. Map `textureBias` to a gradient scale (`exp2(bias)`).
3. **Height convention.** Add `heightIsDepth` (default false) to `Material`;
   shader uses `depth = heightIsDepth ? h : 1 - h`. Re-check CopperSlate and set
   its flag to whichever looks right; document the convention in the importer.
4. **Distance/footprint-scaled steps.** Estimate the on-screen parallax in pixels
   (`heightScale * texelsPerPixel`); skip POM when < ~0.5 px and scale step count
   with it instead of only the view angle. This makes POM free past a few metres.

Validation: oak_meadow and terrain_performance captures before/after; POM plane
test scene with CopperSlate at 0.5 m, 5 m, 30 m, grazing angles.

### Phase 1 — Compressed, usage-aware texture pipeline (1–2 weeks) — implemented 2026-11-13

Status: items 1, 3, 4, 5 done; item 2 only in the cook tool (see below). GPU
upload on DX11/DX12/Vulkan compiles and the loader is covered by
`.local/tests/TextureCompressionTests.cpp` (CTest `TextureCookCopperSlate` +
`TextureBlockCompression`), but an in-editor visual check is still pending.
- `GraphicsTextureFormat` gained `Bc1/Bc4/Bc5/Bc7`; size math lives in
  `GraphicsTextureMipBytes/ChainBytes/RowBytes` (IGraphicsTexture.h) and is
  used by all three uploaders, streaming and residency accounting. BC chains
  stream only when every level down to 4×4 is a whole number of blocks.
- `tools/TextureCook/TextureCook.cpp` (`TextureCook.exe`): PNG/JPG/TGA →
  zstd KTX2. Color → BC7 sRGB (linear-light mips), normal → BC5 (unnormalised
  average, renormalised), mask → BC7 linear (`--toksvig normal.png` widens
  roughness per mip), height/single → BC4. Non power-of-two sources are
  resampled to the nearest power of two. Usage: `--usage`, else
  `importSettings.textureUsage` in the source `.meta`, else the file name.
- Runtime: native BC KTX2 loads all stored mips as-is. Basis KTX2 transcodes to
  **BC7, not BC4/BC5**: libktx's Basis→BC5 reads G from the alpha slice, which
  is only correct for 2-channel normal-mode encodes. CPU readers
  (`GetPixels`, `DecodeDetached`) decode BC back to RGBA8 via bc7enc_rdo.
- Shader: `UnpackNormal()` rebuilds Z when blue is 0 (BC5), so legacy RGB
  normal maps and BC5 maps share one path (material and terrain normals).
- Not done: no editor UI for `textureUsage` and no import-time cooking;
  materials must point at the `.ktx2` explicitly.
- Test scene: `Scenes/Rendering/surface_detail_materials.scene` puts
  `CopperSlatePBRDemo` (PNG) next to `CopperSlatePBRDemo_BC` (cooked into
  `Textures/PBRDemo/Cooked/`) on panels and spheres, with BC ground and walls
  (2 m tiles via `Mesh/surface_detail_ground.obj` / `surface_detail_wall.obj`),
  a grazing side wall, and a low sun. Verified with GameBackendCapture on
  DX12, DX11 and Vulkan (use `--max-fps 60 --warmup 120` so the PNG set has
  time to decode). Visible baseline for Phase 2: the ground POM shows
  layer stair-stepping on the inlay walls near the camera.
- Fixed along the way: OBJ meshes had no tangents, so normal/height maps were
  silently skipped on every OBJ primitive (Mesh.cpp now generates them), and
  `Material::LoadFromFile` did not resolve `Assets/...` paths, so scene
  `materialAsset` links only worked when the scene also inlined the textures.

1. Extend `GraphicsTextureFormat` with `Bc1, Bc4, Bc5, Bc7` (+ sRGB flag already
   tracked). Update bytes-per-pixel → bytes-per-block math in streaming
   (`TextureStreamingPolicy`, `UploadMipTail`, mip allocation sizes) and the three
   `*GraphicsTexture.cpp` uploaders (row pitch = blocks).
2. Add a texture **usage** to `.meta`: `Color`, `Normal`, `Mask` (ORM/roughness),
   `Height`, `Linear`. Default from material slot when unset.
3. **Cook step** (editor import / `Scripts` tool) producing KTX2 with real BC data:
   - Color → BC7 sRGB. Normal → BC5 (store XY, reconstruct Z in shader).
   - Height → BC4 (or R16 for very large `heightScale`).
   - Mask → BC7 linear, or BC4 when single channel (terrain roughness).
   - Load path: when KTX2 is Basis, transcode to `KTX_TTF_BC7_RGBA` /
     `KTX_TTF_BC5_RG` instead of RGBA32.
4. **Better mips, offline.** Normal maps: average unnormalised vectors, then
   renormalise; store the lost length as variance and fold it into the matching
   roughness mip (Toksvig). Height maps: max-preserving or plain average (test
   both for POM stability).
5. Shader: `UnpackNormal()` helper that handles BC5 (2-channel) and legacy RGB.

### Phase 2 — POM v2 for general materials (1 week) — implemented 2026-11-13

Status: `ParallaxOcclusion()` in Object.hlsl marches half the authored steps
(max 32, one per two traversed pixels) then 4 binary steps + a secant step;
the earlier stair-stepping on the test scene's inlay walls is gone.
`ParallaxSelfShadow()` (6–16 steps by hit depth) multiplies directional-light
shadow when `Material::parallaxSelfShadow` is set, scaled by
`parallaxShadowStrength`, faded 10–15 m from the camera. `parallaxClampUv`
clips relief outside 0..1. Flags pack into `parallaxParams.w` (integer part:
1 depth, 2 self-shadow, 4 clamp; fraction: strength × 0.5), so the GPU struct
is unchanged. `CopperSlatePBRDemo_BC` enables self-shadow; verified on
DX12/DX11/Vulkan.
- Item 3: `parallaxDepthOffset` (flag 8) routes opaque, non-terrain draws to
  `m_objectParallaxDepthPipelines` (`PSParallaxDepthMain`,
  `SV_DepthGreaterEqual`, position input `noperspective centroid` as the
  validator requires). Parallax draws were already outside the depth
  prepass; inside portals they fall back to the flat-depth stencil-read
  pipelines. Verified with a sphere sunk into the BC ground on all backends.
  Vulkan needs the entry in the CMake SPIR-V list.
- Item 5: the material inspector's "Show Parallax Cost" (editor-only, not
  serialized, flag 16) shades fetches per pixel on the performance-heat ramp
  (blue 0 … red 32). Test scene ground: ~24 near with self-shadow, ~10 without.
- Cost: self-shadow on the test scene (most of the 1600×900 frame is POM)
  adds ~0.2 ms to `gpu_opaque_ms` on the RTX 4090 (≈0.29 ms at 1080p, at
  the 0.3 ms guardrail); it stays opt-in.
- Content: both CopperSlate materials now use `heightScale` 0.025 (was
  0.08 = 16 cm of relief on 2 m tiles). The height map's tops are authored
  bumpy (copper 0.76–0.95, slate 0.37–0.54), so at 0.08 they rendered as
  centimetre spikes. Relief this deep wants a smoothed height map for POM and
  the fine detail in the normal map; a `--height-smooth` cook option is the
  natural follow-up if a material needs a large scale.
1. Linear march with half the current steps + 4–5 binary/secant refinement steps
   (contact refinement).
2. **Soft self-shadow** for the main directional light only: march from the hit
   point toward the light in tangent space (8–16 steps, Tatarchuk's
   blocker/receiver ratio), multiplied into direct diffuse+specular. Gate by
   distance (< 15 m) and `materialDetail`.
3. **Optional pixel depth offset** (`parallaxDepthOffset` material flag): output
   `SV_DepthGreaterEqual` so early-Z stays partially effective. Requires these
   draws to be in the depth prepass with the same POM evaluation, or excluded from
   depth-equal (they already are). Off by default.
4. Edge handling: `parallaxClampUv` flag to discard/clamp outside [0,1] for
   decals and trim sheets.
5. Material inspector: live step-count / cost readout, debug views (height, POM
   step heatmap — reuse `PerformanceHeatColor` plumbing).

### Phase 3 — Terrain material overhaul (2–3 weeks) — first slice implemented 2026-11-14

Status (first slice: items 1, 3, 4, 5, 6, 8, 9 done; 2 and 7 open):
- **Arrays (1).** `IGraphicsTextureFactory::CreateTexture2DArray` on DX11, DX12
  (with a `TEXTURE2DARRAY` null descriptor) and Vulkan (a 1-slice white array
  default). `TerrainLayerArrays` builds two arrays on a worker thread, cached
  by the layers' texture paths: `albedoHeight` (sRGB rgb + height in A) and
  `normalRough` (normal XY, roughness, AO). They use logical slots 12/13
  (t27/t28, Vulkan bindings 28/29), replacing the 18 per-layer bindings.
  - Those bindings overlapped the light cookie/IES slots 16–23, and DX12 never
    bound the terrain normal/height slots; both problems are gone.
  - Update (later on 2026-11-14): the normal and roughness maps are now
    separate arrays, so there are three, in slots 12–14 (t27–t29, Vulkan
    bindings 28–30). `albedoHeight` is BC7 sRGB; `normal` and `roughAo` are
    BC5. Slices go up to 2048², about 100 MiB for six layers.
  - The BC encode runs on worker threads with bc7enc at uber level 0. Devices
    without BC sampling fall back to RGBA8 at 1024².
  - Mips are built on the CPU (normals renormalised, Toksvig roughness).
  - The first build takes seconds. The result is cached on disk at
    `%TEMP%/EngineTerrainLayerArrays/<hash>.bin`, keyed by source paths,
    sizes and write times.
  - Missing maps get neutral values. Until the arrays are ready the shader
    shows each layer's mean color.
  - `Terrain` no longer uploads each layer texture separately.
  - Open: cooking the arrays offline (TextureCook) so release builds ship
    them instead of encoding at runtime, plus uber level 2 quality.
- **Top-3 and height-lerp (3).** The shader keeps the three strongest vertex
  weights and applies the height-lerp (depth 0.2, scaled by
  `terrainHeightBlendStrength`), fading to linear blending with texture-LOD
  detail.
- **Hex-tiling (4).** Mikkelsen tiling with three rotated copies and a
  height-weighted contrast falloff. Sampling uses `SampleGrad`, and normals
  are rotated back into the UV frame. It runs within 24–40 m and is enabled by
  the manager's `stochasticTerrainVariation`; it is always on for standalone
  terrain.
- **Biplanar (5).** Faces steeper than about 37° (fully side-projected by
  about 49°) fade to world X or Z projections with a UDN normal blend. Side
  projections skip hex-tiling.
- **Distance detail (6).** From 32 to 96 m, a rotated 1/8-scale sample
  modulates brightness against the layer's mean color.
- **Fast tier (8).** AO is applied to ambient, and one GGX sun lobe is added
  within 96 m.
- **New material field.** `Material::terrainUvScale` sets per-layer texture
  density.
- **Content (9).** Six ambientCG CC0 2K sets in
  `Textures/Terrain/PBR/` (133 MB of source JPGs): sand, grass, forest soil,
  gravel, rock and snow. Their materials are in `Materials/Terrain/PBR/`, and
  `Scenes/terrain/terrain_materials.scene` is a test scene. Both are written
  by `Textures/Terrain/PBR/generate_terrain_pbr_content.py`. The streamed
  demo scenes still use the old 512² albedo-only materials.
- **Cost.** RTX 4090, 1600×900, full-screen terrain, uncapped (GPU at full
  clock):
  - Terrain pass: 0.37 ms total, against 0.20 ms with mean colors only.
  - Hex-tiling adds about 0.04 ms; biplanar costs nothing measurable on flat
    ground.
  - `terrain_performance` (fast tier): 0.18 ms.
  - Capped captures (60 fps pacing) leave the GPU at low clocks and vary 2–4×;
    measure uncapped, with a warmup long enough for the arrays to build.

1. **Texture arrays.** Build per-terrain `Texture2DArray`s: albedo(+height in A,
   BC7), normal (BC5), roughness/AO (BC4/BC7). Replace the 24 bindings and the
   `if (layer==N)` chains with `array.Sample(..., float3(uv, layerIndex))`.
   Requires all layers to share a resolution — enforce in the cook step (2K).
2. **Splat/ID map instead of vertex weights.** Per-chunk control texture
   (e.g. RGBA8: 2 layer indices + weight + reserved, or 4 indices + 4 weights)
   so paint resolution is independent of mesh density. Keep vertex weights as a
   migration path. Lifts the 6-layer limit (array of 16–32 layers).
3. **Top-N blending.** Shade only the 2–3 strongest layers per pixel (from the
   control map), with the existing height-blend improved to the standard
   "height-lerp": `w_i' = max(h_i + w_i - (max_j(h_j + w_j) - depth), 0)`.
4. **Hex-tiling** replaces `TerrainTextureOffset` + 2-sample blend for albedo
   and normal within ~40 m; single sample + macro color beyond. Use gradient
   sampling so hex seams don't break mip selection.
5. **Slope-gated biplanar mapping** for cliffs (slope > ~40°), with normal maps
   reoriented per projection.
6. **Distance detail blend**: second sample at UV/8–UV/16 blended in by distance
   to kill far repetition before the macro-color fade.
7. **Terrain POM**, near only (< ~15 m), on the blended height of the top-N
   layers, few steps (8–16). Expensive with arrays; keep behind a quality tier.
   Better delivered in Phase 4.
8. Fast-lighting tier (`8192`): keep as the far/low tier, but let it use the
   blended normal and a cheap specular term when within the near ring.
9. **Content**: replace the 512² procedural textures with CC0 2K PBR sets
   (albedo, normal, roughness, AO, displacement) and calibrate albedo to
   physically plausible values (~0.04–0.5 linear for soils/grass). This matters
   as much as any shader work.

### Phase 4 — Terrain runtime virtual texture / clipmap cache (3–4 weeks)
- Clipmap of 4–6 rings around the camera (e.g. 2048² per ring, ~10 texels/m in
  the inner ring), storing blended albedo+height (BC7 or RGBA8),
  normal (RG16 or BC5 after GPU compress), roughness/AO.
- Pages are rendered by a compute pass that runs the Phase 3 blend once; updated
  incrementally as the camera moves and on paint strokes
  (`TerrainManagerEditing`). Hooks into existing far-terrain cache invalidation.
- Terrain pixel shader becomes ~3 fetches + POM on the cached height. Decals
  (paths, tyre marks) can be baked into the same pages.

### Phase 5 — Geometric displacement near the camera (optional, 2–4 weeks)
- For terrain: denser near-camera patch (or hardware tessellation in DX11/DX12/
  Vulkan, or compute-generated micro-meshes) displaced by the Phase 4 height,
  fading to POM then flat with distance. Gives correct silhouettes, contact
  shadows and collisions-visuals agreement.
- For meshes: adaptive tessellation only for opted-in materials; POM remains the
  default.

## 4. Data / API changes summary

- `Material`: `heightIsDepth`, `parallaxSelfShadow`, `parallaxDepthOffset`,
  `parallaxClampUv`, `detailNormalTexture` + `detailNormalScale/tiling`.
- `ObjectData`: reuse `parallaxParams.w` for flags; add one `float4` for
  self-shadow strength / detail tiling. Keep `ObjectGPUData` in sync in
  `SceneRendererDrawPreparation.h`.
- `GraphicsTextureFormat`: BC formats; texture usage in `.meta`.
- Terrain: layer array resources + control map per chunk.

## 5. Performance guardrails
- Budget per pixel for opaque ground: ≤ 12 texture fetches typical,
  ≤ 32 worst case (near POM + self-shadow).
- POM/self-shadow/hex-tiling all fade out by distance; everything beyond ~50 m
  uses one sample per map + macro color.
- Track per-pass GPU time in the existing performance captures for
  `oak_meadow_expanded` and `terrain_performance`; any phase that costs > 0.3 ms
  at 1080p on the reference GPU needs a quality tier.

## Sources
- Tatarchuk, *Practical Parallax Occlusion Mapping* (SI3D 2006):
  https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/Tatarchuk-POM-SI3D06.pdf
- CryEngine POM / Silhouette POM notes:
  https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/1048721
- Mikkelsen, *Practical Real-Time Hex-Tiling* (JCGT 2022):
  https://jcgt.org/published/0011/03/05/paper.pdf — demo: https://github.com/mmikk/hextile-demo
- Ka Chen, *Adaptive Virtual Texture Rendering in Far Cry 4* (GDC 2015):
  https://gdcvault.com/play/1021761/Adaptive-Virtual-Texture-Rendering-in
- Unigine landscape terrain optimisation:
  https://developer.unigine.com/docs/future/content/optimization/terrain
- N. Reed, *Understanding BCn Texture Compression Formats*:
  https://reedbeta.com/blog/understanding-bcn-texture-compression-formats
- Terrain3D texture preparation (albedo+height / normal+roughness packing):
  https://terrain3d.readthedocs.io/en/latest/docs/texture_prep.html
