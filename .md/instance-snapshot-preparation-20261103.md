# Resident instance records and worker preparation

The existing renderer already kept foliage placements resident and used compute
for per-instance visibility, LOD filtering and indirect draw counts. The remaining
repeated work was registering sources and rebuilding affine/bounds/range metadata
inside every species, LOD and shadow-cascade submission.

Instance sources now register once per view and upload once before the batches
consume them. Camera-independent tile records are cached by stable object slot
and copied snapshot values: world transform, source bounds including resident
LODs, placement base, species ranges, LOD thresholds and shadow range/fade.
Submissions reuse those immutable records across LODs and cascades. Coarse CPU
tile rejection remains; per-placement visibility, LOD and draw counts remain on
the GPU. CPU fallback paths remain available for unsupported contexts.

Changed placement records are built from copied affine and identity arrays.
Changed tile records are built from copied value snapshots. Large batches use
the shared engine scheduler; each worker writes its own output records. Every
worker joins before the owner publishes cached records, merges placement dirty
ranges or writes GPU resources. Shared ownership preserves previous tile readers
through edits and cache retirement; GPU allocations and retirement remain with
the existing frame-slot fence owners. Small edits stay serial, and unchanged
records skip construction and scheduling entirely.

These are generic rendering helpers used by instanced meshes, rather than
grass-specific scripting. Ordinary material/object record assembly still owns
mutable material, probe and graphics-resource caches; isolating those inputs for
further worker expansion remains a separate TODO.

## Validation

Editor and GrassPerformanceTests Debug builds passed. Five CTest suites passed
in 56.37 seconds: GrassPerformance, PerformanceEvaluation, and the DX11, DX12,
Vulkan sparse-slot/shadow pixel regressions. New tests cover serial/parallel tile
equivalence, sparse slots and affine rows, unchanged reuse, transform/bounds/LOD/
range edits, slot retirement and retained-reader lifetimes. A 4096-placement
case compares serial and worker results, including 64-bit placement identities.
Existing tests retain the one-record/64-byte upload check and bounded streaming
allocation check. Unchanged placement sources now also assert zero CPU rebuilds.

Final performance runs used 1920 x 1080, grass_flat_test.scene, DX11 Debug. The
editor capture waited for loading and gameplay, then used 60 warmup frames and
180 measured frames. The moving helper used 240 measured route frames. Layout
and asset-location files were preserved in memory and restored after each run.

| Measurement | Stationary full editor | Moving rendering helper |
|---|---:|---:|
| Average FPS | 149.60 | 104.20 |
| Average interval / frame ms | 6.684 | 9.597 |
| P95 ms | 7.725 | 17.463 |
| P99 ms | 8.269 | 38.439 |
| Maximum ms | 8.663 | 48.624 |
| Tile snapshot/cache preparation ms | 0.178 | 0.281 |
| Tile records rebuilt over capture | 0 | 2 |
| Placement records rebuilt over capture | 0 | 83 |
| New placement upload bytes over capture | 0 | 5312 |
| Tile metadata upload bytes over capture | 0 | 2227392 |
| Edited existing placement upload bytes | 0 | 0 |
| CPU visible-ID upload / fallback bytes | 0 | 0 |

The stationary capture reused 38 tile records per frame, rebuilt no placement
or tile records, and uploaded no instance placement or tile metadata. Its GPU
work averaged 1.749 ms and command recording averaged 3.260 ms. The moving route
uploaded newly streamed placements, with no existing-placement edits caused by
camera movement. Small new sources stayed below worker thresholds; zero new
preparation worker passes in steady-state captures is expected. The larger
changed-snapshot regression cases explicitly exercise the worker paths.
Camera-driven coarse batch membership still updates tile metadata on the moving
route, totaling about 2.23 MB across 240 frames. Those metadata copies are
separate from resident placement data and GPU-generated visible IDs.

These captures are verification, not a controlled before/after speedup claim.
The previous stationary result was approximately 148.5 FPS. The moving helper
omits full editor/gameplay overhead and cannot be compared directly with editor
FPS. Moving P95/P99 and maximum times still exceed a sustained 100 FPS budget;
streaming and shadow spikes remain open. This work does not establish a fix for
the reported moving shadow flicker.

New diagnostics include render_instance_tile_prepare_ms, tile reuse/build counts,
tile parallel passes, instance_prepared_records and instance_preparation_worker_passes.
They are included in recordings/evaluation; the Performance view shows tile
preparation and reuse/build/parallel counts.

Artifacts are under `build/Debug/instance-snapshot-verification-20261103/`:
`ctest-final.log`, `editor-final-1080p.jsonl`, `editor-final-evaluation.json`,
`editor-final-evaluation.md`, `moving-final-on.jsonl`, `moving-final-on.log`,
and `moving-final-summary.json`. Final build log:
`build/Debug/instance-snapshot-final-build.log`.
