# Expanded oak meadow

Open `Engine/Core/Assets/Scenes/terrain/oak_meadow_expanded.scene`.

This is a new map derived from the current `oak_meadow.scene`, not the deleted large-oak scene. It uses `Assets/Prefabs/Terrain/OakMeadowStreamCell.prefab` and existing tree, grass, and material assets. No height archive, removed terrain data directory, or generated cache is required to load the map.

- Extent: 1,800 by 1,800 metres, centered on the original meadow (3.24 square kilometres).
- Layout: five by five 360-metre cells; each cell retains the source meadow's 2,048 oak placements and authored LOD chains. The full logical grid has 51,200 oaks, not all resident at once.
- Residency: a radius of one cell around the viewer, with one newly instantiated cell per update. The authored center remains available; streamed cells are reused and pooled. Near map edges the active neighborhood is clipped to the finite grid.
- Collision: terrain colliders activate around the viewer, with an 8-metre boundary margin. The original first-person camera/controller and sun are retained.
- Seams: the outer two terrain vertex rows are level, blending back to the original heights over 24 metres. Boundary material weights also match. Tree base heights follow the adjusted surface.
- Grass: the existing `GrassField` generates the original species near the viewer, using the source prefab's approximate average placement density (2.5011 patches per square metre), view settings, and LODs. Placement positions are regenerated; they are not an exact copy of the painted grass pattern. This avoids duplicating the 114 MB placement prefab for every cell.

The source meadow and its grass prefab are unchanged. The new map repeats the authored tree layout; it is a streaming/performance scene rather than a uniquely landscaped world. Tree view and shadow distances remain unchanged. The 120 FPS at 1080p target is an acceptance goal, not a performance claim.

Regenerate the cell and scene from their current source assets:

```powershell
python tools/build_large_oak_scene.py
```

The generator checks terrain height continuity at every tile edge. Generated scene data is stored directly in the prefab. Re-running the generator replaces edits to the two generated files.

Validate with the new standalone benchmark:

```powershell
cmake --build --preset debug --target GameBackendCapture --parallel 4
.\build\Debug\Debug\GameBackendCapture.exe --backend dx12 --scene Engine/Core/Assets/Scenes/terrain/oak_meadow_expanded.scene --width 1920 --height 1080 --warmup 600 --frames 240 --max-fps 0 --require-terrain --require-streamed-cells --wait-for-residency 120 --out .local/expanded-oak-capture
```

Benchmark `terrain_cells_loaded` includes both native terrain cells and streamed prefab cells. `terrain_prefab_cells_loaded` and `terrain_prefab_cells_pooled` distinguish this scene's streaming path; the permanently authored center is counted in `terrain_components`, not in streamed-cell totals.

## Validation, November 12, 2026

Debug Editor and GameBackendCapture build successfully. DrawBatchPreparation and InstanceTilePreparation regressions pass.

The DX12 1920x1080 standalone capture completed 240 valid measured frames after 600 warmup frames, with nine terrain components, eight streamed cells, 18,432 resident authored oak placements, and a rendered image. Observed frame interval median/p95: **55.86/80.14 ms**. This is a validation capture, not evidence that the 8.33 ms target has been achieved or that all preparation had settled.

A temporary copy started the player at z=170 m and drove across the z=180 m cell boundary. All 300 measured frames were valid; the player moved 68.14 m and ended at z=237.94 m. Player-center clearance above the sampled terrain stayed between **0.895 and 0.904 m**, with eight streamed cells resident. The source scene's spawn was not changed by this test. Bullet emitted a static/static collision diagnostic during the run; the movement and clearance checks still passed.

Evidence: `.local/expanded-oak-capture/`, `.local/expanded-oak-seam-capture/`, and `.local/expanded-oak-validation.json`. Local capture files are not shipped with the assets.
