# Oak Meadow

Open `Assets/Scenes/terrain/oak_meadow.scene` and press Play. WASD moves, mouse looks, Shift sprints.

The scene uses a painted three-layer ground, a winding footpath, seventeen oak varieties, nine shrub varieties and four grass patch variants. Source oak LODs are selected by authored distance from the entrance; these are static scene choices, not a runtime LOD switch. Grass uses nested whole-blade subsets for its three levels. All plants are separate editable prefabs under `Assets/Prefabs/Woodland`.

Grass is streamed in two textured instanced layers using the existing GrassField system.

`Mesh.foliageWindStrength` enables the shared root-pinned wind field for authored meshes. Grass bends strongly, shrubs moderately, and oak trunks/canopies share restrained motion. The same vertex path drives color and shadows, with bounds padding for wind. Bark and canopies keep source PBR shading; grass and shrubs use the existing fast foliage shading with their imported textures.

Regenerate with `python Scripts/Assets/build_oak_meadow.py --prepare-only`, then `python Scripts/Assets/build_oak_meadow.py` (numpy/scipy and a built Debug Editor required). Existing imported packs are reused. Imports use the native glTF importer; generated intermediates live under `.local/woodland-import`. See ATTRIBUTION.md for CC-BY-4.0 credits.
