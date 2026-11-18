# Engine TODO

## Landscape look (target: large-scale vista with layered haze)

Every landscape feature below must be usable from the editor, not only from
Python scripts or hand-edited scene JSON: settings live in SceneSettings or a
component, show in the Properties view with tooltips, serialize with the scene,
and update live. Offline generators (erosion, asset generators) get an editor
entry point (button/tool window) that runs them and reloads the result.

Order is by dependency and visual payoff, not by section number in older notes.

### 0. Baseline + test shot
- [x] Fixed-pose capture: `GameBackendCapture --camera-position x,y,z --camera-target x,y,z` (recorded in the session header)
- [x] GPU `Sky` timing stage (`gpu_sky_ms` in captures and the editor frame profiler)
- [x] `ENGINE_ATMOSPHERE_FOG=0|1` env override for fog A/B captures
- [x] Rough vista pose (peak over a valley, see command below)
- [x] Vista pose is a scene viewpoint: inactive Camera object "Vista" in landscape_demo.scene (emitted by
  `tools/build_landscape_demo.py`, `VISTA_EYE`/`VISTA_TARGET`). Scene View toolbar "Viewpoints" menu jumps the
  editor camera to any scene camera and sets the selected camera from the current view
  (`Camera::GetWorldPose/SetWorldPose`); captures take `--viewpoint Vista` (recorded as `viewpoint` in the header)
- [x] Vertical dashed line in the sky: the equirect panorama's u wrap (atan2 jump at world -X) gave implicit
  derivatives a full-texture step, so 2x2 quads straddling it sampled the smallest mip. Skybox.hlsl now uses
  `SampleGrad` with wrap-aware u derivatives (checked on DX12, DX11, Vulkan)
- [x] Black sky after the 2026-11-17 architecture merge: the D3D `SkyboxCB` kept `float4x4 invVP` ahead of the new
  ray-basis fields while C++ `SkyboxConstants` dropped it, so every sky/fog/sun constant read 64 bytes off. Removed
- [ ] DX11 vista differs from DX12/Vulkan: weaker sun glow, sparser far forest; not investigated
- [ ] The vista pose predates the 24 km regeneration: eye at 123 m is now low in the valley rather than on a peak.
  Re-pick it from the editor (Viewpoints > Set "Vista" from Scene View) and copy it into `VISTA_EYE`/`VISTA_TARGET`
- [x] Depth precision: forward Z with D32F and near 0.06 gave ~25 m depth steps at 5 km and ~100 m at 10 km. Every
  depth buffer (camera, portal views, shadow atlases) is now reverse-Z; `Graphics/DepthConvention.h` holds the
  convention (clear 0, `CompareNearer` = GREATER as the builder default, `CompareNearerOrEqual`). Projections are
  built with near/far swapped in double (`Camera::GetRenderProjectionMatrix`, `BuildStableShadowCascade`);
  `Camera::GetProjectionMatrix` stays forward-Z for editor picking/overlay math. Shader sites: shadow compares
  (`receiver + bias >= stored`), skybox/grid far-plane triangles and grid plane depth, fluid front depth, parallax
  `SV_DepthLessEqual`, portal aperture near clamp (`z = min(z, w)`) and depth-reset far plane (`z = 0`),
  sky ray unprojection, `ProjectedTextureFootprint` near test. The software occlusion buffer and GPU cull work in
  clip w, and frustum tests are symmetric in z, so neither changed. Verified: DX12 vista vs HEAD 0.10% of pixels
  differ (noise floor 0.01%, all in distant canopy), Vulkan matches, DX11 unchanged apart from its known sky
  difference; portal_traversal_demo, room_larger_inside (DX12/DX11/Vulkan), particle_fluid_river and
  surface_detail_materials match HEAD up to animation/streaming timing; `.local/tests` pass. Vista frame time
  unchanged (A/B on the same machine: HEAD 26.3/27.0 ms, reverse-Z 25.9/26.5 ms). Not checked: editor grid
  (editor-only pass, math changed in `Grid.hlsl`)
- [x] Vista "regressed" from ~10 ms (2026-11-16) to ~26.5 ms at `ec249b3d`. Two causes, neither a code regression:
  - Unfocused captures were throttled. Since background launches (`ec249b3d`), the capture window never has focus
    and Windows 11 power throttling (EcoQoS) ran every CPU stage 2-3x slower on this hybrid CPU. The old 5.2 km
    world at the old pose, current code: 25.2 ms throttled, 10.4 ms with `Engine::Core::DisablePowerThrottling()`
    (`Startup/LaunchActivation`), i.e. the 11-16 number. GameBackendCapture always opts out; Game/Editor opt out
    when launched in the background. Captures from before 11-17 18:00 ran focused; later ones before this fix were
    throttled, so neither set compares with new numbers
  - The 24 km world is heavier at the same pose: 16k far-object cells (radius 64 x 128 m) instead of ~1.6k
  - [x] `TerrainManager::UpdateFarObjects` rescanned the whole far window each frame (map lookups per cell, a sort
    of every uncomputed cell) and rebuilt a block after every 8-cell batch: 4.4 ms/frame plus constant prefab
    re-preparation. Now incremental: the nearest-block-first queue is rebuilt only when the center cell changes,
    only loaded/latched cells are checked for near coverage, and a block rebuilds once its data is complete
    (coverage/radius changes still apply at once). 2-6 far jobs instead of 2. 4.4 -> 0.02 ms; drive-forward
    A/B: median 29.0 -> 21.4 ms, far time over the drive 7.7 s -> 0.46 s
  - [x] Far blocks re-hashed every placement once per second per block (`GetPlacementSignature`) to catch
    in-place edits they never get: `TerrainObjectLayer::placementsChangeOnlyWhenMarked`. Prefabs 3.0 -> 1.4 ms
  - [x] Texture footprints: shadow cascades are orthographic, so each instance chunk's world box with its
    largest scale bounds all its shadow demand in one projection; chunks 4x their diagonal away also use the box
    for the camera. Per-instance work is now camera-only for near chunks. 5.2 -> 2.5-2.9 ms, image identical
  - Result (DX12, 1920x1080, settled after 5000 warmup frames): 24.8 -> 16.8-20.7 ms. CPU path now update 1.7 +
    prepare 5.0 + record 10.5 ms; the GPU varies 11-20 ms between runs (other apps share the GPU)
- [ ] With all far blocks loaded the vista is GPU-bound on far forest: foliage depth 6-11 ms + leaves 3.5-7 ms,
  at detail_scale 0.82-0.94 because far blocks are outside the detail budget. Belongs with section 6's far-forest
  clusters/impostors, or let the far blocks' density follow the budget
- [ ] Benchmarks measure while far objects still stream (600 warmup frames is ~half of the 16k cells): add far
  objects to `--wait-for-residency`, or warm up ~5000 frames
- [ ] Cell-crossing spikes while moving (old and new far code alike): ~35 ms far-block membership rebuilds plus
  ~22 ms near-cell streaming on the same frames; spread block rebuilds over frames
- [ ] Camera-relative precision check at 16 km from origin (transforms are double, check vertex/shader paths)

Vista capture (RTX 4090, DX12, 1920x1080, RelWithDebInfo, uncapped):

```
ENGINE_ATMOSPHERE_FOG=0|1 build/RelWithDebInfo/GameBackendCapture.exe --backend dx12 \
  --scene Engine/Core/Assets/Scenes/terrain/landscape_demo.scene --width 1920 --height 1080 \
  --warmup 600 --frames 300 --max-fps 0 --require-terrain --wait-for-residency 240 \
  --viewpoint Vista --out .local/vista/<name>
```

(`--viewpoint Vista` is the same pose as the older `--camera-position 1272,123,-12 --camera-target -893,28,1238`.)

Baseline 2026-11-16 (medians, ms):

| metric | fog off | fog on |
|---|---|---|
| frame interval | 20.00 | 19.87 |
| simulation update | 3.17 | 3.12 |
| scene prepare | 5.08 | 4.75 |
| command record (camera 8.5, shadow 1.9) | 11.52 | 11.55 |
| GPU total of stages | 5.93 | 5.89 |
| GPU opaque (other 1.6, leaves 1.5, foliage depth 1.8, bark 0.2) | 5.28 | 5.22 |
| GPU terrain / shadow / sky | 0.53 / 0.10 / 0.02 | 0.55 / 0.10 / 0.02 |

Finding: the vista is CPU-bound at ~50 fps. GPU work is ~6 ms; update + prepare + record (~20 ms) add up to the frame
interval, so they appear to run serially. 100 fps needs the CPU critical path under 10 ms: camera command recording
(8.5 ms) is the largest item, then scene prepare. Height fog costs nothing measurable.

- [ ] Profile and cut CPU frame cost for the vista (camera command recording, scene prepare)
  - Why serial: `SimulationLoop` (Game/Application/GameApplication.cpp) runs Update, then PrepareRenderFrame, then
    Render (shadow + camera recording) on one scene-owner thread while the window thread waits; only replay is on the
    render thread. Overlapping update N+1 with recording N needs a scene snapshot boundary; bigger change, do after the
    per-stage cuts below.
  - [x] `SceneTextureFootprints::Request` projected every instance into the camera and all shadow cascades on one
    thread (3.8 ms). Now split into 512-instance chunks on RenderJobs: 1.0 ms. New capture fields
    `render_material_draw_loop_ms`, `render_material_footprint_ms`, `render_material_record_upload_ms`.
  - [x] Frame item preparation did two `dynamic_cast` component scans per object per frame (~3.8k objects); terrain
    components now come from `FrameComponentLookup`: `prepare_items_ms` 4.3 -> 3.3.
  - [x] Sampling profiler without admin rights: `GameBackendCapture --cpu-sample` samples every thread during the
    measured frames and writes `<out>/cpu-profile.txt` (per-thread inclusive/exclusive + caller chains;
    `.local/tests/CpuSampler.h`). Adds sampling overhead: use it to find hotspots, not for timings.
  - [x] Biggest finding: no shader cache. Object.hlsl entry points take ~21–23 s each in fxc (Water.hlsl PSMain 25 s),
    so every launch spent 60–100 s compiling and the compiles were still running on the job workers during
    measurement, starving the frame's parallel work. Added a persistent DX11/DX12 bytecode cache
    (`Renderers/D3DShaderCache`, key = preprocessed source incl. all includes + entry/target/flags/compiler version;
    `%TEMP%/EngineShaderCache`, `ENGINE_SHADER_CACHE_DIR`, `ENGINE_SHADER_CACHE=0`). Warm startup 65.6 s -> 1.8 s.
  - [x] Particle rendering scanned every object with `GetComponent<ParticleEmitter>` four times per frame (~15% of
    the simulation thread) in a scene without emitters; now uses the component registry.
  - [x] Remaining per-draw `GetComponent<Terrain/TerrainBatchAppearance>` in Render/PrepareTerrainMaterial use the
    frame item's `terrainSurface` flag.
  - [x] Standalone game publishes scene snapshots without per-object contents (only the editor reads them):
    simulation update 3.6 -> 2.3 ms.
  - [ ] Shader compile time itself: Object.hlsl ~21 s per entry in fxc (the fog include added ~2 s). Look at what
    fxc unrolls/flattens (lighting loops, terrain layers); cold-cache startup is still ~65 s
  - [x] `Material::Validate` (0.75 ms, cache misses over ~40 fields of each large per-object Material) now runs as a
    parallel pre-pass over the frame lookups before the serial item loop: 0.15 ms. Prefab LOD materials still
    validate inline. Shader prewarm dedupe no longer builds a `tuple<string,string,bool>` per item (transparent set).
  - [x] Texture footprints re-project each 512-instance chunk every 4 frames (staggered, per view; last result is
    re-submitted in between) via `Rendering/TextureFootprintCache` owned by the Scene: 2.55 -> 1.5 ms, same
    residency. Remaining: the parallel join (~60%), serial gather, `RequestGraphicsFootprint` mutex per texture slot
  - [x] `TerrainLayerArrays::Acquire` built a string key (paths + formatted tints) per terrain draw per frame; now
    an identity fast path (texture pointers + tints, weak refs guard address reuse). `PrepareTerrainMaterial`
    looked up the parent TerrainManager twice. Material draw loop 1.3 -> 0.9 ms
  - [x] GrassField visibility loop: per-frame `std::set` of desired tiles replaced by a refresh serial, tile Material
    cached (was a component scan per tile): grass 0.94 -> 0.42 ms with ~1000 active tiles. Captures now carry
    `grass_*_ms` phases
  - [ ] GrassField::LoadSources (0.18 ms) re-derives species LOD paths from string maps every frame, including a
    by-value copy of `ObjectLodProvider::GetObjectLodMeshes()`; needs revisions on its inputs to memoize
  - [ ] Next hotspots (warm cache, simulation thread): item preparation 2.3, TerrainObjectLayer::
    PreparePrefabInstances, Material::PrepareTextures (~4% exclusive), BuildOcclusionSignature, TransformHierarchy::
    Prepare, TerrainManager::UpdateFarObjects (`std::map::count` per far cell), heap allocation ~5%,
    exception-handling frames (`_NLG_Return2`) ~2%

Looking around (`GameBackendCapture --look-yaw <pixels/frame>` feeds synthetic mouse-look through the normal input
path; pin detail with `ENGINE_DETAIL_SCALE=2.5` for comparable runs):

- [x] Turning stuttered: `PersistentInstanceBatches` keys include bound texture and mesh-buffer pointers, which
  texture streaming replaces constantly while the view turns, so every new key created four D3D12 buffers (16% of
  the simulation thread, upload p95 8.7 ms). Batches unused for 8 frames now park their buffers per frame slot and
  new keys reuse them. Look (adaptive detail): median 15.2 -> 13.9 ms, p95 24.8 -> 19.2, max 38 -> 26
- [x] Software occlusion buffer (rebuilt every frame while turning) now starts in the camera's shadow pass, which
  runs first and computes the same view; the color pass adopts it when camera, frame serial, viewport and
  view-projection match bit for bit (`EarlyCameraOcclusion`, joined by every color pass, frame preparation and
  `FenceBackgroundWork`; `ENGINE_EARLY_CAMERA_OCCLUSION=0` disables). Interleaved A/B while turning: occlusion
  wait 0.78 -> 0, command record 7.0 -> 6.5 ms, frame 13.1 -> 12.6 ms
- [x] `Material::PrepareTextures` residency check moved into the parallel validation pre-pass
  (`NeedsTexturePreparation`, read-only); serial texture preparation 0.37 -> 0 ms, items 2.33 -> 2.11 ms
- [x] Shader prewarm skips materials whose shader, asset path and terrain use are unchanged since their last
  prewarm under the same epoch (`ShaderSourceCache::Generation()` + `m_materialShaderBytecodeEpoch`); the check
  runs in the parallel pre-pass. Prewarm 0.29 -> 0 ms, items 2.0 -> 1.5 ms, prepare 2.9 -> 2.46 ms
- [x] Tried: texture pointers out of the instance batch key (per-frame occurrence ordinal instead). No
  measurable change (upload 0.40 vs 0.38 ms while turning): the remaining upload is visible tile membership
  changing with the view, not texture churn. Reverted
- [ ] Shadow record +0.2 ms from sharing workers with the early occlusion job

Vista 2026-11-16 end of day (adaptive detail, median/p95/p99 ms): static 10.0/12.5/14.6 (was 12.0/14.8/19.1);
fast turn `--look-yaw 20` 10.3/14.2/16.8 (was 15.2/24.8/30.3); slow turn `--look-yaw 6` 10.9/14.7/17.7
(was 12.0/17.8/20.6). Pinned detail 2.5 static: 10.2-10.35. Single runs vary by up to ~1 ms on this machine:
compare stage timers or interleaved A/B pairs, not single frame medians
- [x] Detail budget oscillated: the predictive mode grew its cost budget whenever the GPU was under budget, but the
  GPU measures the *applied* scale, which glides far behind the target, so the budget wound up (0.49M -> 1.37M,
  target at the 8x maximum) and geometry overshot to 10-14 ms. Now the budget comes straight from a calibration
  (smoothed GPU ms / smoothed predicted cost of the applied scale), cuts in 0.2 s and grows over the response time;
  the controller follows a lower target in 0.1 s; and when *measured* GPU time is >1.1x the budget, detail is shed
  up to 5x faster than the anti-popping glide. `.local/tests/DetailBudgetTests.cpp` runs the closed loop in
  simulated time (static, fast view changes, and a model whose predictions run opposite to real cost).
  Captures (adaptive): static 12.0/14.8/19.1 -> 10.6/12.5/13.9 ms (median/p95/p99), geometry max 10.2 -> 7.0;
  fast turn 13.9/19.2/23.0 -> 12.6/16.5/19.1, geometry max 14.2 -> 7.4
- [ ] The cost model's per-view predictions do not track GPU cost while turning: views it predicts as densest
  (~300k) measure cheapest (~2.4 ms) and vice versa (likely occlusion the model's coarse grid misses). The budget
  calibration swings 150k..350k with the view. Compare predicted vs measured per view direction
- [ ] Slow turn (`--look-yaw 6`) p95 ~17 ms: frames facing dense content are CPU-bound (camera record ~7 ms, prepare
  ~5 ms, CPU render ~8.8 ms vs the 4 ms CPU budget, which model mode ignores). Either let the CPU render budget
  limit the model target or take the serial update/prepare/record pipeline apart (see "Why serial" above)
- [ ] One 1.77 s hitch in one slow-turn capture (frame 299: `prepare_assets_ms` 8.5, 5 prefab layers started
  streaming; no recorded stage accounts for the time). Not reproduced in three more runs; watch for it
  - [ ] Detail model: between the round-3 and round-4 captures (other edits to shadows/terrain object layers/
    Object.hlsl landed) the vista's `detail_scale` rose 2.03 -> 2.48 and footprints 0.95 -> 2.55 ms. Check whether
    the detail budget should count the CPU footprint pass, since it scales with placements

Vista after CPU work (warm shader cache, same command, two runs): frame 12.6 / 12.8 ms (~79 fps, was 20.0);
update 2.3, prepare 4.3, record 5.7 (camera 4.0, shadow 1.4); GPU ~5.8 ms.

Vista 2026-11-16 round 5 (`--frames 900`; detail_scale 2.48 vs 2.03 above, so not directly comparable):
before validation/footprint changes 13.2 ms; after 11.8 ms (~85 fps), p95 14.1. Update 2.2, prepare 3.3,
record 5.9 (camera 4.5, shadow 1.1); GPU opaque 5.4.

### 1. Aerial perspective + height fog
- [x] Per-pixel analytic height fog in the forward shaders via one shared include (`Shaders/AtmosphereFog.hlsli`); no per-material fog code
- [x] Extinction/inscatter blend: `color * T + inscatter * (1 - T)` (Beer–Lambert), exponential height falloff so haze pools in valleys
- [x] Henyey-Greenstein sun-glow term for in-scatter toward the sun, using the scene's main directional light
- [x] Sky horizon haze from the same parameters (skybox shader)
- [x] Scene settings + editor UI ("Atmosphere" in scene Properties), serialized with the scene
- [x] Shadowed in-scatter (light shafts): `ObjectFogSunVisibility` (Object.hlsl) marches the cascade shadow map
  along the view ray in quadratically spaced segments (finer near the camera); each segment's analytic fog opacity
  is weighted by one bilinear shadow tap, dithered per pixel with interleaved gradient noise (static: there is no
  temporal pass yet). Only the sun-glow share of the in-scatter is shadowed (`ApplyAtmosphereFogShadowed`); fog past
  the last cascade split is sunlit. The range uses the view-wide splits, so lighting bands do not cut shafts at
  object edges. Settings: Light Shafts / Samples (1-32, default 8) / Strength, packed into `fog0.w` (no new
  `ObjectGPUData` field). Opt-in: 8 samples cost ~2 ms GPU on the Vulkan vista (terrain 1.03 -> 1.97, leaves
  4.26 -> 5.19 ms). `ENGINE_LIGHT_SHAFTS=0|N` overrides for A/B captures. `AtmosphereFogSettingsTests` covers
  round trip, clamps and the `fog0.w` encoding. Checked: FXC ps_5_0 + DXC SPIR-V compile; on the vista the change
  only darkens (forest within shadow range), no seams
  - [ ] Sky pixels are not shadowed (the skybox has no shadow map), so shafts never cross open sky
  - [ ] Animate the dither and resolve it once a temporal pass exists; 4-6 samples may then suffice
  - [ ] Not yet seen as distinct shafts: needs a low sun behind trees/ridges with moderate fog. Judge with the step 0
    shot when tuning defaults (below)
- [ ] `GameBackendCapture` on the landscape with `--backend dx12` fails "renderer did not return the requested scene
  capture" (fog on or off; Vulkan and the default scene work). Captures render at 1520x873 for a 1920x1080 window;
  `DX12GameRenderer::ReadSceneCaptureRGBA` assumes the window size. Suspect render scale
- [ ] Fog the remaining passes: particles (incl. fluid), MMD outlines, and the skybox seen through portals (portal object draws already fog)
- [ ] Tune defaults against the step 0 test shot after step 2 sets world scale

### 2. Terrain scale + erosion (Tools/build_landscape_demo.py)
- [x] Grow world to 16–32 km with 800–2000 m relief (now 24.2 km, -56..1888 m; archive v3 stores its own scale, 1/16 m)
- [x] Coarser spacing outside the play area (16 m samples, upsampled on read); 4 m within 3 km of the start
- [ ] Verify far-terrain cache/LOD in-engine at 24 km: the far grid is one height per 128 m cell corner, which may facet the 2 km ridges on the horizon (a finer far grid would need engine work)
- [x] Hydraulic erosion pass (particle-based) before writing `world.heights`
- [x] Export flow and deposition maps alongside heights (`world.flow`, `world.deposition`; format in the script docstring)
- [ ] Editor tool: terrain generation window (size, relief, seed, erosion iterations) that regenerates and reloads the landscape

### 2b. Terrain editor tools (done; needs an artist pass in the editor)
- [x] Realtime brushes: Erode (hydraulic), Weather (thermal), Noise, Terrace
- [x] Select tool: rectangle / ellipse / lasso / polygon with feather, draped highlight, Apply commits one undo step
- [x] Region operations: hydraulic + thermal erosion, smooth, noise, terrace, flatten (Ctrl+click picks height), raise/lower, paint layer

### 3. Distant terrain color + forest coverage
- [x] Macro color map at 16 m (1513^2, the erosion grid) instead of the far palette's one color per 128 m cell
  corner. `LandscapeDriver` composes it on a worker from the near paint rules (`PaintSample`), slope, and the
  generator's `world.flow`/`world.deposition` (`TerrainHydrologyMap.h`, validated against the archive signature);
  `TerrainManager` uploads it as an sRGB mip chain and binds it in texture slot 5 (`ObjectGPUData::terrainMacroMap`,
  `terrainMacroParams`). Distant tiles take their albedo from it; loaded cells blend to it between
  `macroColorNear`/`macroColorFar` (256/640 m) so both sides of the loaded-cell border match. Alpha holds forest
  cover. Editor: TerrainManager "Terrain appearance" (Macro color map, start/full distance, tooltips) and
  LandscapeDriver "Macro color" group (map paths, lushness, flow threshold, scree, scree depth, gully rock,
  brightness, with tooltips via the new `Component::RegisterFieldTooltip`). Draw preparation polls the composer's
  settings every 100 ms, so edits rebuild it live in the editor and in play (0.8 s per build). Verified on DX12,
  DX11 and Vulkan: slopes gain gully and scree detail, mid-distance ground fades into the far colors
- [x] Use flow for lush valley floors / streams, deposition for scree, steep + high flow for bare rock (in the macro
  map, i.e. at distance; the near paint does not read the erosion maps yet)
- [ ] Macro colors use the materials' authored means while the near cells' distant blend uses measured layer-array
  means; at 0.85 brightness distant meadows read a little duller than textured near meadows. Tune, or feed the
  measured means into the composer
- [ ] Forest-density map baked with the macro map (done: macro alpha = canopy cover, and the color already takes the
  canopy tint); far terrain shades forested areas as canopy (canopy color/roughness + canopy normal/height noise so hills read as bumpy forest without instances)
- [ ] Baked horizon / terrain-occlusion map for large-scale self-shadowing
- [ ] Editor: paintable forest-density / macro-color overrides on the terrain component

### 4. Lighting + post
- [ ] GTAO (half-res, temporally filtered) for contact darkening under trees and in valleys
- [ ] Auto exposure (luminance histogram, eye adaptation) with editor min/max EV and speed
- [x] Long-range shadow: terrain sun-visibility map ray-marched over the far height grid on a worker thread when the sun moves (`TerrainManagerSunShadow.cpp`); near terrain multiplies it into the cascade shadow, distant tiles are lit per pixel with it. TerrainManager `longRangeShadows`, `longRangeShadowSoftness`
- [ ] Long-range shadow for non-terrain objects (far trees/impostors stay sunlit inside a ridge shadow; needs a global texture slot on all three backends)
- [x] Bloom: half-res 13-tap downsample / tent upsample mip chain in each backend's compose pass (`Shaders/PostProcess/PostProcess.hlsl`, shared by DX11/DX12/Vulkan). Scene `bloomEnabled`, `bloomIntensity`
- [x] Color grading (warm highlights, cool shadows): analytic split-tone + contrast + saturation after tone mapping. Scene `colorGrading*`, `grading*`
- [ ] Optional authored 3D LUT input for grading (the split tone is analytic today)
- [x] Sun disc / glare: limb-darkened disc + Henyey-Greenstein glow in `Skybox.hlsl` toward the brightest directional light; bloom provides the glare. Scene `sunDisc*`, `sunGlowIntensity`

### 5. Sky computed from the sun
- [ ] Hillaire-2020-style sky (transmittance LUT, sky-view LUT, aerial-perspective froxel volume); keep the HDRI skybox path as a toggle
- [ ] Derive sun light color and ambient SH from the same atmosphere model
- [ ] Replace step 1's analytic fog with the aerial-perspective volume behind the same shader include
- [ ] Cloud layer (panorama/cards to start) and valley mist sprites

### 6. Generated landscape assets (Python, same conventions as Tools/generate_meadow_accents.py)
- [ ] Conifer generator (`Tools/generate_conifers.py`): trunk + layered branch whorls with needle cards; ~4 species/size variants; LOD chain; mid/far quality
- [ ] Boulder / cliff generator: noise-displaced, faceted meshes, ground pivot, LODs, triplanar rock material
- [ ] Far-forest clusters / impostor cards baked from conifer + oak meshes; hook into TerrainManagerFarObjects
- [ ] Check alpha-to-coverage / coverage-preserving alpha mips so distant canopies do not thin out or shimmer
- [ ] Cloud and valley-mist sprite textures (noise-based) + soft particle/card prefabs
- [ ] Prefabs for all of the above under `Assets/Prefabs/Woodland/`, placeable by TerrainObjectLayer
- [ ] Editor: run generators from an editor tool and refresh the asset browser

### 7. Demo composition
- [ ] Final landscape demo vantage point: elevated camera over a valley, backlit sun, dark foreground silhouette
- [ ] Re-run the step 0 benchmark; target 100+ fps on RTX 4090
