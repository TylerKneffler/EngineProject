# Engine TODO

Last updated: 2026-10-28.

Status convention:

- `[x]` Feature implemented (record failed checks separately)
- `[ ]` Implementation or follow-up work remains

## Next priorities

### Distant terrain and large view distances

- [ ] Replace the fixed resident-cell square plus two far-tile levels with a single distance-driven terrain LOD policy in `TerrainManager`: use multiple tile resolutions (for example 1/2/4/8/16 sample strides) selected by projected screen error, stream progressively coarser tiles outward, and keep full terrain objects/colliders only in the interactive radius. Reuse one request queue and upload/memory budgets; keep old geometry until replacements are ready.
  - [x] Add a quarter-resolution 128-cell far-tile level above the existing half-resolution 64-cell and detailed 32-cell tiles. Select all three levels recursively by projected error and near coverage, and retain the previous representation during transitions. The camera far plane bounds far-tile requests; tile stride is selected internally rather than exposed as a view-distance setting.
  - [x] Retire resident full cell objects outside the interactive radius only after a visible far tile covers them; keep them loaded during tile preparation.
  - [ ] Extend the hierarchy to larger maps and unify near/far request priority so added view distance does not multiply cell objects.
  - [ ] Expose the interactive radius, per-level screen-error thresholds, and maximum view distance as independent settings. Log visible coverage, tile count, draw count, per-level resident memory, and frame times for each distance band.
  - [ ] Prove gap-free coverage while moving and turning, then compare at equal camera clip distances against the current radius-based path before reducing the default resident-cell radius.
- [x] Limit far-tile rebuilds after the initial reveal using `TerrainManager.maxUploadsPerUpdate`; prioritize nearby tiles and continue deferred work on later updates. Keep existing tile geometry visible until a replacement is ready.
- [x] Add a focused scheduling regression test for the upload cap, deferred updates, and settled state.
- [x] Add a configurable soft CPU time budget for far-tile rebuilds after initial reveal; always process at least one pending tile.
- [x] Index far patches by tile once, avoiding a full patch scan during each tile rebuild.
- [x] Read the far-grid archive on a background task and apply it on the scene thread when ready.

- [x] Add a two-level hierarchy of far terrain tiles. Use 64-cell, half-resolution parent tiles outside the detailed radius and 32-cell tiles near the viewer or terrain patches; switch each group when replacement geometry is ready.
- [x] Cache seam-free tiles at both far LOD resolutions with a versioned mesh format. Key by source signature, appearance, dimensions, local heights, and patch heights; changed tile data causes a local cache miss while unchanged tiles remain reusable.
- [x] Apply saved terrain edits as far-grid patches on background load, reading only edited full-resolution cells; changed patch heights invalidate affected tile entries.
- [x] Refresh distant heights and paint during a live editor brush stroke: load far tiles for the edit preview, update changed cell patches and colors, and rebuild affected tiles on the next editor view update.
- [x] Persist both far-tile resolutions in a versioned cache keyed by terrain source, appearance, tile heights, and edit patches. Load seam-free coarse tiles directly from cached meshes; changed saved edits invalidate affected entries on the next play-mode load.
- [x] Select far-tile resolution by projected screen size and camera visibility, with a configurable error threshold and hysteresis to avoid rapid LOD switching.
- [x] Make near/far and parent/child transitions gap-free: retain the old tile until its replacement is ready, match or stitch shared edges, and avoid rebuilding an entire far tile when only a small region changes.
- [x] Move far-tile generation and cache reads off the render thread; prioritize visible and nearby tiles, and cap CPU work, GPU uploads, and memory per frame. Reuse the existing terrain streaming budgets where practical.
  - [x] Prepare seam-free, unpatched distant tiles and read/write their caches on a bounded background worker; prioritize visible and nearby tiles, cap far uploads per runtime frame, and account for prepared and resident far meshes in memory usage.
  - [x] Snapshot near seam surfaces and edit patches so their full tile builds also run on the worker; keep urgent gap repairs and small in-place patch updates responsive.
  - [x] Share one upload allowance between near terrain and far tiles during a runtime frame.
- [ ] Rebenchmark at increasing view distances against the current radius-based terrain path. Check steady frame time, traversal hitches, cache warm-up, visual quality, and memory before raising the supported radius.
  - [x] Correct the manual benchmark frame clock so its shared upload allowance resets each rendered frame; aim the startup camera at the measured viewpoint and add a center-of-map case. The 9 km center run now settles all 20 desired far tiles and reaches 8966 m.
  - 2026-10-28 Debug DX11, 1920x1080, center view, 270 measured frames: at a 3218 m camera clip, tiled terrain averaged 114.116 FPS (30.226 ms p95), 25 resident cells, 4 far tiles, 4 median draws, and 45.094/32.309 MiB peak terrain CPU/GPU memory. The matching radius-based cells-only control averaged 15.160 FPS (81.972 ms p95), 289 resident cells, 55 median draws, and 73.416/51.206 MiB terrain CPU/GPU memory. Reports: `.local/performance-logs/terrain-1080p-far-3218-radius-8-distant-center-legacy-oscillate-cold.json` and the matching `cells-only` file.
  - At 9000 m, the settled center view averaged 92.714 FPS cold and 103.185 FPS warm (31.229/29.531 ms p95), reached 8966 m, rendered 20 far tiles, and retained 25 cells. A full camera sweep averaged 65.302 FPS with 46.366 ms p95 and a 95.760 ms worst frame. The separate cold/warm process pair took 24.5/21.0 s wall time and reported 10/28 far-tile cache hits; OS file caching and run variance also affect these timings. Reports: `.local/performance-logs/terrain-1080p-far-9000-radius-8-distant-center-legacy-oscillate-{cold,warm}.json` and the `sweep-cold` report.
  - 2026-10-28 Release DX11 closer-start test, 1920x1080, 270 frames: reducing the interactive radius from 2 cells to 1 reduced steady resident cells from 25 to 9. At a 3218 m clip from the low viewpoint, average FPS rose from 188.708 to 196.856, frame p95 was 8.648/8.604 ms, median draws fell from 13 to 8, and median submitted triangles from 26,866 to 16,394. Both runs showed 3 stride-1 far tiles and no coarser tiles. At a 9000 m clip from the high center sweep, average FPS was 101.082/103.023 and p95 was 17.514/16.562 ms; both runs showed 16 stride-1 and 4 stride-2 far tiles, with no stride-4 tiles. Single runs are sensitive to variance; inspect close-range visual quality before changing the default radius. Reports: `.local/performance-logs/terrain-1080p-far-{3218,9000}-radius-8-distant-{low,center}*legacy*` (files with `-near-1` use the closer start).
  - [ ] Inspect rendered captures for seams and missing terrain during a full camera sweep; repeat controlled runs before setting a supported maximum view distance. The sweep's p95 remains well above 16.7 ms.
- [ ] Profile the terrain scene's render CPU path and near-terrain streaming spikes with far tiles enabled and disabled; target the measured hot path before further far-tile micro-optimizations.
  - 2026-10-28 Release DX11, 1920x1080, 9 km center sweep: 99.002 average FPS, 16.399 ms frame p95. Frame-end CPU p95 was 11.668 ms, of which `Present` was 11.660 ms; GPU terrain p95 was 10.877 ms of 11.883 ms total GPU. Manager and far-tile update p95 were small in the same profile. The CPU waits at presentation while GPU terrain work finishes; optimizing the present call itself is unlikely to help. A later closer-start benchmark overwrote this report path; use the preserved `-release-profile.json` report for a comparable profile, though its measurements differ slightly.
  - Release shadow experiment at 9 km: shadows on averaged 98.398 FPS, 17.389 ms p95; shadows off averaged 119.018 FPS, 14.713 ms p95. Shadow-only `Scene::Render` still cost 5.069 ms CPU p95 with shadows disabled, versus 5.890 ms with them enabled. GPU shadow work was only 1.263 ms p95 with shadows on. Unlit mode did not materially improve the terrain GPU time. These are separate single runs, so repeat before claiming a precise percentage.
  - [ ] Skip or cheaply exit the shadow-only scene traversal when no light casts shadows; profile its prepared-draw and cascade-culling CPU work when shadows are enabled.
  - [ ] Profile terrain GPU screen coverage, overdraw, and material cost at 3.2 km versus 9 km; the 9 km sweep spent about 10.9 ms p95 in the terrain GPU stage. Test lower resolution or a pixel-cost diagnostic before changing LOD geometry again.
  - [ ] Profile near-cell streaming spikes at moving cell boundaries with the complete far field active; the current benchmark crossing p95 is below the sweep p95 but should be checked in Release with correlated per-frame samples.
#### references

- [Distant Horizons project and source](https://gitlab.com/distant-horizons-team/distant-horizons) — overview of its simplified terrain rendering beyond Minecraft's normal view distance.
- [Distant Horizons Core source](https://gitlab.com/distant-horizons-team/distant-horizons-core) — implementation reference for LOD storage, selection, generation, and rendering.
- [Server owners guide](https://gitlab.com/distant-horizons-team/distant-horizons/-/wikis/1-user-guide/1-frequently-asked-questions/5-server-owners/Server-Owners) — LOD caching, pre-generation, and generating the required coarse level directly ("N-sized generation").
- [Render API documentation](https://distant-horizons-team.gitlab.io/distant-horizons/com/seibel/distanthorizons/api/interfaces/render/IDhApiRenderProxy.html) — baked color in render geometry and replacing cached render data after the new version loads.
