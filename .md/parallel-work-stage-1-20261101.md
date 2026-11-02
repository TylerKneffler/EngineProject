# Parallel work: first implementation stage

This stage implements the shared CPU scheduler and several bounded consumers.
The dedicated simulation/render threads and parallel lifecycle/component updates
remain unchecked in `.md/todo.md`; scene mutation and graphics ownership have not
been migrated in this stage.

## Implemented

- `Core/Threading/JobScheduler.h` replaces the render-only pool. It supports
  dependency handles, failure propagation, cooperative nested parallel loops,
  draining shutdown, worker-only asynchronous submissions and execution/queue/wait
  counters. `ENGINE_WORKER_COUNT` overrides the hardware-based default; zero is
  an explicit serial verification mode. Dependency waits use `Schedule`/`Wait`;
  existing result-returning `Submit` calls retain their future API.
- Small render workloads can use multiple contiguous batches rather than
  broadcasting one item at a time to a fixed three-worker pool. Thread-affine
  loops retain an explicit serial path. Background filesystem submissions cannot
  be stolen onto the UI thread by cooperative CPU waits.
- `AsyncTextWriter` moves record formatting, escaping, timestamps and file writes
  onto a dedicated writer. Producers copy immutable typed values with
  `CapturedText`; no scene/component/renderer pointers are dereferenced by the
  writer. Normal Performance recordings and opt-in editor traces use the writer.
  Each queue is bounded to 8 MB / 4,096 waiting records, reports dropped/failed
  records and drains accepted records on stop. The evaluator warns about record
  loss instead of silently presenting a complete-capture interpretation.
- `DirectorySnapshotCache` enumerates, filters, sorts and collects file metadata
  on workers. Tree/grid asset views consume immutable snapshots. Recursive Windows
  notifications invalidate caches, with a one-second polling fallback. Navigation
  and search keys isolate results; there are at most four in-flight scans and 128
  cached directory/query entries. Thumbnail resource preparation remains future
  work.
- `RenderBoundsCache` reuses unchanged geometric bounds by stable render slot.
  Changed bounds are prepared from copied matrices and boxes, then committed after
  the jobs finish. Live hierarchy traversal and transform mutation remain on their
  owner. Recordings expose bounds reuse/update counts and preparation job timings.
- Edit-mode previews use typed emitter/animation registries rather than scanning
  every scene object. Their updates remain serial until the component dependency
  and ownership work is implemented.

## Verification

- Editor Debug build and nine targeted engine tests passed; ten evaluator tests
  passed. Added coverage includes dependency ordering/failures, worker affinity,
  nested progress, serial mode, shutdown drain, writer overflow/order/ownership,
  directory create/delete notifications and isolated bounds invalidation.
- Grass reference images, GPU compaction, fades, LOD and shadow receiver checks
  passed on DX11, DX12 and Vulkan. Automatic mesh instancing and edit-isolation
  image checks passed on all three backends as well.
- A full editor default-scene capture reached 120 warmup Play frames and 120
  measured Play frames, then completed normally. All 276 accepted trace records
  were written, with zero drops/failures; peak queued memory was 18,079 bytes.
  Capture: `.local/performance-logs/parallel-stage-editor-writer.jsonl`.
- The 441-cell resident/streaming routes completed. Bounds reuse averaged over
  500 items per measured frame. Raw data is in
  `.local/performance-logs/parallel-stage-{resident,streaming}.jsonl`.

Worker execution counters are summed job wall time, including scheduling/IO
effects; they are not per-thread OS CPU percentages. Shared-scheduler counters
observed during rendering can include other completed engine jobs and must not
be added to frame totals. Dependency readiness is queued dependency delay, while
worker synchronization measures actual condition-variable waiting.

Historical route captures differ in system conditions: CPU and GPU stages both
became slower in the initial stage run. These results do not establish a full-editor
FPS gain. A serial/parallel preparation comparison uses the same loaded landscape,
viewport, quality and deterministic route, with fixed appearance coverage, to
check scheduling costs separately. Its serial mode disables parallel loops while
preserving asynchronous record packing and worker-only IO.

The final comparison resets generated tile residency before each mode and warms
over 300 frames, then measures 240 frames. Results:

| CPU measurement | Resident serial | Resident parallel | Streaming serial | Streaming parallel |
|---|---:|---:|---:|---:|
| Camera culling | 1.257 ms | 0.791 ms | 1.896 ms | 0.832 ms |
| Main render total | 10.970 ms | 10.690 ms | 13.108 ms | 12.274 ms |
| Frame wall time | 21.428 ms | 21.364 ms | 35.325 ms | 34.368 ms |

This demonstrates a useful preparation-phase reduction, not a large overall FPS
improvement. GPU timings vary between the sequential runs. The resident route
matches placement counts at 238/240 route frames; streaming at 172/240 because
tile publication retains its wall-time budget. Streaming numbers are therefore
not an exact identical-residency comparison. Raw captures:
`.local/performance-logs/parallel-comparison-{resident,streaming}-{serial,parallel}.jsonl`.

## Remaining priorities

Instrument the native submission stalls, then migrate larger render-preparation
and component workloads using explicit dependencies and immutable inputs. A
simulation/render thread split requires input snapshots, frame ownership,
resource retirement, lifecycle ordering and transition barriers. Those are still
required before moving live scene callbacks or graphics calls between threads.
Reloadable modules also need an explicit host scheduler bridge and job completion
barriers before native/script callbacks can safely use background work across
module replacement.
