# Large-scene rendering overhaul

Goal: 120 FPS at native 1920x1080 while retaining usable map editing, foliage
density, view distance, and realistic material appearance.

## Evidence and first structural change

The expanded meadow already uses indexed shared geometry, persistent instance
buffers, GPU instance culling, LODs, and streamed cells. Those systems do not
eliminate repeated CPU scene-object preparation or shading of overlapping leaf
layers.

The 600-frame Release DX12 capture on the RTX 4090 at 1080p recorded 38.7 million
leaf pixel-shader invocations per frame, approximately 18.7 per output pixel.
That includes coverage work and helper invocations; it is not a count of unique
visible leaves. Leaf shading took 7.17 ms median. Actual leaf geometry measured
1.35 million vertex-shader invocations and 1.13 million primitives, substantially
below the CPU candidate-triangle counter.

The new rendering path prepares visibility once, stores view-local instance draw
records referencing the persistent GPU buffers, and reuses those records for:

1. Coverage depth, using the same transforms, alpha masks, LOD fade, and clipping.
2. Full material lighting with depth-equal testing and depth writes disabled.

The lighting shader requests early depth testing so hidden layers do not run
the expensive material path. This is safe only with matching coverage and no
depth writes; see [Microsoft's earlydepthstencil documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil).
Custom shaders, parallax, skinned/morphed geometry, grass-budget geometry,
selection overlays, wireframe, and portal views keep the existing path.

This establishes reusable draw records across passes. It is not yet a persistent
scene-level draw database or a virtualized mesh system.

## Measurements and limits

Release DX12, RTX 4090, expanded meadow, native 1920x1080, uncapped,
1,200 warmup frames and 600 measured frames per run. No editor or build ran
concurrently with the captures. The control uses the same executable with
`ENGINE_FOLIAGE_DEPTH_PREPASS=0`; the new path is enabled by default.

| Median measurement | Disabled control | Enabled repeat |
| --- | ---: | ---: |
| Opaque GPU time, including coverage depth | 9.29 ms | 5.61 ms |
| Full leaf lighting | 7.43 ms | 1.47 ms |
| Additional coverage depth | 0 ms | 2.27 ms |
| Full leaf pixel-shader invocations | 38.76 million | 1.01 million |
| CPU scene preparation | 3.52 ms | 3.64 ms |
| CPU command recording | 7.94 ms | 8.38 ms |
| Complete frame interval | 13.98 ms | 14.85 ms |
| Draw submissions | 177 | 279 |

The earlier enabled run measured 5.99 ms opaque GPU and 14.00 ms complete
frame interval. The GPU reduction is substantial, but these runs do **not**
show an overall FPS improvement. CPU work still exceeds the 8.33 ms budget;
the extra geometry pass also adds CPU submissions and vertex/coverage work.
Scenes with little overlap may benefit from disabling this path.

Both control and enabled repeat retained 18,432 resident authored placements,
eight streamed cells plus the central terrain, and identical GPU LOD selection
counts (347 / 601 / 4,389 mesh-part instances). No density, LOD-distance, or
resolution reduction was used. The top 520 image rows contain 998,400 canopy
pixels: 1,069 pixels differ, with mean absolute channel error 0.00102 on a
0–255 scale and five pixels with any channel difference above 20. The images
are not bit-identical; inspected canopy crops show no broad foliage loss.
Animated foreground grass is excluded from this static comparison.

Local evidence is summarized in `.local/foliage-overdraw-validation.json`.
The same-executable control is in `.local/foliage-depth-control`, the enabled
repeat in `.local/foliage-depth-final-dx12`, and the earlier enabled run in
`.local/foliage-depth-after`.

## Validation

- Release capture executable and Debug editor builds pass, including the new
  Vulkan SPIR-V entry point.
- Release `IndexedInstanceGeometry`, `EditorLodPreview`, `DrawBatchPreparation`,
  and `InstanceTilePreparation` regressions pass. Debug draw-record replay and
  editor LOD regressions pass. Replay checks cover indexed/nonindexed and
  direct/indirect draws using identical resources and ranges across both passes.
- DX11 and Vulkan each completed 240 valid native-1080p frames with 102 coverage
  draws and the same 347 / 601 / 4,389 GPU LOD selection counts as DX12. Their
  enabled frame medians were 16.88 and 13.94 ms respectively; these are functional
  checks, not same-backend before/after speedup claims.
- Backend validation exposed a DX11 visibility telemetry defect: copying half
  a structured record returned invalid counts. Copying the complete 48-byte
  record restored matching counts. This changes diagnostics, not culling policy.
- A 300-frame DX12 movement capture crossed the expanded meadow's cell boundary
  from z=170 to z=187.06 (17.11 m displacement), with terrain/streaming checks
  passing, 102 coverage draws, and changing GPU visibility. Median frame time
  was 17.54 ms, p95 21.37 ms; this verifies movement and residency, not the FPS
  target. Evidence: `.local/foliage-depth-moving`.
- Full-image previews retain the dense canopy across all three backends.
  DX11 lighting differs from DX12/Vulkan; no cross-backend pixel-parity claim
  is made. Interactive Scene toolbar, selection, and painting checks remain.

## Remaining architecture work

- Separate generated foliage render data from the general object/component
  hierarchy: stable handles for shared material/mesh records and instance blocks,
  with additions, edits, and removals published as deltas. Preserve placement IDs
  for selection and painting, and retain resources until frame completion.
- Retain draw classification and material records across frames with explicit
  invalidation for editing, streaming, hot reload, and multiple editor cameras.
  Avoid reconstructing thousands of unchanged CPU records for each view.
- Extend GPU visibility to spatial clusters and depth-pyramid occlusion, with
  conservative bounds for wind, camera movement, and newly streamed cells.
- Add geometric-error LOD/mesh-cluster data for large opaque meshes such as rocks
  and trunks. Foliage also needs coverage-aware simplification and overdraw
  control; triangle reduction alone does not solve its fragment workload.

Each stage needs same-scene A/B measurements, stationary and moving views,
streaming/edit invalidation checks, and image comparisons. The 8.33 ms target
remains open until full frame time, including CPU preparation, meets it.

### Next implementation boundary: persistent scene draw data

Start at `SceneFramePreparation.cpp`, `SceneInstanceBatching.cpp`, and
`SceneRendering.cpp`. Existing component lookup, static-item, placement-pool,
and batch caches should be retained and extended rather than duplicated.
The missing boundary is a stable render representation shared by the camera,
shadow, and editor views, instead of rebuilding equivalent per-object inputs.

| Persistent record | Changes that invalidate it |
| --- | --- |
| Mesh section and LOD chain | Geometry publication, import/hot reload, LOD edits |
| Material and pipeline classification | Material edits, textures/shaders becoming ready, render settings |
| Placement block and conservative bounds | Paint, erase, transform, wind policy, streamed-cell changes |
| Draw group membership | Any changed mesh/material/classification or block addition/removal |
| View visibility and indirect arguments | Camera, projection, shadow cascade, or relevant scene change |

Authored objects and placement IDs remain the editor's source of truth. A
render handle must contain a generation so deletion and slot reuse cannot
select or draw a different object. Publish immutable frame snapshots, retire
old GPU allocations behind frame fences, and keep independent visibility
outputs for Scene, Game, and shadow views. Current public material fields and
script edits require a reliable revision/dirty notification boundary before
removing validation scans; caching raw pointers alone would be incorrect.

Acceptance: an unchanged camera frame must reuse group membership and
material records without reconstructing thousands of inputs. Painting one
cell should update that cell's records, while moving the camera changes view
data and GPU selection. Verify paint/erase, transform/material changes,
stream-in/out, hot reload, undo/redo, Play/Stop, and simultaneous editor views
before treating the persistent path as complete. Measure total frame time as
well as preparation/recording; a cheaper stage alone is insufficient.
