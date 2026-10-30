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
