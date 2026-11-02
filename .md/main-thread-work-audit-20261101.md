# Additional main-thread work audit

Static audit of the editor frame loop and major rendering, serialization, asset,
audio, picking and export paths. This identifies scheduling candidates, not newly
measured bottlenecks. Existing grass-capture timings still prioritize submission
stalls and render preparation. Implementation tasks are in `.md/todo.md`.

| Work found | Evidence | Proposed ownership |
|---|---|---|
| Recording formatting and writes | `PerformanceRecording.cpp:136` locks and writes events/frames; `:160` periodically flushes. `EditorFrameProfiler.cpp:247` formats and writes during frame recording. | Copy immutable records on owner; bounded background formatting/writer queue, ordered output, overflow accounting and stop/drain. |
| Asset-browser disk scans | `AssetsExplorerView.cpp:597` enumerates directories in `DrawGridDirectory`, then filters and sorts. Tree enumeration also exists at `:439`. | Worker-maintained directory/metadata index and cached UI snapshots; notifications invalidate entries. |
| History, saves and scene decoding | `EditorState.cpp:5283` captures a full scene snapshot; `:5311` tracks document snapshots. Save invokes scene serialization/writes at `:1958`; loading calls into deserialization at `:2218`. | Incremental/versioned snapshot capture on scene owner; encoding, comparison, file IO and parsing on workers; ordered scene publication. The main history path already skips unchanged revisions and active drags. |
| Import and asset remapping | `EditorState.cpp:4937` invokes model/motion decoders and writers synchronously; model-stage opening also invokes import at `:2593`. | Cancellable worker jobs producing detached results, then bounded owner-thread binding/resource publication. |
| Lighting bake | `EditorState.cpp:2090` calls `Scene::BakeLighting` directly. | Background bake over immutable scene/asset inputs with progress, cancellation and version checks. |
| Lazy shader/texture preparation | `SceneRendering.cpp:1297` compiles custom shader variants on cache misses; `:2137` prepares textures during item preparation. `Texture.cpp:275` decodes image data synchronously. | Background bytecode compilation and texture decode; prewarming and thread-safe caches; graphics resource/pipeline publication on its owner. |
| CPU particle work and collisions | `ParticleEmitter.cpp:626` advances particles; `:790` traverses scene bodies inside fluid collision processing. | Independent emitter jobs or existing GPU simulation, shared collider snapshots and batched physics queries; no concurrent unprotected access to the live physics world. |
| CPU deformation and warped meshes | `SceneRendering.cpp:2025` onward samples warp fields and processes mesh vertices during render preparation. Skin palettes are also prepared in the same scene walk. | Worker mesh/rig preparation from frozen transforms and geometry; reuse unchanged results; publish GPU uploads on render owner. |
| Picking and bone selection | `ScenePlacementAndPicking.cpp:256` walks objects, then processes source vertices, joints and triangles for skinned picking. | Cached broadphase/triangle indexes and worker-prepared posed geometry; revision-tagged hover queries; selection commit remains on UI/scene owner. |
| First-use audio decode | `AudioSource.cpp:117` loads sounds; `:134` decodes a full OGG file before initialization. | Asynchronous preload/decode and bounded playback-command publication. Audio rendering/mixing is a separate ownership concern. |
| Hot-reload preparation | `EditorHotReload.cpp:238` copies module/symbol files; `:267` snapshots the scene before replacement. `ScriptFileWatcher.cpp:94` implements recursive fallback scanning. | Background copy/validation/scans, bounded log processing, then a coordinated safe module/scene replacement boundary. Never unload code still referenced by queued callbacks or registry functions. |
| Build console handling | `GameBuildManager.cpp:320` drains all available child-process output and forwards lines to the console. | Budget UI log publication and parse large output off-thread; keep process polling nonblocking. Compilation is already in a child process. |
| Editor previews | `EditorApplication.cpp:478` walks objects in open preview scenes to find active emitters and animations. | Active preview registries and simulation/job scheduling; UI consumes completed preview state. |
| Export image output | `VideoExportApplication.cpp:1181` consumes readback results; encoding/output work follows the export loop. | Graphics-owner readback plus bounded CPU encoding/write queues, frame ordering and deterministic export backpressure. Separate from ordinary editor FPS. |

## Existing background work to preserve

- Hardware sampling already has a dedicated sampler thread in
  `PerformanceView.cpp:356`; remaining frame-record formatting/writing is separate.
- Script watching already uses overlapped `ReadDirectoryChangesW`; the recursive
  scan is a fallback, not a per-frame full scan under normal operation.
- Builds execute as child processes. The frame loop polls status and drains output.
- Render visibility and packing jobs, terrain asynchronous streaming, and GPU
  particle/grass paths already exist. Extend their coverage rather than duplicating
  pools or moving already asynchronous work again.

## Ownership and priority

Keep window message processing, UI construction and immediate input capture on the
UI thread. The proposed simulation thread owns live scene mutation and lifecycle
ordering. The render thread owns graphics submission, resource publication and GPU
readback operations. Workers consume immutable inputs and publish versioned results.
Audio's real-time path must not block on scene or disk work.

Prioritize recurring scans/preparation and recording overhead for steady frame time.
Import, bake, save, shader first use and audio first use are primarily hitch and
responsiveness targets; this audit does not establish their cost in the supplied
grass recording. Export is a separate throughput workload. Benchmark them separately
and track UI, simulation, render, worker and writer threads independently after the
game loop is split.
