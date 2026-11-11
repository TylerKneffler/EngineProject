# Engine TODO


## Large oak meadow performance

Baseline: `.local/performance-logs/performance-20261110-201358-276.jsonl` (Debug editor, DX12, 286 scene-view frames, not playing). Median editor update interval 44.6 ms; median 14.3 million submitted triangles; grass instances and grass tiles built were zero.

- [ ] **Remove streamed oak prefab preparation hitches.** Profile `TerrainObjectLayer::PreparePrefabInstances`, including prefab parsing, geometry preparation, and graphics uploads. Cache reusable prefab geometry and materials across cells, move thread-safe preparation off the scene thread, and budget remaining main-thread uploads. Preserve editable placements and safe cell unload/reload. Three baseline frames spent 5.1?6.2 seconds in prefab preparation; verify cold and warm cell crossings separately.
- [ ] **Reduce steady tree rendering cost.** Measure oak geometry and leaf pixel cost by LOD on a repeatable camera route. Test earlier LOD transitions and lower-cost distant foliage while checking appearance and transitions. The baseline peaked at 20.3 million submitted triangles and had a median 6.8 ms GPU leaf pass; compare median and tail frame times against matched captures.
- [ ] **Reduce tree shadow submission and rendering cost.** Profile tree shadow batches and test shorter shadow distances and dedicated low-cost shadow proxies. Check shadow quality and CPU and GPU timings before adopting changes; do not assume distance-selected LODs are cheaper. The baseline median shadow submission was 7.7 ms CPU, with a 27.7 ms 95th percentile.
- [ ] **Investigate missing grass.** Determine why active grass fields produced no tiles or instances in the baseline. Verify editor and play-mode visibility, cell streaming, grounding, and behavior after terrain edits; include grass in subsequent performance measurements.
- [ ] **Reprofile in a Release build.** Capture the same scene and camera route in Debug and Release, in both editor and play mode. Record median and 95th-percentile frame times, prefab-preparation hitches, tree and shadow costs, grass counts, and visual quality before and after each change.
