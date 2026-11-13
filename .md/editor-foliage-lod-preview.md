# Scene-view foliage LOD visibility

The Scene toolbar forced a single object LOD, but uploaded geometry records
still contained the runtime distance crossfade. For near-detail foliage, the
shader discarded that geometry beyond the first transition while the editor
excluded the replacement bands. Changing the toolbar LOD moved the visible
distance band instead of providing a usable preview.

The user trace `.local/performance-logs/performance-20261112-194914-332.jsonl`
contains 291 Scene-only frames: all have zero GPU instance batches and zero
LOD1/LOD2 draw counts (zero-based metrics). All 288 Game-only frames use GPU
instance batches and multiple LODs. This matches the fixed-LOD Scene path and
the reported UI LOD 1 showing only nearby trees. The trace alone cannot count
fragments discarded by the shader; the distance-fade path establishes the cause.

## Fix

- Fixed editor LOD previews clear geometry crossfades before object-record
  upload. The selected detail remains visible at every distance within the
  loaded scene and view frustum.
- Screen-based LOD shader flags are suppressed during a fixed preview, so they
  cannot reinterpret the cleared fade as zero coverage.
- The toolbar offers **Auto**, which remains the default as assets load.
  Fixed LOD 1/2/3 are optional inspection modes. Previously the toolbar replaced
  Auto with a fixed index, potentially locking LOD 1 before other assets arrived.
- Game rendering and shadow fades retain their existing policy. Asset values,
  density, view distance, and source-local culling bounds are unchanged.

`EditorLodPreviewTests` exercises coverage at 0–1,000 units for every authored
prefab band and runtime-generated LOD, reproduces the missing-near-LOD failure,
and verifies that Auto, Game, and shadow records retain their fades. The test
passes in Debug and Release. The updated Debug Editor and Release
GameBackendCapture build successfully. IndexedInstanceGeometry,
DrawBatchPreparation, and InstanceTilePreparation also pass in Release.

Interactive camera movement and toolbar switching should be checked in the
rebuilt editor; automated coverage checks do not substitute for that UI check.
