# Command recording breakdown and directional light-grid reuse

Added the current CPU recording, moving-shadow, editor panel and shader-source
invalidation priorities to the TODO list, then started with recurring work in
view command preparation.

Recordings and the Performance Timings tab now separate compute recording,
scene snapshot/resource retention and view recording. The remaining packet and
queue bookkeeping is reported separately in recordings/evaluation. These scopes
are inside pipeline_record_ms and must not be added to that total. The baseline
showed 3.677 ms in views versus 0.032 ms retaining the scene snapshot.

Found unnecessary per-view Forward+ work: a directional-only light list was
rebuilt for every cluster and uploaded every frame, although the list consists
of the same ordered light indices in every cluster. Cache these grids by the
existing GPU frame-slot buffer pair and grid layout (tile dimensions, slice
count, per-cluster capacity and light count). Camera pose, light colors,
intensities and directions still update normally; they do not change these
directional cluster index lists.

Local-light frames erase the cached layout before overwriting a buffer. Buffer
replacement clears layout entries, and layout changes force rebuilding. Upload
buffers retain their existing frame-slot fence ownership and lifetimes. No
shader quality, cascade count, grass density or output resolution changed.
This is generic scene rendering, rather than a grass script optimization.

## Verification

Editor and GrassPerformanceTests Debug builds passed. The initial five selected
CTest suites passed in 48.44 seconds, covering performance evaluation, moving
shadow upload/cascade stability and DX11/DX12/Vulkan pixel regressions. The pixel
fixtures now check warm directional reuse, point-light invalidation, recovery
after disabling that point light and matching pixels after recovery. A stronger
follow-up also counts uploads across the capture to require an actual rebuild
after a local-light frame overwrites the grid. All three backend suites passed
again with that assertion, in 37.14 seconds.

Two sequential Debug DX11 grass_flat_test.scene captures used the same editor
layout and fixed 1920 x 1080 Game target, 60 gameplay warmup frames and 180
measured frames. User layout and asset-location files were saved in memory and
restored after each run.
Both captures retained 3422 grass instances, 45 candidate draws and 24 shadow
draws, with one rendered view.

| Measurement | Baseline | Directional grid cache |
|---|---:|---:|
| Average FPS | 130.21 | 154.30 |
| Average frame interval ms | 7.680 | 6.481 |
| Frame interval P95 ms | 9.540 | 7.377 |
| Frame interval P99 ms | 10.130 | 7.835 |
| Command recording ms | 3.792 | 3.041 |
| View recording ms | 3.677 | 2.948 |
| Render startup ms | 0.432 | 0.134 |

The cached light-grid check averaged 0.0031 ms, with one hit per measured frame
and zero cluster-list upload bytes. All 180 frames used exactly 1920 x 1080;
the recording subscopes sum to the total within serialization precision.
Earlier captures did not expose cluster preparation separately. Startup timings
support reduced recurring work, but these single sequential runs cannot assign
the entire FPS or recording-time difference to the cache. View recording still
dominates, so further cached bindings and immutable object/material preparation
remain open. Moving-route validation after the shadow fixes, editor panel work
and shader change-driven invalidation remain open too.

Artifacts: `build/Debug/recording-verification-20261103/` contains baseline and
cluster-cache JSONL captures, their evaluations, `ctest.log` and the follow-up
`ctest-invalidation.log`. Build logs include `build/Debug/cluster-cache-final-build.log`
and `build/Debug/cluster-cache-invalidation-build.log`.
