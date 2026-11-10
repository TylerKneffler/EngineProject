# Scene loading follow-up

- [ ] Keep the previous scene as a true staging fallback until the replacement is complete. Current incremental restore modifies the active scene and rolls it back only on failure.
- [x] Restore one object node per editor frame with a progress count; disable editor controls during the restore phase.
- [ ] Split large component payloads and prefab instantiation into time-budgeted work units; each node's component data and prefab reference are still processed as one unit.
- [x] Allow canceling the background read or an in-progress incremental restore, restoring the saved scene on restore cancellation.
- [ ] Keep graphics and other owner-thread-only resource creation on the editor thread; move only parsing and CPU-only asset preparation to workers.
- [x] Profile Oak Meadow's `Oak forest` terrain layer separately.
- [ ] Turn `TerrainObjectLayer::PreparePrefabInstances` into a resumable build that creates a bounded number of prototype/mesh batches per frame, then swaps the completed generated objects into the active scene.
- [ ] Make DX12 static geometry uploads nonblocking or batch submissions; the Oak profile shows about 275 ms waiting on upload fences across 85 mesh buffers.
- [ ] Reduce CPU-side Oak batch setup. One captured preparation took about 4.9 s total, including 3.0 s in prototype definition, 1.18 s in shared source geometry, and 0.53 s constructing mesh components (profile stages can be nested, so these values overlap).
- [ ] Measure startup and interactive scene loads before and after each change, including peak frame time and total load time. (Interactive completion time is now emitted to the debugger output; frame-time capture remains.)

## Current constraint

`EditorState` reads and parses the scene file on a worker, then incrementally restores authored object nodes on the editor thread. The current scene is cleared at restore start and held as a rollback snapshot; the replacement graph is published after all nodes are restored. A separate staging `Scene` would be needed to keep the old graph live until commit. Oak's remaining long frame occurs later, when the renderer synchronously constructs terrain prefab batches and creates/waits for GPU buffers. The profiler identifies that work, but it is not yet spread across frames.

The loading indicator is a compact, non-modal banner so the editor remains visible and usable during the background file read and shows top-level object progress during restore. A heavy component or prefab instantiation can still stall an individual frame.
