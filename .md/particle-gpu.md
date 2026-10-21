# GPU particle path

The graphics interface now exposes compute pipeline creation, storage buffers,
compute constants, read and write buffer bindings, dispatch, a storage barrier,
and an explicit copy to a readback buffer. DX11, DX12, and Vulkan implement
these operations. The caller records compute work before a Vulkan render pass
and inserts `StorageBarrier` before a draw reads the output buffer. A readback
copy is complete only after the submitting frame has finished on DX12/Vulkan;
map the readback buffer after that fence. No particle state is copied to the CPU
during ordinary playback or drawing.

`ParticleEmitter` has an opt-in **GPU simulation** setting. The current compute
path supports world-space ballistic particles without collision. It uses a
fixed 60 Hz step, two dense storage buffers, a live-count header, a compaction
pass, and a spawn pass. Dead slots become available on the next compaction.
The GPU vertex shader fetches particles from the live buffer; no per-particle
scene object or CPU instance upload is needed. CPU simulation remains the
fallback for collision, custom solvers, local-space simulation, and editor
scrubbing. GPU spawn uses stable IDs and a fixed-seed hash, but parallel
compaction does not guarantee stable slot order.

The scene budget reserves each active GPU buffer's full capacity. This is a
conservative allocation because the live count stays on the GPU. The editor's
ordinary live count and world bounds currently cover CPU particles only.
`RequestGpuSnapshot` and `ResolveGpuSnapshot` provide explicit GPU count, age,
and world bounds inspection. Record the request after simulation, submit the
frame, wait for its fence, then resolve. A snapshot copies the full dense pool,
so it is intended for tools and validation rather than every-frame UI updates.
Changing the scene budget while GPU particles are live does not trim that
buffer immediately; restart the emitter to apply its new capacity. GPU buffer
reallocation retains previous buffers until emitter destruction to avoid
releasing resources referenced by an in-flight frame.

`GpuComputeTests` verifies dispatch, compaction, expiry, and explicit readback
on DX11. `ParticleRenderingTests` verifies a DX11 image and GPU snapshot.
`GpuParticleBackendsTests` runs a fixed-seed CPU reference beside the GPU path
on DX11, DX12, and Vulkan, comparing live count, age, bounds, budget, and
expiry. It captures an emitted frame and an expired frame from each backend.
`GpuParticleImageComparison.py` subtracts the empty frame and compares particle
silhouettes; DX11/DX12 and DX11/Vulkan overlap by 0.938 in the current test,
while DX12/Vulkan match exactly. DX12 and Vulkan copy the linear scene target
to a readback buffer only when a capture is requested. Render images and a
comparison sheet are written to `.local/artifacts`.

## Fluid neighbor query

`GpuParticleNeighborGrid` builds a hash grid from the dense GPU particle buffer
and runs batches of radius queries in compute shaders. It uses the CPU grid's
cell size, active and finite particle filter, source-index exclusion, and
direct scan for radii larger than two cells. The output is one bitset per query;
decoding bits in order yields ascending source indices independent of parallel
bucket insertion order. The result buffer can be consumed by a later GPU fluid
pass without readback. `RequestReadback` is for validation; wait for the frame
fence before `ResolveReadback` on DX12/Vulkan. Repeated records retain old
buffers until a resolved readback or `ReleaseRetiredAfterFence`.

`GpuParticleNeighborGridTests` compares the DX11 compute result with
`ParticleNeighborGrid` for negative and positive boundary cells, a grid of
query centers and radii, inactive and non-finite particles, index exclusions,
multiple bitset words, and a rebuilt particle snapshot. The Vulkan variants
are compiled by the `VulkanShaders` target. The query path currently accepts
up to 100,000 particles and 64 MiB of result bits. It uses 32-bit shader cell
coordinates clamped to ±1 billion and float distance arithmetic, whereas the
CPU grid uses wider cells and double distance arithmetic. Avoid relying on
identical results for extreme coordinates or distances exactly at a radius
threshold until those cases have been validated separately.
