# Worker command recording: native Game shadow stream

The native Game render owner can send the recorded shadow packet to an engine job worker. Each backend wraps a command stream owned by that worker. DX12 records into a separate command list and allocator for each frame slot, submits it before the main frame list, and reuses the allocator only after the slot fence. Vulkan records into a separate primary command buffer and command pool for each frame slot. Its query-pool resets are recorded into that worker buffer so they execute before its queries; the two buffers are submitted in shadow-then-main order. DX11 has an optional deferred context, finalized as an `ID3D11CommandList` and executed on the immediate context before scene rendering.

The path is opt in with `ENGINE_WORKER_COMMAND_RECORDING=1`. DX11 also needs `ENGINE_DX11_DEFERRED_COMMANDS=1`; value `2` alternates immediate and deferred recording on successive frames for a matched benchmark. `ENGINE_COMMAND_RECORD_BENCHMARK=<csv path>` writes per-frame worker mode, shadow CPU time, render-owner frame time, and update-to-submission latency. The default remains immediate/serial recording because a single shadow packet offers little CPU overlap with the scene packet.

## Native measurements

Debug Game, default scene, 1280 x 720, warmup frames excluded. The DX11 alternating capture contains 5,341 immediate and 5,342 deferred frames in the same run. Immediate shadow recording had a 0.084 ms median; deferred recording and execution had a 0.260 ms median. Render-owner frame p95 was 0.411 ms immediate and 0.757 ms deferred. Update-to-submission latency p95 was 2.909 ms immediate and 4.610 ms deferred. DX11 deferred contexts do not show a benefit on this route.

Separate 30-second serial/worker captures for the explicit backends show DX12 render-owner p95 of 1.332/1.264 ms and Vulkan p95 of 0.270/0.358 ms. Median shadow CPU time was 0.059/0.208 ms for DX12 and 0.021/0.082 ms for Vulkan. These are single-run static-scene measurements, so the small DX12 frame-p95 difference is not evidence of a gain. Additional independent packets and matched moving routes are needed before enabling worker recording by default.

## Verification

Debug Game and Editor build. Native Game completed 30-second serial and worker runs on DX11, DX12, and Vulkan with exit code 0. The per-frame CSV confirms that the DX12/Vulkan worker paths recorded thousands of frames. DX11 deferred recording remains optional because its GPU timestamp state is tied to the immediate context; its benchmark uses CPU and latency measurements.
