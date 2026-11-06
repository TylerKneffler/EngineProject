# Prefab placement performance

Enable detailed timing before starting the editor (PowerShell, from the repository root):

```powershell
$env:ENGINE_PREFAB_LOAD_PROFILE = '1'
& .\build\Debug\Debug\Editor.exe
```

Drag a prefab into the scene. `prefab-load-profile.log` records file reading/parsing,
object-tree deserialization, component load totals, prefab baseline serialization,
and queued graphics preparation by component type. Timings are milliseconds;
component times are included in their enclosing stage, so do not sum all rows.
Profiling is disabled when the environment variable is absent.

Scene-view prefab drags prepare file data, meshes, materials, rig/clip data, and
packed morph deltas on a loading worker. Components attach on the scene owner; scripts are
never constructed by the loading worker. A release during loading retains its
placement. Escape cancels a pending placed prefab; abandoning a drag, changing
the viewport scene, or reloading the scene discards its pending payload.

GPU preparation for these placements yields between components using a 2 ms
soft budget. A single driver call can exceed that budget. Texture decoding and
publication retain their existing asynchronous queue and two-upload limit.
Owner-side hierarchy construction and prefab baseline capture are still serial;
this change does not guarantee every editor frame stays under 2 ms.

Reproduce placement without UI interaction:

```powershell
cmake --build build/Debug --config Debug --target PrefabPlacementBenchmark
$env:ENGINE_PREFAB_LOAD_PROFILE = '1'
& .\build\Debug\Debug\PrefabPlacementBenchmark.exe
# Background preparation, owner publication, and per-frame upload batches:
& .\build\Debug\Debug\PrefabPlacementBenchmark.exe --async
# An optional first argument selects a different prefab path.
ctest --test-dir build/Debug -C Debug -R '^PrefabDocumentCache$' --output-on-failure
cmake --build build/Debug --config Debug --target AsyncPrefabTests
ctest --test-dir build/Debug -C Debug -R '^AsyncPrefab$' --output-on-failure
```

The benchmark initializes DX12 and a scene, then adds two independent copies of
Mococo Abyssgard.low to that scene. It reports initialization separately from
prefab instantiation and graphics preparation. It excludes editor drag hit testing,
UI work, and undo bookkeeping. First placement is the first in-process load;
the operating system's disk cache is not cleared.

Local Debug samples on 2026-11-06:

| Placement | Before total | After total | Before CPU load | After CPU load |
| --- | ---: | ---: | ---: | ---: |
| First copy | 2379 ms | 2296 ms | 1463 ms | 1504 ms |
| Second copy | 2254 ms | 1995 ms | 1690 ms | 1176 ms |

The change gives prefab cache lookup and insertion the same normalized absolute
path, including Windows case normalization. Relative paths previously missed
entries inserted under absolute paths, repeating parsing and document copying.
The optimized second-copy profile has no `read_parse` stage. File timestamps and
sizes still invalidate stale entries; instance components remain independent.
These are single-run samples and include timing variability, not a guaranteed
speedup. Mesh decoding/morph loading and GPU mesh preparation remain substantial
costs. Scene initialization took 24–27 seconds in these Debug runs and is excluded
from the placement totals.

Async placement Debug samples after moving mesh/material/rig decoding and morph
packing to the worker (same benchmark; total includes background wait):

| Placement | Background work | Owner publication | Upload batches | Longest upload batch | Total |
| --- | ---: | ---: | ---: | ---: | ---: |
| First copy | 1643 ms | 591 ms | 38 | 76 ms | 3224 ms |
| Second copy | 1544 ms | 287 ms | 27 | 21 ms | 2569 ms |

This targets responsiveness rather than faster completion. The background wait
does not block the editor; the remaining hierarchy/baseline publication still
does. These samples do not show a reduction in total loading time. The upload
batch maximum also demonstrates why the 2 ms setting is a soft budget rather
than a hard frame-time guarantee. The benchmark drains batches back-to-back to
measure their cost; the editor processes them on successive rendered frames.
