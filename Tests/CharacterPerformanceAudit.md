# Character interaction performance audit

Measured on 2026-10-24 with the Windows Debug build, using the bundled Mococo
PMX prefabs and Sad Cat Dance VMD. These are headless CPU measurements, not
rendered frame rates or interactive UI latency measurements.

| Operation | Before | After |
| --- | ---: | ---: |
| First low-detail Mococo instantiation | 3363 ms | 2018 ms |
| First prefab override query | 1367 ms | 0.004 ms |
| Scene start | 2995 ms | 122 ms |
| Disable followed by an explicit prefab diff | 1473 ms | 337 ms |
| Embedded VMD snapshot capture | 2493 ms (XML) | 907 ms (binary) |
| Embedded VMD snapshot restore | 2718 ms (XML) | 1883 ms (binary) |

The embedded VMD comparisons use an intermediate build with the other fixes
already present. The initial benchmark's headless restore called a scene API
that requires a graphics provider; the benchmark now calls the serializer
directly. There is no valid initial-build restore timing.

Additional final measurements: repeat instantiation 727 ms; VMD binding
1284 ms; applying the bound pack 80 ms. The full-detail prefab also passes:
first instantiation 2071 ms, start 121 ms, repeat instantiation 713 ms,
binding 1132 ms, pack application 63 ms. Timings vary with machine load.

## Changes

- Cache decoded animation assets by resolved path, modification time, and
  file size; cap the cache at eight entries and 64 MiB of source data.
  Return editable copies and invalidate on save. Avoid building an unused
  target rig when only rig-owned clips are present.
- Map animation channels in the existing output copy instead of copying
  every key twice. Move the temporary pack clip into the mapping input.
- Compare unchanged prefab subtrees before building patch paths. Mark a
  freshly captured prefab baseline as already free of overrides. Applying
  an override skips deserialization of unchanged components.
- Use cached override indicators in the hierarchy and inspector during
  runtime playback. Toggling an object no longer triggers a diff of the
  entire animated prefab from these UI paths. Loading a scene resets its
  runtime lifecycle flag.
- Blend morph weights in reusable CPU storage and publish each mesh's final
  result once. Repeated identical results retain their revision instead of
  resetting to rest and uploading each animation layer.
- Preserve authored VMD endpoints while omitting interior samples in constant
  translation/rotation intervals. Avoid evaluating constant-axis Bezier curves.
- Use lossless binary in-memory main-scene history/play snapshots and prefab
  patch caches. Saved scene files continue to use the existing public format.
  Reuse the current history baseline at play start and the restored snapshot
  at stop, avoiding extra complete captures. JSON numeric formatting uses
  `to_chars` instead of constructing a stream for every fractional value.

## Verification

Build targets: `CharacterPerformanceTests`, `PrefabComponentOverrideTests`,
`HumanoidRigTests`, and `Editor`.

`CharacterPerformanceTests` verifies independent clip-cache values, save and
deletion invalidation, selective component deserialization, named facial-layer
blending, stable morph revisions, runtime lifecycle reset, prefab loading,
animation/disabled updates, VMD binding/application, and exact serialized
animation equality after snapshot restore. It runs on both Mococo prefabs.
Prefab component override and humanoid mapping regressions also pass.

Cold loading, binding, and restoring large embedded motions remain synchronous.
They can still produce visible pauses in Debug; these changes do not establish
that all UI lag is eliminated. GPU rendering and interactive input latency need
an editor profiling session to measure separately.
