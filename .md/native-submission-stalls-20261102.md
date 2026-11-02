# Native graphics submission stall audit (2026-11-02)

DX11 Debug, current authored scenes, 240 measured frames per route after warmup. Native timing was enabled for this diagnostic; it adds a clock and thread-cycle query around each call. The grass route measures render and spatial grass updates without editor UI or physics. Terrain measures update and render without editor UI. Raw JSONL and evaluator reports are in `.local/performance-logs/native-stalls-final-20261102/`.

| Route | Long native calls (at least 5 ms) | Largest call |
|---|---:|---:|
| Grass resident, serial | 17 frames in the focused audit | Camera dispatch 85.5 ms; shadow dispatch 84.7 ms; `UpdateSubresource` 82.9 ms; shadow draw 43.1 ms |
| Grass resident, parallel | 0 / 240 frames in the full matched run | 0.8 ms |
| Grass streaming, serial | 0 / 240 frames | 0.8 ms |
| Grass streaming, parallel | 0 / 240 frames | 0.7 ms |
| Terrain moving, serial | 0 / 240 frames | 3.1 ms |
| Terrain moving, parallel | 0 / 240 frames | 1.1 ms |

The focused resident audit split `Map`, `Unmap`, and `UpdateSubresource`. Its largest map was 0.084 ms, unmap 0.037 ms, and indirect-argument copy 0.335 ms. Shadow `UpdateSubresource` averaged 3.3 calls per frame and stalled in 7 frames; camera and shadow dispatches stalled in 5 and 3 frames. The longest upload and dispatches each consumed about 206–212 million thread cycles, consistent with time spent executing or spinning in the driver. One 6.6 ms binding call consumed only 22,465 cycles, consistent with descheduling rather than driver work. Thread cycles do not establish the exact driver or GPU cause.

In the full matched run, grass GPU stage time averaged 53–62 ms and CPU render time about 16–18 ms. GPU work is asynchronous, so these figures cannot be added, but the GPU cost makes queue backpressure plausible. The long-call category shifted across repeated resident captures; maps and indirect arguments were consistently short.

An opt-in experiment used D3D11 dynamic structured upload buffers so `Map(WRITE_DISCARD)` could rename storage instead of calling `UpdateSubresource`. The matched focused run removed long upload calls but still had 11 frames with calls at least 5 ms, including dispatches up to 120.3 ms. Frame P95 rose from 157.2 to 170.5 ms while grass GPU time changed from 62.8 to 64.5 ms. The experiment did not eliminate submission stalls and was removed from the code. This comparison is a single run per mode, so the frame-time difference is not a reliable performance effect.

DX11 blocking `WaitIdle` is now timed as `fence_wait`; the measured routes did not call it during frames. DX12 and Vulkan native-call timing are not yet connected to these DX11 captures. No buffer-reuse change is justified by the measured map or indirect-argument calls. The remaining long calls need a GPU-work or driver-queue experiment with matched image output before a performance fix can be claimed.
