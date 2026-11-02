# Parallel work: asynchronous asset thumbnails

Asset-browser thumbnail preparation now runs on the shared worker scheduler.
Only visible grid tiles request previews; folders with many assets no longer ask
for every off-screen thumbnail while constructing the grid.

## Implemented

- At most two thumbnail preparation jobs can be queued/running across preview
  clients. Requests and polling never wait for decoding to finish. Jobs contain
  owned paths, stamps, and cancellation tokens, with no scene/provider pointers.
- Workers perform file timestamp checks, private image/HDR decoding, material
  scalar parsing, rig/brush/model loading, and CPU rasterization. The resulting
  owned 128 x 128 RGBA pixels return to the editor owner.
- `Texture::DecodeDetached` bypasses live texture caches and omits mip-chain
  generation for CPU preview sampling. Brush-tip decoding uses the same path.
  Material preview parsing reads only the scalar fields its rasterizer uses,
  avoiding live texture acquisition and generic cache pruning on workers.
- The draw path consumes ready results and creates graphics textures on the
  graphics owner. GPU-backed previews stay in owner-held caches with 30-second
  inactivity retention; global sharing/invalidation uses weak references.
  Expired entries are released during owner polling before drawing, avoiding
  destruction of textures referenced by the current UI draw commands.
- Completion polling requests a redraw, including jobs that outlast the recent
  visibility window. Periodic metadata refresh occurs on workers. Scene previews
  use the derived sidecar timestamp, allowing independent thumbnail replacement.
- Invalidation, cache destruction, and provider replacement discard stale futures.
  Cancellation is checked around preparation boundaries; an already-running
  decoder may finish its current operation, but cannot publish into a new entry.

## Validation

`AssetPreviewAsyncTests` verifies the two-job bound while both workers are gated,
nonblocking request/cache destruction, cancellation of queued work, stale ready
result rejection, completion-driven redraw after a two-second delay, external
file refresh, shared-client invalidation, and owner-only GPU upload/destruction.
It exercises private image decoding plus image, material, brush, model, and scene
sidecar previews. Cached live texture pixels stay unchanged during replacement
image decoding. The optional CMake target is source-guarded and the local test
has a narrow gitignore exception.

The focused CTest run passed AssetPreviewAsync, GrassPerformance, and
MaterialShaderRegression. Debug Editor and ProjectScripts rebuilt. The complete
editor smoke trace `.local/performance-logs/parallel-stage3-editor.jsonl` finished
120 warmup Play frames and 120 measured Play frames. All 276 accepted records were
written, with zero dropped/failed records, and the trace ends in `capture_complete`.

This completes the asset-thumbnail TODO. Full asset import, scene capture/save,
shader compilation, live render-texture decoding, and the dedicated render-thread
migration remain separate TODOs. No grass-scene FPS gain is claimed from moving
editor thumbnail work.
