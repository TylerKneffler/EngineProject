# Texture residency and budgeted mip streaming

Runtime conventional mip streaming is implemented for DX11, DX12 and Vulkan.
It is opt-in: the isolated oak test on this RTX 4090 did not establish production
memory pressure, so the reference renderer retains its full-resolution default.
Virtual texturing remains deferred. No representative working-set or bandwidth
measurement has established a need for it.

## Enable and configure

Set `ENGINE_TEXTURE_STREAMING_BUDGET_MB=256` before starting the engine. This is
an allocation budget in MiB for Texture-created GPU resources on each provider,
including pinned textures, pending replacements and images retained by in-flight
frames. Missing/invalid/zero disables streaming. `ENGINE_TEXTURE_MAX_DIMENSION`
remains an independent ceiling. Source dimensions and CPU/editor pixels retain
their full resolution.

Graphics-owner code can call `Texture::ConfigureStreaming(TextureStreamingSettings)`.
Settings include `budgetBytes`, `uploadBytesPerFrame` (128 MiB default),
`holdFrames` (30), `prefetchMips` (one finer mip) and `maxDecodes` (two).
Changing a live budget stops new admission immediately and retires surplus detail
without waiting. Existing allocations can temporarily exceed a newly lowered
budget until their last-use fences finish; new allocations cannot exceed the
remaining budget. Enabling after load queues fallback preparation; disabling
queues full-quality restoration while retaining the current binding until ready.

## Runtime policy and GPU lifetime

The renderer gathers demand outside its material cache, after resolving draw
records. A cached per-geometry UV metric bounds local units per UV repeat from
triangle derivatives, including secondary UV selection and geometry revisions.
A projective Jacobian bound includes the camera projection, actual viewport,
nonuniform transforms, near-plane intersections and frustum rejection. Every
instance contributes. Color views, portal renders and directional-shadow cascades
merge their highest demand, including offscreen shadow casters. Terrain tiling and
world-space UV mapping adjust the footprint. Deformed/custom-coordinate surfaces,
UI, particles and photometry use conservative demand where derivative bounds are
unavailable; pressure can still reduce detail and reports unmet demand.

The desired first mip is floor(log2(source dimension / projected UV-repeat
pixels)), minus prefetch, clamped to the chain and dimension ceiling. A sliding
minimum retains recent fine demand through turns and expires it for visible
receding objects as well as hidden ones. Pressure overrides the hold. High
projected footprints win over hidden/low-footprint textures; equal priorities
preserve registration order. Multiple scenes on one provider share a streaming
tick and upload budget per prepared native frame.

Each managed texture retains a permanent fallback tail, at most 64 pixels in
its largest dimension (or the smaller explicit dimension cap), rather than a
single averaged texel. Fallbacks preserve more cutout structure while finer tails
load. Their allocations count against the budget. If a floor cannot fit, the
resource stays pending and blocked admission is reported; the controller never
drains the GPU or silently grows past its allocation budget.

Planning reserves pinned textures and fallback storage. Detail admission includes
simultaneous old/new storage and already-retiring images, using exact DX12/Vulkan
allocation queries and a rounded payload estimate on DX11. Intermediate tails
are admitted when the finest request cannot fit. Replacements publish after
upload completion; the old binding remains available meanwhile. A strict upload
ceiling can leave a large atomic tail unmet; configure the ceiling for the largest
desired tail. The best attainable tail remains resident instead of repeatedly
being evicted because the next tail exceeds the upload ceiling.

CPU decoding/mip generation uses bounded background jobs. Invalidation, provider
changes and preview replacement discard obsolete publication state. Full sources
are re-decoded when necessary; no new cooked asset format is required. HDR and
explicitly CPU-retained textures remain pinned and budgeted. Streaming preserves
source pixels, geometry, shaders, alpha cutoffs and authored assets.

DX12 upload lists, allocators, staging and image owners survive until polled fences
complete. Vulkan uses submission fences and cleans partial allocations on failure.
Copies use the graphics queue, ordering subsequent draws without queue-idle waits.
Staging in flight is bounded; capacity pressure defers uploads. Shutdown may wait;
runtime upload, replacement and retirement do not.

Direct and worker contexts retain sampled/depth textures in native frame slots;
slots release owners after their renderer fence completes. DX11 retains draw
owners behind event queries polled with DONOTFLUSH. Context-local caches avoid
repeated locking for unchanged bindings. DX12 descriptor slots return only after
the texture wrapper loses its owners. Vulkan descriptor keys include monotonic
texture identities, preventing stale view-handle reuse.

## Telemetry

The Performance view and JSONL recorder report:

- Resident/source/uploaded payloads, upload counts, resource releases, omitted
  top mips and resource count.
- `texture_requested_bytes`, `texture_allocated_bytes`,
  `texture_streaming_budget_bytes`, `texture_pinned_bytes`,
  `texture_retiring_bytes`, `texture_staging_bytes`.
- `texture_streaming_upload_bytes_total`, `texture_evictions_total`,
  `texture_unmet_demand_count`, `texture_blocked_admission_count`.
- `texture_mip_demand` and `texture_resident_mips`: arrays indexed by source mip.

Difference cumulative upload/eviction totals for interval volume. Demand reflects
the previous view collection. These are live allocation/payload measurements,
not queries of physically resident VRAM pages. Retired owners remain counted.
Direct factory resources (render targets, BRDF LUTs, fonts) and driver allocations
are outside the Texture budget; compare it with the existing selected-adapter OS
usage/budget counter when choosing a target-hardware budget. DX11 allocations are
estimates. Source payload counts per resource, including fallback/replacement
owners referring to one source.

`Texture::GetStreamingInventory()` exposes owner-thread per-path/color-space
dimensions, mip levels, resident/demand mips and managed/queued/failure state.
The CPU-only `plan_texture_streaming.py` remains a separate policy simulator;
its single-mip floor and immediate reclamation do not model runtime fence lag.

## Measurements and validation

The six oak PNGs contain 288 MiB of RGBA8 mip payload at full resolution,
96 MiB with a 2048 dimension cap and 24 MiB at 1024. Inventory excludes staging,
alignment, duplicate color-space bindings and non-PNG assets. The 2026-11-07
Release isolated upload audit measured:

| Backend | Full dedicated MiB | 2K dedicated MiB | 1K dedicated MiB |
|---|---:|---:|---:|
| DX11 | unavailable | unavailable | unavailable |
| DX12 | 288.188 | 96.188 | 24.188 |
| Vulkan | 288.012 | 96.012 | 24.012 |

Full local process usage was 298.039 MiB on DX11 and 309.293 MiB on DX12 against
a 22.8 GiB OS local budget. This small test does not establish the complete forest
working set or pressure on constrained hardware. All 54 static leaf-mask
comparisons had IoU/coverage ratio 1.0. Historical captures/allocations are in
`.local/texture-residency-budget-validation/`. That validator intentionally returns
1 for unavailable DX11 dedicated-allocation validation even when every supported
allocation and mask gate passes.

`TextureStreamingTests` checks projected resolution/UV demand, prefetch, recession
hysteresis, incomplete-upload publication, completed-frame retirement and budget
floors. The Python suite guards runtime upload/retirement against queue/device
idle and blocking fence calls, and exercises turns, teleports and pressure.

`TextureStreamingBackendTests` runs full-quality reference captures, uninterrupted
rapid 180-degree turns, teleports/backtracking and constrained working sets on all
three backends. It reserves a pinned texture and exercises 8/2/0.5 MiB streamable
pools plus that allocation. Every measured route frame must fit the total budget;
the constrained case must report unmet demand. Nine settled and 24 transient
cutout comparisons per backend require IoU >= 0.90 and coverage ratio 0.95–1.05.
A separate shadow pass must retain demand from an offscreen caster. A 1,200-image
stress exceeds the DX12 heap capacity to check descriptor recycling. Runtime
scene resource-drain callbacks must remain zero.

Diagnostic readback waits are outside uninterrupted routes and excluded from
timing. Full traces, reference/settled/transient PPM/PGM captures and backend
summaries are in `.local/texture-streaming-validation/`. The summary script reports
host mean/P95/P99, available deduplicated GPU opaque samples, warmup-inclusive
timings, requested/resident/staging/retiring peaks, uploads, evictions, unmet demand
and executable/source fingerprints. These are correctness/pressure fixtures,
not a whole-forest FPS acceptance measurement.

```powershell
cmake --build build --config Release --target TextureStreamingTests TextureStreamingBackendTests TextureResidencyTests
ctest --test-dir build -C Release -R '^Texture(Streaming|Residency_)' --output-on-failure
python Scripts/Assets/summarize_texture_streaming.py .local/texture-streaming-validation
```

Use matched target-hardware scene routes and OS measurements to choose production
budgets. Evaluate virtual texturing only after conventional mip streaming has a
measured working-set/bandwidth limit with acceptable mip quality.

## Completed Release validation, 2026-11-08

All 12 targeted CTest entries pass. Across three APIs, all 4,860 uninterrupted
turn/teleport/backtracking frames stay within their texture allocation budgets.
All 27 settled and 72 transient cutout comparisons pass; minimum transient IoU
is 0.996775 and maximum coverage ratio is 1.000294. Offscreen caster demand stays
at source mip 2 instead of the fallback mip 3. All 3,600 descriptor replacements
complete and runtime scene resource-drain callbacks remain zero. Each backend's
36 explicitly synchronized diagnostic captures are recorded separately; earlier
unsynchronized harness captures and failed shadow-pass setup runs are superseded.

| Backend | Small total budget MiB | Peak allocated MiB | Peak requested MiB | Unmet route frames |
|---|---:|---:|---:|---:|
| DX11 (estimated allocations) | 0.625 | 0.500 | 1.382 | 540 |
| DX12 | 0.625 | 0.500 | 1.382 | 540 |
| Vulkan | 0.585 | 0.499 | 1.382 | 540 |

The small budget includes the pinned texture and forces a measurable quality
shortfall rather than allocating past the limit. Full per-budget upload/eviction
volumes and timing percentiles are in `.local/texture-streaming-validation/summary.md`
and `summary.json`. Each API resolves 1,620 unique GPU opaque samples for its
1,620 route frames. These fixtures do not establish whole-scene performance gains.

The final synchronized real-oak regression after asynchronous-upload changes
passes all 54 mask comparisons and all six supported DX12/Vulkan allocation
gates, reproducing the allocation table above. Evidence is in
`.local/texture-streaming-oak-regression/summary.json` and its logs/captures.
DX11 dedicated allocation remains unavailable, so the existing combined oak
validator returns 1 for that unsupported measurement. This is distinct from the
12 fully passing CTests and the supported allocation/mask gates.

Build/check logs: `.local/texture-streaming-verified-build.log`,
`.local/texture-streaming-backend-final-build.log`,
`.local/texture-streaming-final-ctest.log`, `.local/texture-streaming-oak-build.log`
and `.local/texture-streaming-oak-regression.log`.
