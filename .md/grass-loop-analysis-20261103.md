# Grass floor fix and loop timing - 2026-11-03

Source: `.local/performance-logs/performance-20261103-172347-156.jsonl`.

The recording contains three Play sessions and scene changes. The combined 72 FPS average mixes different workloads and does not describe the large landscape. The large `grass_flat_test.scene` session, 22:24:06.647-22:24:19.484 UTC, contains 701 Play samples: **54.61 FPS / 18.311 ms mean frame interval**, P95 **29.019 ms**, P99 **43.536 ms**. Most frames render one 1814 x 1069 view; the capture is an interactive session, not the fixed 1080p automated benchmark. The distant single-cell session renders Scene view while Play simulation runs, so it is not a matched Game-view comparison.

## Render and gameplay loop times

All values below are milliseconds. These scopes are nested or overlap; do not sum the whole table.

| Scope | Mean ms | P95 ms |
| --- | ---: | ---: |
| CPU frame work | 18.078 | 28.852 |
| Render host scope, including simulation join | 18.059 | 28.833 |
| Render setup/preparation, including scene preparation and UI construction | 10.336 | 11.127 |
| Scene preparation | 8.966 | 9.652 |
| Render item preparation (inside scene preparation) | 8.790 | 9.484 |
| Command packet recording | 2.783 | 3.243 |
| Main-pass replay | 1.622 | 4.233 |
| Shadow-pass replay | 1.814 | 9.119 |
| Editor UI construction (inside setup/preparation) | 1.256 | 1.457 |
| Editor UI draw submission | 0.119 | 0.144 |
| Present | 0.108 | 0.138 |
| Gameplay simulation on worker | 4.751 | 15.001 |
| Simulation overlapping graphics submission | 3.688 | 13.666 |
| Main-thread wait for simulation | 1.071 | 1.948 |
| Measured GPU stages | 3.900 | 7.055 |

The main recurring CPU cost is render-item preparation: **8.790 ms**, about 48% of the frame interval. Transform preparation is 0.023 ms and spatial preparation 0.055 ms. Adding simulation workers does not remove the owner-thread item-building work. The item timer includes several operations (material preparation, mesh state and draw-item construction); this capture cannot separate their individual contributions. The next profiling target is that item-preparation scope, followed by caching unchanged work and parallelizing immutable item construction where safe.

## Gameplay subscopes

| Scope | Mean ms | P95 ms |
| --- | ---: | ---: |
| Object/component updates | 2.778 | 12.324 |
| Grass updates (nested) | 0.939 | 3.178 |
| Pose phase | 0.409 | 1.430 |
| Constraints (nested) | 0.370 | 1.235 |
| Physics total | 0.993 | 2.271 |
| Physics setup | 0.157 | 0.640 |
| Physics simulation | 0.167 | 0.625 |
| Physics postprocessing | 0.638 | 1.286 |
| Post-physics gameplay | 0.563 | 1.219 |

Physics is not the main recurring frame cost. Its `physics_pending_ms` field is accumulated simulation time waiting for a fixed step, not CPU execution time. Simulation overlaps submission, so adding 4.751 ms to the render host duration would double-count work.

The worst CPU frame is frame 1835: **151.440 ms**, with **139.013 ms** simulation wall time and **137.435 ms** waiting for simulation. Object updates account for 109.304 ms; grass update accounts for only 0.688 ms on that frame. The recording does not identify the individual component responsible inside object updates, so attributing that hitch specifically to grass generation or physics would be unsupported. Another slow frame has long replay and worker wall times with little recorded thread CPU, which can include scheduling delays; wall time alone does not establish a GPU stall. GPU queries resolve asynchronously and are not same-frame proof of the cause.

## Floor collision fix

Both `grass_single_cell_test.scene` and `grass_single_cell_distant_test.scene` stored `MeshObjectCollider.collisionEnabled=false` on `Flat Grass Terrain`. They have no TerrainManager to reactivate those colliders. Enabled each authored floor collider; the stress scene keeps its existing distance-managed activation.

Added the optional `GrassPlayerFloors` regression to the existing local grass test executable. The runtime test loads both actual scenes, simulates the authored capsule under gravity at its original spawn and three additional interior drops, checks it never falls below the floor, and checks it settles with negligible vertical velocity. All eight cases passed at center Y=0.9 m. Build passed and CTest registration was verified.

Run: `ctest --test-dir build/Debug -C Debug -R "^GrassPlayerFloors$" --output-on-failure`. Direct verification log: `.local/grass-player-floors-20261103.log`. Reload these scenes from disk to use the corrected collision settings; an already loaded copy retains its old disabled collider.

Per-session evaluator reports:
.local/performance-logs/grass-loop-20261103-initial-scene.md
.local/performance-logs/grass-loop-20261103-grass_single_cell_distant_test.md
.local/performance-logs/grass-loop-20261103-grass_flat_test.md
