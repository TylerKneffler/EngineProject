# Stage 17: owned native graphics commands and fence retirement

The simulation owner records shadow, particle, scene, and native UI graphics commands into value-argument packets. The render owner replays these commands and does not dereference live objects or components. The temporary draw list with object and component pointers stays on the simulation owner. Mesh buffer owners, shared textures and buffers, and packet-owned UI vertices remain alive with each submitted packet until its GPU completion token. Replaced scene resources move from the simulation owner with the packet and retire after the same token.

Mutable scene upload buffers rotate by backend frame slot. Native portal shadow buffers and live mesh edits replace upload buffers before a new frame writes them, with old buffers deferred through the fence. Component removal, scene reset, and reload drain pending GPU work before component-owned resources can be destroyed. The native render handoff accepts one pending packet; the next frame setup, resize, idle wait, and shutdown drain it.

The simulation owner still waits for the next frame slot after the previous render submission. Preparation of frame N+1 therefore does not yet overlap CPU graphics submission for frame N. That work remains under the parent rendering TODO.

At this stage, the recorded native path disabled GPU occlusion queries because their previous-frame results belonged to the render owner. Restoring that feedback without a live backend read on the simulation owner was tracked separately under the parent TODO.

Stage 18 restores render-owner occlusion feedback and overlaps the next simulation update with prior-frame submission.

Verification: Debug Game, Editor, and GrassPerformanceTests build. The full GrassPerformanceTests suite passes. DX11, DX12, and Vulkan command-replay pixel checks have zero reference differences. Hidden native DX11 and DX12 Game runs completed several frames and exited with code 0 after WM_CLOSE. Temporary renderer preferences were removed.
