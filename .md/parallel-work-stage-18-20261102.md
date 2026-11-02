# Stage 18: concurrent native simulation update and occlusion replay

The native simulation handoff now has two phases. Frame N+1's scene update starts while the render owner can still be submitting frame N. The window owner then waits for the render owner to prepare a reusable frame slot before asking the simulation owner to publish its snapshot and record shadow and scene commands. This preserves the scene commit boundary and the one-packet render queue. Resize, scene destruction, idle waits, and shutdown continue to drain submitted work. Minimized frames update the scene without writing a frame slot that may still be in use.

DX11 enables multithread protection on its immediate context. DX12 texture-upload submissions and frame submissions share a queue lock across each submission and fence signal. Vulkan texture uploads, frame submissions, presentation, and device idle waits share a queue lock. These guards allow update-side resource creation while the preceding render packet is submitted.

Recorded occlusion decisions are evaluated by the backend context during render-owner replay. Replay omits the hidden object's draw and query commands while preserving its resource bindings and GPU timing transitions. The visible object's query is recorded for later frames. The completed replay cull count is reported to the simulation owner on the next frame, so native diagnostics reflect backend decisions without reading query state on the simulation thread.

The update phase overlaps render submission when their work durations coincide. Slot-bound render preparation and command recording still begin after the previous packet is submitted; they can overlap execution of that packet on the GPU. The next frame slot is never prepared before its backend fence wait.

Verification: Debug Game, Editor, and GrassPerformanceTests build. The full GrassPerformanceTests suite and the recorded-occlusion regression pass. DX11, DX12, and Vulkan pixel comparisons have zero reference differences. Six-second native Game runs on all three backends exit cleanly after WM_CLOSE; renderer preferences are removed by the smoke script.
