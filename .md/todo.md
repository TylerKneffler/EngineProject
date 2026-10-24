# Engine roadmap

Last updated: 2026-10-23.

Status convention:

- `[x]` Complete
- `[ ]` Incomplete

## Next priorities

# Terrain component feature

- [x] Add a reusable Terrain component with configurable X/Y cell counts and N/M cell dimensions, editable height samples, and generated mesh geometry.
- [x] Persist terrain dimensions, height data, and material layers with scene serialization.
- [x] Show terrain tools when a selected object has Terrain: brush size/strength and raise, lower, average, blend/smooth operations in the scene viewport.
- [x] Support a list of layers with height based assignment and manual painting; preview the weights through vertex colors.
- [x] Load terrain layer material assets and blend base color textures using painted weights in the viewport shader (up to six layers); use material or preview colors when a texture is unavailable.
- [x] Integrate terrain creation and inspection into editor component workflows.
- [x] Add focused headless regression tests for terrain mesh generation, brush editing, painted weights, and persistence.
- [x] Add editor brush ray integration coverage and a DX11 rendered viewport regression for painted material blending.

### Large-world terrain performance

Target: a roughly 225–230 km² world (about 15 km across) that can be traversed end to end. At 128 m per cell, a 15.36 km square has 120 × 120 = 14,400 cells. The current 16.5 km test scene has a 20,000-cell cap, enough to populate its full grid as the camera traverses it.

Earlier baseline (Release DX11, 960×540, this machine, dirt/grass textures, before far-field LOD): radius 2 / 256 m / 25 cells averaged about 108 FPS after loading; radius 3 / 384 m / 49 cells averaged about 91 FPS. With a two-cell load budget, radius-2 streaming averaged 13.6 ms per frame and peaked at 21.8 ms in the measured route. Rebenchmark with far-field LOD enabled before quoting an FPS target.

- [x] Store procedural cells as coordinates, regenerate unedited heights on reload, and retain full terrain data only for edited cells. Raise the test-scene cap above the 16.5 km grid and verify 14,400 coordinates without resident meshes or JSON payloads.
- [x] Cache generated static cell heights, painted weights, and mesh geometry on disk as cells are visited; restore them on reload and regenerate when terrain settings or source materials change.
- [x] Add an offline full-map cache bake and compact the cache format. The Release baker prepared all 16,641 cells in about 0.75 GiB of compact files.
- [x] Persist a compact world manifest with row runs for procedural cells and independent sparse height/paint edit files. Save and reload edits at opposite map edges without rewriting the other edge's data.
- [x] Add distance-based LOD meshes and coarse far-field terrain so hills remain visible several kilometers away while nearby cells stay editable at full resolution. Stitch LOD borders without cracks.
- [x] Cap synchronous cell loads and unloads per update and load nearby cells first, reducing sudden full-radius rebuilds.
- [x] Generate noise, meshes, and cell data asynchronously; cap GPU uploads and prioritize cells in the travel direction. A short Release 960×540 radius-2 streaming run averaged 10–12 ms with one upload per frame; a frame above 30 ms remains to profile on the full-map route.
- [ ] Reduce terrain draw calls and material bindings through batching or instancing; add frustum, distance, and shadow-pass culling. Profile CPU and GPU costs before and after.
- [ ] Keep terrain coordinates stable across at least 15 km using double-precision world positions or origin rebasing. Verify camera motion, picking, painting, collision, and shared borders near opposite edges.
- [ ] Define bounded CPU/GPU memory and cache budgets for a 15.36 km map; measure peaks while traversing, teleporting, and backtracking.
- [ ] Add a repeatable Release route across the full map at 1080p. Record median and 95th-percentile frame times, worst streaming stall, visible distance, resident cells, CPU/GPU timings, and memory; target about 100 FPS with kilometer-scale terrain visibility.
- [ ] Preserve cross-cell brush editing and saved edits through LOD changes and load/unload cycles; add regressions at cell, LOD, and world-coordinate boundaries.


### Particle systems and fluid-ready emitters
