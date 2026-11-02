# Stage 16: native frame draw packet handoff

The simulation owner moves its prepared frame draw list into a packet at the render handoff. Native shadow and camera passes consume that packet instead of the scene's mutable draw list. The packet also retains shared mesh vertex and index buffers. The render owner keeps the packet through the GPU completion token before releasing it. The handoff remains bounded to one CPU frame.

The draw entries still borrow objects, components, and other render state. The CPU completed-frame barrier remains required. The final owned-render-packet TODO stays open until draw records, UI, camera, material, texture, and pipeline inputs no longer dereference the live scene, and the barrier can be removed safely.

Verification: Debug Game and Editor build. The full GrassPerformanceTests executable passes. DX11, DX12, and Vulkan pixel regressions pass with the native draw packet path. A hidden native DX11 Game opens and exits cleanly on WM_CLOSE. The temporary renderer preference used for this check was removed.
