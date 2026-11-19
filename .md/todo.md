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
- [x] DX11 vista differed from DX12/Vulkan (weaker sun glow, sparser far forest): not a rendering difference.
  DX11 captured the composed back buffer, DX12/Vulkan the HDR scene color before post-processing with a plain
  2.2 gamma, so their captures had no bloom, tone mapping or grading. DX12 now copies the back buffer after
  compose, Vulkan the swap-chain image after composition (swap chain gets `TRANSFER_SRC` usage; BGRA swizzled).
  Second cause: benchmark runs passed the host's real input to the game, so a capture window that ended up in
  the foreground let the user's mouse turn the camera during warmup (runs of one viewpoint diverged; Vulkan was
  off by ~40 degrees once). Benchmarks now get an unfocused, key-less snapshot; only synthetic input drives them.
  Vista now: DX12 == DX11 pixel for pixel, Vulkan differs on 0.22% of pixels, all three at the exact viewpoint.
  Captures taken before 2026-11-18 on DX12/Vulkan show pre-post-processing images
- [x] Vista pose re-picked for the 24 km map: eye (2304, 463, -3072) on a 410 m foothill at the valley's south-east
  rim, target (283, 254, -865): across the valley and a lake at the backlit north-west ranges, dark canopy in the
  foreground. Chosen from candidates found by searching the far grid for rim peaks with a deep drop toward the
  sun's azimuth and high ranges behind (A2 at (2688, 972, -4352) is a wider, flatter alternative). Set in
  `VISTA_EYE`/`VISTA_TARGET` and the scene's "Vista" camera. Numbers above this line used the old pose
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
- [x] Benchmarks measured while far objects still streamed (600 warmup frames was ~half of the 16k cells).
  `--wait-for-residency` now also waits for `TerrainManager::GetFarObjectStatus().settled` (window computed, no
  jobs or dirty blocks, every block's instances prepared, latched per block since its last rebuild); frame logs
  carry `far_object_cells`, `far_object_cells_queued`, `far_object_blocks`, `far_object_blocks_preparing`,
  `far_objects_settled`. The timeout message prints which condition is missing. Vista settles after ~109 s.
  - [x] Found on the way: the prefab source stat cache (`PreparePrefabInstances`) dropped each entry when it went
    1 s stale, so a layer visited less than once a second (round-robin under the 3 ms budget, ~300 layers) found its
    sources pending on every visit; a far block whose prefabs no other layer kept fresh never built. Stale entries
    now keep serving until the refresh lands
  - [ ] Fully settled vista measures 40.1 ms median / 47.5 p95 (DX12, one run, not A/B'd), far above the
    16.8-20.7 ms recorded with half the far cells streamed. Expected direction (see far-forest GPU item above) but
    the size needs a stage breakdown before trusting older numbers
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

(`--viewpoint Vista` was `--camera-position 1272,123,-12 --camera-target -893,28,1238` until 2026-11-18; it is now
`--camera-position 2304,463,-3072 --camera-target 283,254,-865`, so the tables below are from the old pose.)

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
- [x] `GameBackendCapture` with `--backend dx12` failed "renderer did not return the requested scene capture" when
  the window came out at 1520x873 instead of 1920x1080. Two bugs: (1) startup placement centred the benchmark
  window on the splash's monitor and shrank it to that monitor's work area (smaller or DPI-scaled monitor);
  benchmarks now skip `StartupSplash::PlaceWindow`. (2) DX12 mapped `rowPitch * height` of the readback, but the
  copy's last row is not pitch-padded, so widths not a multiple of 32 px (64 px for the RGBA8 back buffer copied now) overran the buffer and `Map` failed; it
  maps the footprint size now. `IGameRenderer::ReadSceneCaptureRGBA` returns the size actually captured (DX12 and
  Vulkan read the window size; Vulkan also recreates its readback buffer after a resize), and the benchmark writes
  the image at that size and says when it differs from the request. Checked DX12 at 1520x873 and 1920x1080, and an
  oversized 3840x2400 request (Windows caps the window at 2564 px wide here) on DX12/Vulkan/DX11. The capture tool's
  default scene (`oak_meadow.scene`, removed) is now `terrain/landscape_demo.scene`
- [x] Fog particles (CPU, GPU, fluid surface) and MMD outlines. Particles read a per-pass `particleFog` buffer
  (t7 / Vulkan binding 8: camera position + fog0..3, since particle constants already fill the 128-byte push
  constant); alpha particles blend to the in-scatter, additive ones fade by transmittance
  (`AtmosphereFogTransmittance`); the fluid surface fogs at its density-weighted particle center. Checked on DX12,
  DX11, Vulkan with a dense-fog copy of particle_showcase; fluid river runs on DX12/Vulkan. MMD outline fog
  compiles but was not seen in a capture
- [x] Fog the skybox seen through portals: `ViewLighting::SetSkyboxFog` now sets the sky fog terms for the main sky
  and portal skies alike (portal skies use the connected view's eye height). With fog forced on in
  portal_traversal_demo the sky inside the aperture matches the main sky exactly (213, 219, 226)
- [x] Tune defaults against the step 0 test shot: on the new vista the defaults (density 2e-4 at height 0, falloff
  0.004 = 250 m scale height, sky distance 30 km) give the layered look: haze pools on the far valley floor and
  around the ridge bases while the peaks and the foreground stay crisp. Thinner, deeper fog (8e-5..1.2e-4 with
  830..500 m scale heights) washed the whole range to one flat tone. The landscape scene never enabled fog; it
  does now, with those values written out (the generator carries `<settings>` over)

### 2. Terrain scale + erosion (Tools/build_landscape_demo.py)
- [x] Grow world to 16–32 km with 800–2000 m relief (now 24.2 km, -56..1888 m; archive v3 stores its own scale, 1/16 m)
- [x] Coarser spacing outside the play area (16 m samples, upsampled on read); 4 m within 3 km of the start
- [x] Verify far-terrain cache/LOD in-engine at 24 km: confirmed faceted. Far tiles take vertices only at 128 m cell
  corners (the archive is read just for seam cells), so distant peaks were a few flat-shaded triangles. The macro
  color map now carries a 16 m world-space normal map (same grid and mapping, texture slot 2,
  `terrainMacroParams.w`), and distant tiles are lit per pixel with it whenever a macro or long-range shadow map
  is bound: the ranges shade with eroded ridges, gullies and spurs. Same on DX12/DX11/Vulkan
- [ ] Far-terrain silhouettes are still the 128 m polygon outline (visible on peaks against the sky). A finer far
  grid (e.g. 32 m from the archive's 16 m samples) needs sub-cell vertices in the far-tile mesher
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
- [x] Forest-density map baked with the macro map (macro alpha = canopy cover; the color takes the canopy tint), and
  distant tiles shade forest as a bumpy canopy: `ApplyDistantCanopy` (Object.hlsl) tilts the 16 m macro normal with
  world-space crown/clump/stand noise (14/42/130 m) and darkens its hollows, each octave fading out once a pixel spans
  half its period. TerrainManager "Distant canopy relief" (0..1, default 0.6, tooltip), packed as
  `terrainMacroParams.w = 1 + relief`. Subtle in practice: the canopy albedo is very dark, and within
  `farObjectRadius` (8 km) the far tree impostors cover it; with impostors off it changes 2.7% of the vista's
  pixels, all on the far forest. No specular on distant tiles, so no canopy roughness

### 4. Lighting + post
- [x] GTAO (half-res, temporally filtered) for contact darkening under trees and in valleys:
  `Shaders/AmbientOcclusion/AmbientOcclusion.hlsl` (compute) after the main view's draws
  (`SceneRendererLighting::ComputeAmbientOcclusion`). New context call `CopySceneDepth` copies the scene depth into
  a buffer (DX12 `CopyTextureRegion` of the depth plane; Vulkan `vkCmdCopyImageToBuffer` while the scene pass is
  paused; DX11 a small compute copy from a now-typeless depth; recorded in command packets, gated by
  `SupportsSceneDepthCopy`). GTAO per half-res pixel: view-space position/normal from depth, 2 slices x 2 sides x
  6 steps (rotated per pixel and frame), cosine-weighted arc integration, then blended with last frame's AO
  reprojected through the camera stored in the output's header (depth-checked). Applied next frame through a new
  structured slot 47 (DX12 root SRV t47, Vulkan binding 48) to ambient + environment (diffuse and specular) only,
  never sunlight: object surfaces, foliage cards, grass, fast terrain and per-pixel distant tiles
  (`ScreenAmbientOcclusion` in Object.hlsl: reprojects, 4-tap depth-matched filter, 1 off screen / disoccluded).
  Scene `ambientOcclusion` (`enabled`, `radius` 1.5 m, `intensity`, `temporalBlend` 0.15); editor block. Landscape
  demo: on. No measurable frame cost on DX12 or Vulkan (vista and near view, 4090). Notes:
  - fxc compiled `continue`/`break` inside the unrolled GTAO loops into a GPU hang (TDR); the loops are now
    branch-free. One later DX12 run still froze GPU timings once and has not reproduced in 12 runs since; the
    debug layer is clean. `ENGINE_GPU_DEBUG=1` now prints DX12 debug-layer errors/warnings to stderr
  - Editor viewports (D3D11View / D3D12View / VulkanView) don't register their depth yet, so AO is game-view only
  - Dense foliage shows some half-res blotchiness; a depth-aware spatial filter pass would smooth it
  - Vulkan's AO GPU timer includes the barrier wait for the opaque pass to drain (reads ~1.3 ms; frame time
    shows no cost)
- [x] Auto exposure (luminance histogram, eye adaptation) with editor min/max EV and speed: three small passes
  in each backend's post-process before compose (`Shaders/PostProcess/PostProcess.hlsl`
  `ExposureLuminancePS` / `ExposureHistogramPS` / `ExposureAdaptPS`, shared plumbing in `Graphics/PostProcess.*`).
  64x32 log-luminance map (centre-weighted metering) -> 64-bin histogram over -14..+10 EV -> 1x1 adapt pass that
  trims the histogram to [lowPercent, highPercent], clamps to [minEv, maxEv] and eases from the previous frame
  (two ping-pong RGBA32F targets, separate up/down speeds, snaps after resize or re-enable). Compose maps the
  adapted average to mid grey; `outputExposure` stays as compensation. GPU-only, no readback. Lit views only.
  Scene `autoExposure` (`enabled`, `minEv`, `maxEv`, `speedUp`, `speedDown`, `lowPercent`, `highPercent`);
  editor block under Output Tone Mapping. Landscape demo: on, compensation -0.4 (vista meters at -2.7 EV, about
  where the fixed exposure was). DX11/DX12/Vulkan meter identically. Note: fxc mis-compiled a nested,
  partially unrolled histogram loop (DX counted ~1/10 of the map); it's one flat `[loop]` now. Not separately
  GPU-timed (64 + 1 pixels reading a 16 KB map)
- [x] Long-range shadow: terrain sun-visibility map ray-marched over the far height grid on a worker thread when the sun moves (`TerrainManagerSunShadow.cpp`); near terrain multiplies it into the cascade shadow, distant tiles are lit per pixel with it. TerrainManager `longRangeShadows`, `longRangeShadowSoftness`
- [ ] Long-range shadow for non-terrain objects (far trees/impostors stay sunlit inside a ridge shadow; needs a global texture slot on all three backends)
- [x] Bloom: half-res 13-tap downsample / tent upsample mip chain in each backend's compose pass (`Shaders/PostProcess/PostProcess.hlsl`, shared by DX11/DX12/Vulkan). Scene `bloomEnabled`, `bloomIntensity`
- [x] Color grading (warm highlights, cool shadows): analytic split-tone + contrast + saturation after tone mapping. Scene `colorGrading*`, `grading*`
- [ ] Optional authored 3D LUT input for grading (the split tone is analytic today)
- [x] Sun disc / glare: limb-darkened disc + Henyey-Greenstein glow in `Skybox.hlsl` toward the brightest directional light; bloom provides the glare. Scene `sunDisc*`, `sunGlowIntensity`

### 5. Sky computed from the sun
- [x] Hillaire-2020-style sky: `Rendering/Atmosphere/PhysicalAtmosphere.*` (Rayleigh/Mie/ozone media, Bruneton
  transmittance LUT 256x64, Hillaire multiple-scattering LUT 32x32, planet geometry in double) and
  `PhysicalSky.*`, which renders the sky on a worker into a 512x256 RGBA32F world-space panorama (with mips)
  whenever the sun moves >0.2°, its illuminance >1%, the camera altitude >100 m or the parameters change.
  The panorama replaces the sky-view LUT: it feeds the existing skybox draw and EnvironmentMap unchanged, so
  sky, reflections and ambient all come from one image. CPU-side because the generic graphics context has no
  offscreen color targets; the GPU only samples. Scene `physicalSky` (`enabled` is the HDRI toggle; HDRI is
  used until the first panorama is ready), `groundHeight`, `rayleighScale`, `mieScale` (haze), `mieScaleHeight`,
  `mieAnisotropy`, `ozoneScale`, `groundAlbedo`, `skyIntensity`; editor "Physical Sky" block. The skybox's
  analytic sun glow is off over the physical panorama (the aureole is in it); the sun disc stays.
  Landscape demo: enabled, `mieScale` 4. Tests: `.local/tests/PhysicalAtmosphereTests.cpp`
- [x] Derive sun light color and ambient SH from the same atmosphere model: the strongest directional light is
  multiplied by the transmittance toward the sun at the camera altitude (`attenuateSun`), and the environment
  map / SH are prefiltered from the physical panorama on the same worker
  (`SceneRendererLighting::ApplyPhysicalAtmosphere`)
  - Found while doing this: the split-sum BRDF LUT had lost its `/ sampleCount` in the 11-09 refactor
    (`RenderUtilities.h` `BrdfIntegration::Integrate`), making specular IBL ~256x too bright. It went unseen
    because PNG HDRI skyboxes drop their CPU pixels after upload, so `EnvironmentMap::Acquire` returns null and
    those scenes get no environment lighting at all (still true for the HDRI path; separate issue). Fixed, with
    a LUT range check in PhysicalAtmosphereTests
- [x] Replace step 1's analytic fog with the aerial-perspective volume behind the same shader include:
  `AtmosphereFog.hlsli` has a physical mode (fog0.w >= 100, packed by `ViewLighting::PackPhysicalAerialPerspective`)
  used by objects, particles and outlines with no call-site changes; light shafts keep working
  (`aerialPerspective` toggle, height fog otherwise). Deviation: it's analytic, not a froxel volume:
  per-channel Rayleigh + Mie extinction with their own exponential height profiles integrated along the
  ray, sun single scattering (Rayleigh + HG phases, sun illuminance at the camera) and Hillaire's
  multiple-scattering term, combined as color*T + S*(1-T)/tau. Exact for the lower atmosphere at
  terrain distances. A froxel volume needs 3D textures and a global texture slot, which the generic graphics
  context doesn't have yet
  - [ ] Froxel aerial-perspective volume (32x32x32, compute) once 3D textures exist; would add cloud/terrain
    shadowing of the in-scatter beyond the current shaft march
- [x] Volumetric clouds: `Shaders/Clouds/Clouds.hlsl`, a fullscreen pass blended (premultiplied) over the sky right
  after it, so terrain and objects drawn later occlude it. Flat layer (base 2600 m, 1400 m thick, above the
  1.9 km peaks), ray-marched per pixel (48 steps, jittered): 3D value-noise FBM shaped by a 2D weather/coverage
  noise, cumulus profile (flat bases, threshold rising with height for domed tops), Worley billows and value-noise
  edge erosion; 5-step light march toward the sun (Beer-Lambert with a multiple-scattering floor), two-lobe
  Henyey-Greenstein phase, sky light brighter toward the tops; fades out toward max distance into the hazed sky.
  Wind drifts the field. `SceneSettings::volumetricClouds` (coverage, density, base, thickness, shape scale, edge
  detail, wind speed/direction, sky light colour/intensity, sun intensity, steps, max distance), serialized, in
  scene Properties > Atmosphere with tooltips, read every frame (live in the editor). Constants: 128 bytes in an
  extra slot of the sky constant ring (`SceneRenderer::kCloudConstantSlot`), Vulkan SPIR-V built by CMake.
  Enabled in landscape_demo. Sky pass (sky + clouds) 1.0-1.3 ms GPU on the vista, same on DX12/DX11/Vulkan
  - [ ] Cloud shadows on terrain and objects (project the coverage/density toward the sun into the sun light)
  - [x] Half-resolution march: `Clouds.hlsl` `CSMain` ray-marches one thread per half-res pixel into a structured
    buffer (`SceneRenderer::m_cloudSamples`, retired on resize) in a compute pass before the sky
    (`SceneRendererLighting::MarchVolumetricClouds`); `PSMain` upsamples it bilinearly (graphics slot 6, the lights
    are rebound after). The grid size and step count are bit-packed into `shape.w` to stay within 128 bytes of
    constants. New GPU stage `Clouds` (`gpu_clouds_ms`). Vista cost 1.33 ms -> 0.61 ms on DX12 (march 0.58 +
    sky/composite 0.03), DX11 0.46, Vulkan 0.82; image nearly identical (edges marginally softer). Vulkan's
    `StorageBarrier` now also covers fragment-shader reads (it only covered compute/vertex/indirect)
  - [ ] Temporal reprojection (march a quarter of the pixels per frame) if the march needs to get cheaper still
  - [ ] Clouds in portal views and fog/aerial perspective between camera and cloud

### 6. Generated landscape assets (Python, same conventions as Tools/generate_meadow_accents.py)
- [x] Conifer generator (`Tools/generate_conifers.py`): trunk + layered branch whorls with needle cards; ~4 species/size variants; LOD chain; mid/far quality
  Spruce_tall_01 (24 m), Spruce_young_01, Pine_scots_01, Fir_subalpine_01 under `Prefabs/Woodland/Conifers`. LOD0 sprays
  (3-14k tris) / LOD1 fewer larger sprays with card coverage matched to LOD0 (side coverage within ~5-20%) / LOD2
  whorl stars / view-baked impostor (6 views + top, 38 tris, `impostorBillboard`, same card layout as the oak pack).
  Shared procedural needle atlas (spruce, fir, pine, clump cells) and bark; species colour is muted linear vertex colour
  (inner/outer/tip, per-branch jitter). Instanced chain resolves to LOD0 -> LOD1 -> impostor. Shared helpers:
  `tools/woodland_assets.py` (native .mesh writer, numpy rasterizer/impostor baker, PNG I/O, prefab/material writers)
- [x] Boulder / cliff generator: noise-displaced, faceted meshes, ground pivot, LODs, triplanar rock material
  `tools/generate_boulders.py`: Boulder_01-03, Boulder_flat_01, Cliff_block_01/02 under `Prefabs/Woodland/Boulders`.
  Superellipsoid cut by random planes (smooth-min) + FBM, cliffs with vertical cuts and stepped strata; one radius
  function sampled on icospheres per LOD; normals split at 38 degree creases. New `Material::triplanarScale`
  (world-space 3-projection base/ORM/occlusion/normal with UDN blend, projections < 4% skipped; packed in
  `textureUvSets1.z`, a terrain-only lane) with the Rock030 set; meshes keep box UVs at the same scale
- [x] Far-forest clusters / impostor cards baked from conifer + oak meshes; hook into TerrainManagerFarObjects
  `tools/generate_forest_clusters.py` bakes 4 stands (2 oak from the pack's LOD2 meshes + material textures, 2 conifer)
  of 14 trees on 36 m. `TerrainManager::farClusters` (asset, match text, baked tree count) + `farClusterDistance` /
  `farClusterSize` / `farClusterMinimumTrees`: far blocks wholly beyond the distance bucket their trees on the grid and
  replace squares where one family is >= 70% with one cluster (centroid, scale sqrt(count / baked)). Landscape demo:
  2.2 km, 40 m, 5 (scene + `tools/build_landscape_demo.py`)
- [x] Check alpha-to-coverage / coverage-preserving alpha mips so distant canopies do not thin out or shimmer
  Real-time rendering is single-sample (MSAA only in DX11 video export), so A2C does not apply. Measured: PNG foliage
  gets runtime box-filtered mips, which at the 0.32 cutoff grow oak canopy/impostor coverage 14-60% by mip 4-7 (crowns
  bloat and pulse) and drop sapling impostors to 25-48%. `tools/cook_foliage_textures.py` cooks every Mask material's
  PNG base colour with `TextureCook --alpha-cutoff` (push-pull fill + per-mip coverage rescale, BC7) and repoints the
  19 materials; generated foliage is cooked the same way. `--check` prints the per-mip table
- [x] Cloud and valley-mist sprite textures (noise-based) + soft particle/card prefabs
  `tools/generate_sky_sprites.py`: Cloud_Puff_A/B, Mist_Wisp_A/B, Mist_Sheet (`Textures/Atmosphere`), prefabs
  ValleyMist / LowClouds (ParticleEmitter) and MistCard (Blend mesh) under `Prefabs/Woodland/Atmosphere`. Soft
  particles: `ParticleEmitter::softness` fades against the main view's depth copy (reused from AO, or copied when AO is
  off; `SceneRendererLighting::SceneDepthForPixelShaders`, particle t8), plus `nearFadeDistance`, `fadeInTime`,
  `sizeVariation`, `randomRotation`. Portal views draw hard. Verified on DX12/DX11/Vulkan
- [x] Prefabs for all of the above under `Assets/Prefabs/Woodland/`, placeable by TerrainObjectLayer
  Conifers/, Boulders/, Clusters/, Atmosphere/. Landscape demo uses them through its layers: `LandscapeDriver`
  grows conifers by altitude (`ConiferShare`: oaks below 350 m, mixed foothills, conifers from 710 m to the tree
  line; picked from existing draws so the lowland oak layout is unchanged) and puts the generated boulders in rock
  fields; far impostors and clusters follow automatically. `Scenes/terrain/woodland_assets_preview.scene` shows every
  asset at its LODs; captured on DX12, DX11 and Vulkan
  Demo update: cliff blocks join the forest layer on steep rock below the snow (<= 2 per cell, upright, sunk by the
  slope; 900 m reach). They are authored `cullAfterLastLod`, which `GetObjectLodProfile` now passes through and
  `FarPlacements` skips, so near-only props never become far objects (~30% of cells are steep). The scene gets an
  "Atmosphere" group (`build_landscape_demo.atmosphere_items`): LakeMist (new wide prefab) over all five lakes,
  ValleyMist + three mist cards on the first lake by the start, LowClouds on four summits in the vista
- [x] Editor: run generators from an editor tool and refresh the asset browser
  Tools > Asset generators lists `tools/generate_*.py` / `cook_*.py` (docstring as tooltip), plus "Run all woodland
  generators" (ordered queue). `AssetGeneratorRunner` runs one at a time without a console window, streams output to
  the Console, then reloads rewritten textures (`Texture::Invalidate`) and refreshes open scenes
  (`OnAssetContentsChanged`); the asset browser lists new files through its directory watch. Builds; not yet exercised
  in a running editor
- [x] Wider, less bright tree and foliage colours: `Material::foliageColorVariation` (masked foliage; world-space noise
  at ~9 m crown and ~70 m stand scale trades yellow-green against blue-green and +-~20% value; `textureUvSets1.w`) on
  oak leaves/impostors (0.7), conifers and clusters (0.55/0.7), identical across LODs. Oak leaf + impostor tint
  [0.86, 0.84, 0.77] (darker, less saturated); terrain meadow layer `grass.material` [0.24, 0.32, 0.12] ->
  [0.20, 0.26, 0.11]

### 7. Demo composition
- [ ] Final landscape demo vantage point: elevated camera over a valley, backlit sun, dark foreground silhouette
- [ ] Re-run the step 0 benchmark; target 100+ fps on RTX 4090
