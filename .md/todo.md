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
- [ ] Grow world to 16–32 km with 800–2000 m relief (currently ~5.2 km, -120..124 m)
- [ ] Coarser spacing for far tiles; keep 4 m near the play area; verify far-terrain cache/LOD at that range
- [ ] Hydraulic erosion pass (particle-based) before writing `world.heights`
- [ ] Export flow and deposition maps alongside heights

### 4. Distant terrain color
- [ ] Bake low-res macro color map from height/slope/flow/deposition; drive `terrainMacroColors` from it at distance
- [ ] Use flow for lush valley floors / streams, deposition for scree, steep + high flow for bare rock
- [ ] Baked horizon / terrain-occlusion map for large-scale self-shadowing

### 5. Lighting + post
- [ ] Long-range shadow: extra far cascade or terrain horizon-map shadow beyond cascade range
- [ ] Bloom
- [ ] Color grading LUT (warm highlights, cool shadows)
- [ ] Sun disc / glare when looking toward the sun

### 6. Generated landscape assets (Python, same conventions as Tools/generate_meadow_accents.py)
- [ ] Conifer generator (`Tools/generate_conifers.py`): trunk + layered branch whorls with needle cards; ~4 species/size variants; LOD chain; mid/far quality
- [ ] Boulder / cliff generator: noise-displaced, faceted meshes, ground pivot, LODs, triplanar rock material
- [ ] Far-forest clusters / impostor cards baked from conifer + oak meshes; hook into TerrainManagerFarObjects
- [ ] Cloud and valley-mist sprite textures (noise-based) + soft particle/card prefabs
- [ ] Prefabs for all of the above under `Assets/Prefabs/Woodland/`, placeable by TerrainObjectLayer

### 7. Demo composition
- [ ] Landscape demo vantage point: elevated camera over a valley, backlit sun, dark foreground silhouette
