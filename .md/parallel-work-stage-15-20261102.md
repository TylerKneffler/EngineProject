# Stage 15: live mesh buffer retirement

Mesh buffer replacement now hands old vertex, index, morph, and posed buffers to the scene's deferred retirement bucket. The render owner collects that bucket after submission and releases it when the frame completion token reports finished. This covers geometry edits and surface poses on objects that remain in the scene.

The CPU completed-frame barrier remains because rendering still reads live scene objects and components. The final owned-render-packet TODO remains open. Other component-owned GPU resources and in-place writes require a separate audit before removing the barrier.

Verification: Debug Game, Editor, and GrassPerformanceTests build. The full GrassPerformanceTests executable passes, including a regression checking that live mesh vertex and posed buffers enter retirement rather than being released on replacement.
