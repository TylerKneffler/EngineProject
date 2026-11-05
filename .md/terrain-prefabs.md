# Shared terrain prefab placements

The oak forest is authored as 2,048 `TerrainObjectLayer` placement records beneath the terrain. Each record keeps its prefab asset, terrain-local position, normal, yaw, scale and persistent placement ID. The object brush writes these records, retaining the existing scene history callback and terrain selection. Mesh-only grass layers retain their existing renderer.

`TerrainPrefabInstances.cpp` derives resident render batches from these records. The current forest has 85 shared sources (LOD0 bark/leaf, LOD1 bark/leaf and one 28-triangle impostor for each of 17 varieties), and 2,700 resident spatial batches. Parts and levels share one placement array per variety/tile. GPU selection follows the camera, with complementary dither fades at 28 m and 90 m. Generated objects remain runtime-only and excluded from saves and Play snapshots. See `tree-runtime-lod.md` for backend validation and asset regeneration.

All 51 oak prefab assets currently have their primitive collider and rigid-body components removed. The generator no longer adds them. Ground and player collision are retained.

## Future collider edits

Edit the oak variety's **LOD0/base prefab** in `Assets/Prefabs/Woodland/Oaks` and add its primitive collider there. The layer's existing `objectLods` definitions identify the base prefab for the LOD1/LOD2 visual variants, so they inherit the same collider policy. The renderer checks saved prefab timestamps once per second and rebuilds derived instances when definitions change. Adding, changing or removing the collider affects all matching placements, including variant paths supplied by the brush. If the prefab supplies no rigid body, generated collider objects use static bodies.

This path is for static terrain scenery. It does not instantiate arbitrary gameplay scripts or skinned animation per placement; those remain ordinary prefab-instance use cases.

Regenerate just the forest placement layer with:

```powershell
python Scripts/Assets/build_oak_meadow.py --forest-only
```

## Validation

`TerrainSharedPrefabs` uses a CPU buffer recorder rather than creating a graphics device. It checks shared vertex-buffer allocation, placement transforms under a translated layer, serialization exclusion, collider addition/edit/removal across base and LOD placements, absolute/relative prefab paths, and all 2,048 actual oak placements. It also checks that the forest has no generated tree collider or rigid body.

The Release editor builds. Camera movement, transition fades, same-tile independent selection, persistent placement reuse and visible directional shadows pass on DX11, DX12 and Vulkan. Actual oak entrance/overlook captures, shadow comparisons and terrain underside checks also pass on all three backends. The headless regression covers base/LOD collider edits and all authored placements. Raw results are under `.local/tree-lod-*`.
