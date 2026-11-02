# Parallel work: persistent spatial draw slots

The scene spatial index now receives bounds in persistent draw-slot order.
Camera and shadow queries map frame draws back to those slots. Temporarily absent
draws retain their last bound, so LOD fades and visibility changes do not alter
the index membership. Multiple chart draws sharing a slot use a conservative
union; any unbounded draw makes the aggregate unbounded.

This removes one cause of full tree rebuilding. New draw slots and index quality
changes may still rebuild. Bounds preparation, material work, and graphics calls
retain their existing owners. Static render-item caching and full scene snapshots
remain separate work.

## Verification

- Debug Editor builds. `GrassPerformance` and `RenderSpatialIndex` CTests pass.
- Scene regression toggles a bounded mesh off and on and checks that neither
  transition rebuilds the spatial index. Existing grass render checks exercise
  camera and shadow visibility.
- The DX11 441-cell hardware route completed 240 measured frames each for
  resident and streaming modes. Resident frames averaged 0.082 ms in the index
  with 9 rebuilds; streaming frames averaged 0.081 ms with 7 rebuilds. The full
  spatial stage, including bounds preparation, averaged 0.491 and 0.453 ms.
  Captures: `.local/performance-logs/parallel-slot-{resident,streaming}.jsonl`.

The prior stage reported 44 resident and 193 streaming rebuilds on its runs.
These are separate captures with different machine conditions and residency, so
their frame time averages are not a controlled before/after comparison. The new
captures establish that the slot path runs and does not rebuild on every
streaming frame; they do not establish an FPS gain.
