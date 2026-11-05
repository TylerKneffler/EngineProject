# Tree geometry and memory audit (2026-11-05)

## Ownership

`TerrainPrefabInstances.cpp` groups placements by exact prefab path (including visual LOD), then by a 64 m X/Z tile, then by mesh part. The actual oak scene has 2,048 placements, 4,096 bark/leaf part instances, 100 source meshes and 1,316 derived batch objects. The retained-definition baseline loads 51 prefab definitions (17 varieties times three LODs): placements use 50 visual definitions, and one additional LOD0 base is loaded for collider policy. Grass uses one OakMeadowGrass prefab with generated runtime tiles, not a prefab per tile. A batch object is not necessarily a submitted draw: visibility, passes and renderer grouping affect draws. There is no current dynamic tree LOD selection.

CPU render sources are shared across tiles within a layer, but constructed separately for other layers. Indexed source geometry is expanded into a triangle stream. Native vertex buffers are shared by `SharedMeshBuffers.h` using provider, usage, size, stride, content hash and full byte comparison. Distinct species/LOD content must remain distinct. This cache retains a full CPU byte copy for collision-safe comparison, in addition to each source mesh's CPU vector. Weak cache entries do not keep buffers alive after their final owner.

Each batch owns a Material component copied from its source; shader/material values are not one shared component. The measured forest has 1,316 material components referencing only six unique Texture objects. Texture identity is cached through `Texture::Acquire`; this audit does not claim six native texture allocations because no texture residency/render warm-up occurs here. Instance transforms are per part/tile, so bark and leaf parts retain separate placement arrays.

## Change

Prefab definition scenes are now local to preparation, rather than retained in `PrefabInstances`. All geometry, transforms, material values and collider components needed by generated objects are copied before definitions expire. This removes the duplicate loaded CPU mesh population after preparation. It preserves the expanded render sources, content-comparison copies and derived batches. Saved-prefab collider propagation and serialization regression pass.

## Device-backed measurements

Release configuration, actual oak scene, hidden 64 x 64 device window, no frame rendering or texture warm-up. DX11/DX12 use build/Release; Vulkan uses the Vulkan-enabled Release configuration under build/Debug. Each row is one cold process; elapsed timings include preparation, audit enumeration and snapshot serialization. They are not rendered frame timings or a repeated performance benchmark.

| Backend | Working set before | Working set after | Cold prepare/audit before | Cold prepare/audit after |
| --- | ---: | ---: | ---: | ---: |
| DX11 | 1,187,876,864 B | 944,762,880 B | 4,614.76 ms | 3,848.72 ms |
| DX12 | 1,228,750,848 B | 987,312,128 B | 3,408.09 ms | 3,621.19 ms |

Both backends retain exactly 100 unique non-null native vertex handles for 1,316 batches, 235,920,960 B of native vertex payload and 235,920,960 B of source CPU vector capacity. Upload shadows report zero. Instance-transform capacity is 210,048 B. Snapshot size is 1,253,104 B. Payload bytes do not include driver allocation granularity, heaps, residency overhead, other renderer resources or process memory. Summing `Mesh::GetGpuBufferMemoryBytes()` over batches would count shared geometry repeatedly; native handles are deduplicated here.

The working-set reductions are 243,113,984 B (231.9 MiB) for DX11 and 241,438,720 B (230.3 MiB) for DX12. Peak reductions are much smaller because original and expanded data still coexist during preparation: 1,190,248,448 to 1,161,293,824 B on DX11; 1,230,835,712 to 1,204,781,056 B on DX12.

Vulkan after-change measurement: 1,059,299,328 B working set, 1,277,706,240 B process peak and 3,504.24 ms cold prepare/audit, with the same 100 native buffers and byte counts. No Vulkan before-change measurement was captured. The cross-layer probe passed on DX11, DX12 and Vulkan (four new part/tile batches each). A separate second-layer probe places the same prefab in two different tiles and asserts that every new batch reuses a previously observed native vertex handle.

## Historical 12.4 GiB peak

The maximum recorded working set is 13,348,921,344 B (12.432 GiB), at 2026-11-05 11:48:44.327 Eastern, in `.local/performance-logs/performance-20261105-114838-156.jsonl`. The nearest frame-details record, 90 ms earlier, has playing=true, frame_ms=210.648 and render_batching_ms=21.1008. The trace has 260 frame-details records, 219 in Play, and an overall median frame_ms of 230.7075. These are historical editor timings, not a matched baseline for this change.

Play captures serialized snapshot bytes from the history baseline or a fresh capture and parses them asynchronously, retaining both bytes and a parsed tree during Play (plus the worker's temporary byte capture). Runtime-only derived trees are excluded from serialization. The current measured authored snapshot is about 1.20 MiB, so current tree snapshot bytes alone cannot explain a 12.432 GiB working set. Original mesh copies, expanded derived geometry, cache comparison bytes, textures, terrain, history and Play restoration can overlap. The old trace contains no allocation-stack/owner breakdown; it establishes correlation with Play, not attribution or a leak. GPU usage in that sample was 433,504,256 B with a different scope from process working set.

## Reproduction and limits

Build target TreeAllocationAudit and run TreeAllocationAudit.exe with dx11, dx12 or vulkan (requires a Vulkan-enabled build). CTest names are TreeAllocation_dx11, TreeAllocation_dx12 and TreeAllocation_vulkan. The same audit was first run through the existing local OakBackendCapture harness. Run TerrainPrefabAndOakTests.exe shared-prefabs for placement, bounds, source sharing, snapshot exclusion and saved collider edit regression. Raw results and build logs are under `.local/tree-audit-*`.

Native buffer identity and payload are validated, not physical committed/resident GPU allocation totals. This run does not validate instance-buffer uploads or rendered frame timings. Matched DX12 editor Edit / Play / Stop captures are documented below. Rendered GPU timing comparisons, instance-buffer allocation validation and native committed/resident allocation totals remain open; the historical peak must not be reported as fixed by this retention reduction.

## Matched editor Edit / Play / Stop captures

The DX12 Release editor was run four times at a fixed 1920 x 1080 Game viewport: after-a, before-a, before-b, after-b. Each run includes 30 initial Edit frames, 20 Play warm-up frames, 60 measured Play frames, and 20 restored Edit frames after Stop. The before executable temporarily retained the definition scenes; final source and executable release them. The same instrumentation and seeded layout were used throughout. These are stationary entrance-camera editor captures, not GPU-isolated benchmarks.

| Run | Edit working set median (MiB) | Play working set median (MiB) | Stopped working set median (MiB) | Process peak (MiB) | Play frame median (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| before-a | 1912.5 | 1799.2 | 1820.2 | 2159.5 | 237.97 |
| before-b | 1913.5 | 1793.4 | 1839.7 | 2160.3 | 236.84 |
| after-a | 1684.1 | 1560.3 | 1606.7 | 1987.1 | 232.73 |
| after-b | 1713.2 | 1579.4 | 1612.6 | 2009.7 | 251.37 |

Measured Play private-byte medians were 2,185.5 / 2,180.3 MiB before and 1,947.7 / 1,946.0 MiB after. That is a repeatable roughly 234-238 MiB reduction in private bytes. Working-set reduction is approximately 214-239 MiB across these runs. Frame times overlap and vary materially; no consistent frame-rate improvement is established. All runs reported 103 median main-pass physical draws. Physics medians ranged from 0.055 to 0.200 ms, well below the historical approximately 30 ms figure, but these stationary-camera tests do not validate moving-player collider activation.

Serialized Play-snapshot capacity is 581,919 B and serialized scene-history capacity 702,732 B. Snapshot clear retains its string capacity after Stop; these counters deliberately report capacity, not string length. Parsed snapshot nodes, in-flight serialization/parse copies, other editor documents and allocator overhead are not included. Process peaks were 1.94-2.11 GiB; the historical 12.432 GiB peak was not reproduced.

The allocation audit now calculates the hypothetical cost of giving every placement its own expanded CPU geometry: 5,490,717,408 B (5.11 GiB) for the current forest. This is not actual retained memory. Two overlapping such populations would be 10.23 GiB before terrain, textures, buffer-cache comparison bytes and editor state. It demonstrates how per-tree geometry copies could reach the historical magnitude, without proving that the old executable had those exact owners. Geometry shared today occupies 235,920,960 B (225 MiB) of source CPU capacity.

Reproduce with `python Scripts/profile_tree_memory.py .local/performance-logs/tree-memory-run.jsonl --frames 60 --warmup 20 --stop-frames 20`. The runner verifies completion, measured phase counts and viewport dimensions, writes a separate layout, and emits a summary JSON. The new opt-in `ENGINE_EDITOR_AUTO_PROFILE_STOP_FRAMES` extends profiling through restoration. Process working set, peak working set and private bytes are captured in frame records; serialized Play-snapshot and scene-history capacities are also included in ordinary Performance recordings through the shared frame serializer. All four captures completed with zero dropped or failed writer records.
