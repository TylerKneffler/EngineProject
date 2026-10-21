# Particle rendering contract

The CPU emitter owns particle state and simulation. Rendering reads live state
into a frame-slot upload buffer and issues one instanced quad draw per emitter
texture/blend batch. Particles do not create scene objects. The same HLSL shader
and graphics pipeline contract are used by DX11, DX12, and Vulkan; Vulkan builds
the shader as SPIR-V.

Scene and Game views use their own camera matrices. Quads face the active camera,
use particle color, size, rotation, opacity, and optional RGBA texture, and fade
over lifetime. They pass the normal depth test and never write depth. Additive
and source-alpha blend modes are available.

Portal views reuse the particle records with the connected camera. Stencil
testing limits draws to the portal aperture, the destination content scope
filters emitter objects, and the exit clip plane removes particles on the
wrong side of the destination portal. Each visible portal view draws its own
eligible particle batches.

Alpha particles draw in scene object order and then in stable particle order
within each emitter. They are not sorted by camera depth; overlapping alpha
effects can therefore blend differently as the camera or object order changes.
Additive effects are insensitive to this ordering. Particles blend after scene
geometry and do not cast shadows or write occlusion depth. The CPU scene budget
defaults to 100,000 live particles, matching the render record limit; excess
spawns are discarded rather than deferred.

Soft-particle intersection fading is currently disabled. It requires a
shader-readable scene depth snapshot or resolve for each view, including portal
views, plus synchronization across all three backends. Lifetime fade remains
active, but quads can show hard intersections with opaque surfaces.

The editor selects emitters as scene components, draws their spawn shape and
live bounds, and can focus the camera on those bounds. Individual particles
are not selectable scene objects.
