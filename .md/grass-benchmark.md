# Grass test scene benchmark (2026-10-30)

Scene: `Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene` on the local
Windows Debug build. Both cases use the same 648 placements, 18 m layer view
distance, 8 m shadow distance, and 16 visible 4 m tiles. The full case forces
LOD 0 through 18 m; the layer LOD case switches to LOD 1 at 6 m and LOD 2 at
10 m. LOD meshes keep the same patch placement and reduce blade counts from
54 to 30 to 14 (short) and 48 to 26 to 12 (tall).

| Measurement | Full detail | Layer LOD |
| --- | ---: | ---: |
| Runtime grass vertices | 1,029,960 | 615,420 |
| First 60-frame headless run, including load | 37.9 s | 26.4 s |
| Reverse-order warm 60-frame headless run | 25.3 s | 25.1 s |
| First four-frame DX11 PNG export, 960x540 | 38.7 s | 35.4 s |
| Reverse-order four-frame DX11 PNG export | 35.0 s | 34.8 s |

The layer LOD cuts tile geometry by **40.2%**. The reverse-order runs show no
meaningful wall-time gain in this small scene. The first-run time difference
was largely scene load and file-cache variation. Export times include scene
loading, simulation, readback, and PNG encoding; they are not GPU frame times.
The visual comparison at frame 15 is in `.local/grass-benchmark/full_render`
and `.local/grass-benchmark/layer_lod_render`.
With the layer view distance reduced to 6 m in a separate smoke scene, visible
grass tiles fell from 16 to 10, confirming tile unload uses the layer value.

Reproduce with `python Scripts/benchmark_grass_scene.py --frames 60
--render-frames 4`. It writes per-case scene files, headless state logs,
rendered frames, and `report.json` to `.local/grass-benchmark/`.
## Resident LOD performance update (2026-10-31)

The grass performance path now indexes placements only after edits, generates
new tiles incrementally within a configurable CPU budget, and caches all three
LOD geometries as resident GPU buffers. Switching a warmed LOD does not rebuild
vertices or upload a mesh. GPU publication is limited to one completed LOD per
refresh; an individual driver allocation is atomic and can exceed the budget.
Generated tiles and obsolete preview objects are excluded from authored saves
and Play/Stop snapshots. Scripts editing placements in place must call the
layer's `MarkConfigurationDirty()` after the edit. `useEditorCamera` explicitly
selects the editor camera for edit previews; gameplay uses `viewerObjectName`.

DX11 uploads shadow constants once per pass instead of mapping per draw. Grass
tile bounds also participate in cascade culling. Performance recordings include
grass indexing, generation, upload, cached LOD switches, pending tiles, resident
LOD counts and GPU bytes, plus shadow binding/draw details and constant-map counts.

Hardware audit: RTX 4090, Debug build, 1848 x 1069, 240 moving-camera frames
after warming. This is the game renderer with a hidden window, without editor
UI or physics, so it is not a direct editor-FPS comparison.

| Measurement | Result |
| --- | ---: |
| CPU frame, including render submission and presentation | 3.03 ms average |
| CPU grass update | 0.045 ms average |
| CPU shadow pass | 1.50 ms average |
| Cached LOD switches | 16 |
| New LOD uploads during measured movement | 0 |
| Placement rescans during measured movement | 0 |
| Cached grass GPU geometry (48 LODs across 16 tiles) | 469.7 MiB |
| Authored binary scene snapshot | 56,149 bytes / 29.2 ms |

Caching trades memory for stable LOD switches. Shared mesh instancing remains
a separate rendering TODO and should replace expanded tile geometry for larger
foliage populations. Wind and actor bending remain vertex-shader features;
these changes do not implement either animation.

Reproduce with `build/Debug/Debug/GrassPerformanceTests.exe --render-audit`.
The audit writes `.local/performance-logs/grass-render-audit.jsonl`.
`ctest --test-dir build/Debug -C Debug -R "^(GrassPerformance|PerformanceRecording)$"`
checks edit invalidation, incremental preparation, resident LOD switching,
legacy preview cleanup, snapshot exclusion, DX11 constant data/ranges/fallback,
and GPU buffer preservation during geometry exchange.

### Mesh-owned runtime LODs

Resident LOD storage, active-level state, distance hysteresis, buffer switching,
cache invalidation, and LOD memory accounting now live in `Mesh`. Grass keeps
its nearby-tile selection and deferred generation queue; ordinary meshes gain
no per-frame update or scene-wide LOD scan. Existing static geometry is retained
as LOD 0 when an object receives additional runtime levels. Geometry edits clear
cached alternatives. Runtime LOD caches are not serialized as authored assets.

The same hardware audit after this refactor measured 3.045 ms average CPU frame
time versus 3.031 ms before, and 0.051 ms grass update versus 0.045 ms before.
Both captures measured 240 moving frames after warming, with zero placement
rescans and zero new LOD uploads. These small differences do not establish a
meaningful performance change; ownership and reuse are the benefit of this
refactor. Captures are `.local/performance-logs/grass-render-audit-before-mesh-lod.jsonl`
and `.local/performance-logs/grass-render-audit.jsonl`.
### Expanded terrain stress layout

`grass_flat_test.scene` now extends ten 16 m terrain cells from the original
cell in both X and Z: 21 x 21 cells, 336 x 336 m, from -168 to +168 m on
both axes. The original cell remains editable at the center; 440 surrounding
instances use `Assets/Prefabs/Terrain/GrassStressCell.prefab`. Each cell carries
648 authored grass placements, totaling 285,768. Player, lighting, grass density,
LOD distances, and materials are retained; the camera far clip is 352 m.

Distant fields return before source loading, placement indexing, or generation.
Grass remains restricted to the existing 18 m viewer radius. The engine loader
validated all 441 terrain cells and grass fields, their bounds, and the placement
count. `GrassPerformanceTests.exe --scene-check` repeats this validation.

`python Scripts/expand_grass_test_scene.py --radius 10` reapplies the layout
without duplicating existing grid instances. `generate_grass_test_scene.py`
now regenerates the same expanded grid. The historical timing comparisons above
predate this expansion; new stress measurements must use the expanded scene.

### Grass fades and flat-terrain motion (2026-10-31)

Grass appearance fades over `fadeDuration` (default 0.35 s), with a 2 m
`distanceFadeWidth` at the viewer-radius boundary. Resident LOD changes retain
both meshes briefly and use complementary world-anchored dither coverage in the
camera and shadow pixel shaders. Opaque depth writes are retained. Mesh owns
transition state; grass advances it and sets tile visibility. Occlusion queries
are bypassed during fades and their cached results are invalidated at fade
boundaries so an initially invisible tile does not remain culled.

Grass species triangle streams are shared through the engine resource cache,
keyed by resolved source path and file modification time. Entering a new terrain
cell reuses the six source meshes instead of parsing each file again. Tile LOD
geometry is still prepared incrementally and remains separately resident.

The 18:03 capture confirms terrain physics setup at 0.78 ms average with 1?4
active terrain colliders, but grass work still averaged 15.2 ms and peaked at
146 ms before these changes. That capture does not measure player height.
A deterministic walking-capsule test reproduced a physical 71 mm height range
and a 44 mm worst vertical frame step on flat terrain. Correcting flat terrain
triangle contact normals reduced the range to 0.899435?0.900000 m and the worst
step to 0.565 mm. Non-planar terrain and ordinary mesh contacts retain their
existing behavior.

The hidden DX11 fade check measured 0, 665, 1373, 2091, and 2738 visible pixels
at 0/25/50/75/100% coverage. Mesh transition tests verify resident buffer reuse,
complementary outgoing/incoming coverage, and completion without an upload.
The grass, terrain collision, and performance recording regression checks pass.

### GPU grass instancing (2026-10-31)

The grass rendering path now replaces the expanded tile geometry described above
with shared source vertex buffers and compact instance transforms. Each placement
uses three float4 affine rows (48 bytes). Short/tall species have separate compact
buffer slices, reused by all three LODs; sources are shared across fields on the
same graphics device. Camera, shadow, and outline shaders apply instance transforms.
Tile AABBs are computed from eight source bounding corners per placement rather
than scanning and copying every blade vertex. LOD selection and crossfades reuse
existing source and placement buffers.

Preparation/upload time consumes a common scene budget (default 2 ms), rather
than granting every field another 2 ms. This is a soft budget: an atomic graphics
allocation may overrun it. Nearby tiles remain cached for two tile widths beyond
the visibility radius; farther tiles are released. GPU memory recording deduplicates
shared sources and placement buffers. `grass_instances` counts placements in
enabled tiles and `grass_instance_upload_bytes` records new placement uploads.
`grass_tiles_built` now counts prepared instance tiles, not expanded LOD meshes.

The regression fixture prepares two tiles with six resident LODs in two placement
preparations. One hundred moving updates produce no new builds/uploads/scans.
DX11, DX12, and Vulkan readback tests match two instanced species against reference
geometry with zero differing color channels, including nonzero source vertex
ranges and placement slices. Progressive dither fades pass on all three renderers.
The hardware audit now updates all 441 fields; its output is
`.local/performance-logs/grass-instancing-render-audit.jsonl`. It excludes the editor
UI, full gameplay update, and physics, so its timings are not editor FPS.

The completed 441-field DX11 audit measured 240 moving frames at 1848 x 1069:
1.563 ms average grass update, 26.540 ms total helper frame time, and 4.984 ms
CPU shadow submission. Shared grass GPU buffers occupied approximately 1.6 MB.
Movement prepared 16 newly visible instance tiles and made 65 cached LOD switches.
Two newly visited cells indexed 648 authored placements each; the final 60 frames
performed no placement indexing. CPU render submission remains a separate limit
on total frame rate. The editor build, three targeted regression tests, and
DX11/DX12/Vulkan instancing and fade readback checks passed.
