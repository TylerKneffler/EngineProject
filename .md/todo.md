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
- [ ] Make the vista pose part of the landscape scene (a second camera / viewpoint the editor can select), not only a CLI argument
- [ ] Vertical dashed line artifact in the sky, left of center in the vista capture: identify (debug draw? far object LOD?)
- [ ] Depth precision: forward Z with D32F and near 0.06 gives ~25 m depth steps at 5 km and ~100 m at 10 km. Switch to reverse-Z (compare GREATER, clear 0, near/far swapped in projection, depth reads in shaders/occlusion/Hi-Z) before step 2 grows the world; also raise Camera.far (9000 today)
- [ ] Camera-relative precision check at 16 km from origin (transforms are double, check vertex/shader paths)

Vista capture (RTX 4090, DX12, 1920x1080, RelWithDebInfo, uncapped):

```
ENGINE_ATMOSPHERE_FOG=0|1 build/RelWithDebInfo/GameBackendCapture.exe --backend dx12 \
  --scene Engine/Core/Assets/Scenes/terrain/landscape_demo.scene --width 1920 --height 1080 \
  --warmup 600 --frames 300 --max-fps 0 --require-terrain --wait-for-residency 240 \
  --camera-position 1272,123,-12 --camera-target -893,28,1238 --out .local/vista/<name>
```

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
- [ ] Shadowed in-scatter (light shafts): march the cascade shadow map along the view ray (few jittered steps, resolved by temporal pass) or via the froxel volume of step 2
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
- [ ] Bake low-res macro color map from height/slope/flow/deposition; drive `terrainMacroColors` from it at distance
- [ ] Use flow for lush valley floors / streams, deposition for scree, steep + high flow for bare rock
- [ ] Forest-density map baked with the macro map; far terrain shades forested areas as canopy (canopy color/roughness + canopy normal/height noise so hills read as bumpy forest without instances)
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
