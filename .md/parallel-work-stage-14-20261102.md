# Stage 14: GPU completion and resource lifetime

The standalone render owner now receives completion tokens from DX11 event queries, DX12 fence values, and Vulkan frame fences. It retains replaced scene buffers, material shader pipelines, and shadow atlases until the GPU completes the frame that could still reference them. The render loop caps submitted GPU frames at three and drains before resize and shutdown. Native object removal or scene clearing drains GPU work before component resources are destroyed. Persistent instance batch replacements remain in their frame slot until that slot is safe to reuse.

The render packet still borrows live scene objects and components. The completed-frame CPU barrier remains in place; simulation and graphics submission do not yet overlap. Component-owned GPU resources that can be replaced without removing the object still need a common retirement path before all GPU lifetime cases are covered.

Verification: Debug Game, Editor, and GrassPerformanceTests build. The regression executable passes DX11 completion-token and destructive-scene-barrier checks along with the existing scene and rendering tests. The DX12 and Vulkan instance-render checks also pass their completion-token assertions and pixel comparisons. A hidden native Game startup, resize, and WM_CLOSE shutdown exits with code 0.
