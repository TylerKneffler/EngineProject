# Terrain performance cache bake

Run these commands from the repository root to prepare every cell before play:

```powershell
cmake --build build/Release --target TerrainCacheBake --config Release
build/Release/Release/TerrainCacheBake.exe
```

The performance scene covers 129 × 129 cells (16,641 total). The baker reads
its noise and terrain settings, writes compact CPU mesh data into
`.local/cache/terrain_performance`, and releases each temporary cell before
building the next. Existing valid files are reused, so an interrupted bake can
be resumed with the same command. Use `--force` to rebuild every cell,
`--limit 16` for a short trial, or `--output directory` for another cache root.
When using `--output`, set the scene driver's `cacheDirectory` to the same
directory before play.

The cache stores heights, layer weights, normals, and colors. Positions, UVs,
painted texture channels, and grid indices are reconstructed when a cell loads.
The runtime keeps full resolution near the camera and selects 2, 4, or 8
sample LOD meshes farther away, out to a 32-cell (4.1 km) radius. Every LOD
keeps all cell-border height samples so adjacent meshes share their edge
segments. The cache format and offline bake command are unchanged.

The performance scene prepares cached or generated cell data, sparse edits,
and CPU meshes on a background worker. Completed cells enter the scene on the
main thread, with one geometry upload per update. Its bounded preparation
queue prioritizes nearby cells in the direction the viewer entered a new cell.
The `TerrainManager` inspector exposes the queue and upload budgets.
The current 32 × 32 cells use about 48 KB each, about 0.75 GiB for the full
map. The files contain no GPU resources and can be loaded by the rendered
scene. Material file changes or a changed noise configuration regenerate
affected cache data. Painted terrain edits remain in the scene's separate
`TerrainData` directory.
