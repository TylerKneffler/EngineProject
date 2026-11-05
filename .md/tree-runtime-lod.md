# Shared terrain prefab tree LOD

The oak forest stores 2,048 base-variety placements. Each variety has three resident visual bands: the imported LOD0 mesh below 28 m, imported LOD1 below 90 m, and an authored 28-triangle impostor beyond. The original LOD2 assets remain available for alternative chains. Material changes between levels are supported; impostors use their own atlas and alpha-tested material.

Prefab-part transforms are baked into each shared source once. Every part and LOD in a variety/tile shares one persistent placement array. Camera movement dispatches GPU selection and changes neither authored placement records nor resident geometry. Tile metadata is cached; conservative per-placement bounds retain boundary overlaps. A complementary world-space dither blends the two neighboring bands over one metre, avoiding a hard distance switch. This uses fades, not stateful hysteresis.

Camera and directional-shadow passes use the same distance thresholds and complementary dither intervals. The layer shadow range still limits casters. Independent LOD bands are part of the persistent indirect-buffer cache key: without that distinction, deferred DX12/Vulkan commands could consume another band's output when buffers share identical geometry.

The current three-band prefab path produces 85 shared sources and 2,700 resident spatial batch objects for the forest. The higher resident batch count includes alternative LODs, not simultaneous visible draws for every level. Runtime-derived objects are excluded from saves and Play snapshots. Collider policy still comes from the base prefab and saved edits propagate across visual variants.

Reimport the supplied impostors and migrate the scene with `python Scripts/Assets/build_oak_impostors.py`. This imports the original 28-triangle meshes and UVs, matches the existing ground pivots, writes native mesh/material/prefab assets, and changes forest placements to base varieties. Regenerating the forest with `build_oak_meadow.py --forest-only` preserves the runtime chains when impostor assets exist.

`TreeLodBackendTests` moves a camera near/far/back and through both transition bands, uses distinct material colors to assert the selected level, and checks that resident geometry and placement owners remain unchanged. The fixture intentionally shares the same vertex buffer between all bands to exercise deferred-buffer isolation. CTest names are `TreeLod_dx11`, `TreeLod_dx12`, and `TreeLod_vulkan`. The headless prefab regression checks all 2,048 placements, 85 sources, shared placement arrays, serialization exclusion and collider edits.

## Validation results

The three backend fixtures pass near/middle/far selection, return movement, both fade boundaries, two independently selected placements in one tile, unchanged resident owners, and zero placement-record upload after camera movement. Shadow-on/off image comparisons darken 647 pixels on DX11 and 653 pixels on DX12 and Vulkan. A separate early shadow-only preparation fix preserves authored two-sided caster policy for the new independent tree bands, so back-facing cards contribute to the atlas.

The actual oak scene passes entrance/overlook capture, forced directional-shadow comparison, shader-error checks and terrain underside isolation on all three backends. Authored light/material shadow settings are retained; the diagnostic comparison explicitly enables them. Captures and logs are in `.local/tree-lod-oak-validation`. The Release editor builds with the feature. The shared culling shader explicitly preserves the existing grass tile-origin LOD path while new tree bands use per-placement bounds.

## Existing regression outside the tree path

`ShadowSparseSlotsDX11` passes GPU/fallback compaction, five-distance grass LOD/fade equivalence, grass wind, zero unchanged uploads and GPU shadow-receiver equivalence, then fails its generic shadow-distance monotonic-coverage assertion. The same assertion fails in the pre-feature Release DX11 executable with full=0 and partial=0, matching the final scoped build. This is an existing generic grass shadow-distance issue, not a new tree LOD failure. Raw baseline proof is in `.local/tree-lod-existing-baseline.log`; final scoped results are in `.local/tree-lod-scoped-results.log`. The tree-specific sidedness fix is scoped to independent prefab bands. All three final `TreeLod_*` tests pass, including visible shadows and independent same-tile levels with zero placement uploads.
