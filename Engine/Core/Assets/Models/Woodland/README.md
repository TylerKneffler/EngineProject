# Oak Meadow

Open `Assets/Scenes/terrain/oak_meadow.scene` and press Play. WASD moves, mouse looks, Shift sprints.

The scene uses a painted three-layer ground, a winding footpath and seventeen oak varieties. Source oak LODs are selected by authored distance from the entrance; these are static scene choices, not a runtime LOD switch. Trees are separate editable prefabs under `Assets/Prefabs/Woodland/Oaks`.

For performance, the authored terrain grid uses 16x16 cells, there is no grass streaming system, no shrub-prefab layer, and the sun uses no realtime shadow casting in this scene.

`Mesh.foliageWindStrength` enables the shared root-pinned wind field for authored meshes. The oak meadow scene intentionally keeps tree trunks and canopies at zero wind so they remain static and do not trigger shadow-map churn from shader-driven vertex motion. The same vertex path drives color and shadows, with bounds padding for wind. Bark and canopies keep source PBR shading.

Regenerate with `python Scripts/Assets/build_oak_meadow.py --prepare-only`, then `python Scripts/Assets/build_oak_meadow.py` (numpy/scipy and a built Debug Editor required). Existing imported packs are reused. Imports use the native glTF importer; generated intermediates live under `.local/woodland-import`. See ATTRIBUTION.md for CC-BY-4.0 credits.
