# Distance texture quality

Scene Properties > Texture LOD controls the shared built-in object shader, masked shadow shader, foliage shader, and terrain layer sampling. Existing scenes default to disabled.

- Distance Texture LOD enables the policy.
- Full Quality Distance is the start of the transition in scene units.
- Lowest Quality Distance is the end of the transition.
- Maximum Mip Bias adds 0-8 mip levels to hardware derivative-based sampling. A bias of 4 requests approximately one-sixteenth the texture resolution along each axis, subject to the available mip chain and filtering.
- Simplify Distant Shading smoothly fades normal maps, metallic/roughness maps, occlusion detail, parallax, and environment reflections. Optional detail-map samples are skipped after the transition. Direct lighting transitions from PBR to diffuse-only lighting, retaining light attenuation, cookies, IES profiles and realtime shadows.

Material Properties > Texture LOD allows an Override Texture LOD profile. Enable the override and disable its texture LOD to exempt a material from the scene policy, or choose independent distances and bias. Overrides are saved in both material assets and inline scene components.

The policy is evaluated per fragment using distance from the active render camera. It works with instancing and large surfaces. Built-in masked color and shadow paths share the distance policy. Custom material shaders must use the helpers in Object.hlsl to opt into it. Shader LOD leaves texture residency unchanged. For an independent opt-in upload limit, set `ENGINE_TEXTURE_MAX_DIMENSION` before launch; see `texture-residency.md`.

Terrain triangles are normalized to upward-facing winding when generated or restored from caches, and terrain color/shadow passes cull backfaces. Material double-sided settings remain available for other objects. Oak bark uses one-sided rendering; flat leaf cards keep two-sided rendering.

Masked surfaces default to normal hardware-selected alpha coverage while color uses coarser mips. Maximum Alpha Mip Bias (0?8) independently reduces alpha sampling quality over the same distance interval. Disable Alpha Testing At Far Distance removes the mask and its coverage sample once Lowest Quality Distance is reached; the surface then renders as opaque geometry. This can reveal card rectangles, so use it only with suitable distant geometry. Both controls are opt-in, available in scene settings and material overrides, and shared by built-in color, foliage, and shadow passes. Renderer timing and near/mid/far visual comparisons are still required before choosing oak defaults.
