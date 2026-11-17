# Engine TODO

## Landscape look (target: large-scale vista with layered haze)

Ordered by visual payoff.

### 1. Aerial perspective + height fog
- [ ] Fullscreen pass after the opaque passes and before tone mapping; reconstruct world position from depth
- [ ] Extinction/inscatter blend: `color * T + inscatter * (1 - T)` (Beer–Lambert)
- [ ] Height-falloff density (exponential), so haze pools in valleys
- [ ] Henyey-Greenstein sun-glow term (g ~ 0.7) for in-scatter toward the sun
- [ ] Expose density, height falloff, inscatter color, and sun-glow strength in ProjectSettings / scene
- [ ] Apply to terrain, foliage, water, and sky horizon consistently (no per-shader fog)

### 2. Sky computed from the sun
- [ ] Replace static equirect skybox with Hillaire-2020-style sky (transmittance LUT, sky-view LUT, aerial-perspective froxel volume)
- [ ] Derive sun light color and ambient SH from the same atmosphere model
- [ ] Feed the aerial-perspective volume into the fog pass from step 1
- [ ] Cloud layer (panorama/cards to start) and valley mist sprites

### 3. Terrain scale + erosion (Tools/build_landscape_demo.py)
- [x] Grow world to 16–32 km with 800–2000 m relief (now 24.2 km, -56..1888 m; archive v3 stores its own scale, 1/16 m)
- [x] Coarser spacing outside the play area (16 m samples, upsampled on read); 4 m within 3 km of the start
- [ ] Verify far-terrain cache/LOD in-engine at 24 km: the far grid is one height per 128 m cell corner, which may facet the 2 km ridges on the horizon (a finer far grid would need engine work)
- [x] Hydraulic erosion pass (particle-based) before writing `world.heights`
- [x] Export flow and deposition maps alongside heights (`world.flow`, `world.deposition`; format in the script docstring)

### 3b. Terrain editor tools (done; needs an artist pass in the editor)
- [x] Realtime brushes: Erode (hydraulic), Weather (thermal), Noise, Terrace
- [x] Select tool: rectangle / ellipse / lasso / polygon with feather, draped highlight, Apply commits one undo step
- [x] Region operations: hydraulic + thermal erosion, smooth, noise, terrace, flatten (Ctrl+click picks height), raise/lower, paint layer

### 4. Distant terrain color
- [ ] Bake low-res macro color map from height/slope/flow/deposition; drive `terrainMacroColors` from it at distance
- [ ] Use flow for lush valley floors / streams, deposition for scree, steep + high flow for bare rock
- [ ] Baked horizon / terrain-occlusion map for large-scale self-shadowing

### 5. Lighting + post
- [x] Long-range shadow: terrain sun-visibility map ray-marched over the far height grid on a worker thread when the sun moves (`TerrainManagerSunShadow.cpp`); near terrain multiplies it into the cascade shadow, distant tiles are lit per pixel with it. TerrainManager `longRangeShadows`, `longRangeShadowSoftness`
- [ ] Long-range shadow for non-terrain objects (far trees/impostors stay sunlit inside a ridge shadow; needs a global texture slot on all three backends)
- [x] Bloom: half-res 13-tap downsample / tent upsample mip chain in each backend's compose pass (`Shaders/PostProcess/PostProcess.hlsl`, shared by DX11/DX12/Vulkan). Scene `bloomEnabled`, `bloomIntensity`
- [x] Color grading (warm highlights, cool shadows): analytic split-tone + contrast + saturation after tone mapping. Scene `colorGrading*`, `grading*`
- [ ] Optional authored 3D LUT input for grading (the split tone is analytic today)
- [x] Sun disc / glare: limb-darkened disc + Henyey-Greenstein glow in `Skybox.hlsl` toward the brightest directional light; bloom provides the glare. Scene `sunDisc*`, `sunGlowIntensity`

### 6. Generated landscape assets (Python, same conventions as Tools/generate_meadow_accents.py)
- [ ] Conifer generator (`Tools/generate_conifers.py`): trunk + layered branch whorls with needle cards; ~4 species/size variants; LOD chain; mid/far quality
- [ ] Boulder / cliff generator: noise-displaced, faceted meshes, ground pivot, LODs, triplanar rock material
- [ ] Far-forest clusters / impostor cards baked from conifer + oak meshes; hook into TerrainManagerFarObjects
- [ ] Cloud and valley-mist sprite textures (noise-based) + soft particle/card prefabs
- [ ] Prefabs for all of the above under `Assets/Prefabs/Woodland/`, placeable by TerrainObjectLayer

### 7. Demo composition
- [ ] Landscape demo vantage point: elevated camera over a valley, backlit sun, dark foreground silhouette
