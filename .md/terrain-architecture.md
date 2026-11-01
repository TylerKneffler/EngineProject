# Terrain ownership and data

The engine has two terrain pipelines. Keep their roles distinct when adding
large-view-distance features.

| Area | Owner | Role |
| --- | --- | --- |
| Editable heightfield cells, streaming, LOD, collision, brush edits, render batches, and distant visual tiles | `Engine/Core/Compoonents/Obj/TerrainManager.*`, `Terrain.*` | Reusable runtime and editor terrain mechanics |
| Authored height and far-grid format | `Engine/Core/Compoonents/Obj/TerrainHeightArchive.h` | Reads and writes the saved heightfield data; the old script include forwards here for compatibility |
| Performance scene setup | `Engine/Core/Assets/Scripts/TerrainPerformance/TerrainPerformanceDriver.*` | Chooses the authored map and materials, loads the far-grid data into `TerrainManager`, sets preview camera, and reports measurements |
| Procedural chunk demo | `Engine/Core/Assets/Scripts/TerrainGen/` | Separate procedural shapes and chunk generator used by `Scenes/Procedural/terrain_gen*.scene` |
| Renderer integration | `Engine/Core/Scene/Rendering/SceneRendering.cpp`, `Engine/Core/Shaders/Object/Object.hlsl` | Shared terrain draw path and shading |

The performance scene is `Engine/Core/Assets/Scenes/terrain/terrain_performance.scene`.
Its heightfield source is `Engine/Core/Assets/TerrainData/TerrainPerformance/world.heights`;
`world.fargrid` is its coarse visual source. Terrain materials and textures live
under `Assets/Materials/Terrain` and `Assets/Textures/Terrain` respectively.

Data lifetimes differ:

- `world.heights` and `world.fargrid` are tracked authored assets.
- Sparse `.terrainedit` files are authored edits under the scene's
  `TerrainManager.cellDataDirectory`. `TerrainManager` reads and writes them.
- The inspector's optional runtime bake uses `TerrainManager.runtimeBakeDirectory`.
  If unset, the inspector defaults to a `BakedCells` child of
  `cellDataDirectory`. The existing `TerrainPerformance/BakedCells` folder is
  ignored generated data; the bundled scene currently leaves the runtime bake
  directory empty, so it does not use that folder.
- `TerrainPerformanceDriver.cacheDirectory` points to `.local/cache/terrain_performance`
  for regenerated mesh caches. This is disposable local data, keyed by source
  signature and appearance configuration.

New reusable heightfield rendering or streaming behavior belongs with
`TerrainManager` and `Terrain` under `Engine/Core/Compoonents`. Scene setup and
benchmark choreography belong in `Engine/Core/Assets/Scripts`. The procedural
chunk generator has different geometry and its own scene assets; any shared
streaming interface should be introduced explicitly rather than assuming it
uses heightfield cells.

## Player-local terrain collision

`TerrainManager.streamCollidersByDistance` uses the player's position inside
its cell, independently of terrain draw distance or loaded radius. The current
cell is active. Within `colliderActivationDistance` world metres of an edge,
its neighbor is also active. Near two intersecting edges, both neighbors and
the diagonal are active: at most four cells, including negative grid positions.
The default edge distance is 1 m and is clamped below half a cell per axis.

For streamed terrain, this applies to loaded cells. For authored terrain grids,
`manageAuthoredCellColliders` lets a manager discover Terrain descendants and
control their existing colliders without loading, unloading, or disabling their
rendering or scripts. The grass stress scene uses this mode under `Grass Terrain
Landscape`. Its serialized colliders start disabled; the manager activates the
player's region before the first physics step.

Inactive terrain bodies are suspended and removed from the physics world.
Bullet skips them before collider configuration and skeleton-reference searches.
Newly selected cells are enabled before physics runs. Performance hardware
samples report `terrain_collider_cells` and `terrain_active_collider_cells`.
The manager inspector also shows its active collider count.

Validation covers 1/2/4 active cells, negative coordinates, exact seams, finite
map edges, teleports, transformed landscapes, native body removal, raycasts, and
streamed cells. A standalone physics check on the 441-cell grass scene measured
0.65 ms average setup after initialization with one active terrain collider;
this excludes editor/rendering work and does not establish full editor FPS.
