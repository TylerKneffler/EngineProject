# Material shader assets

Materials expose an ordered **Shaders** list in both the object inspector and
the material asset inspector. Select or drag an `.hlsl` asset into **Shader file**,
or enter a relative or absolute path in **Asset path** and press Enter. Other
file types are rejected by the picker. Use Move up/down, Enabled and Remove to
manage the passes. An empty list retains the built-in surface.

The first enabled entry draws the surface; subsequent enabled entries redraw the
same geometry as overlays. An empty path explicitly uses the built-in surface.
**Auto** uses the material's alpha mode for the surface and alpha blending for
overlays. **Opaque**, **Alpha** and **Additive** override blending per entry.
Overlays test depth with less-or-equal and never write depth. Passes share material
textures, lights, transforms, bones and morphs. Each additional pass costs a draw.
This does not feed one shader's pixel output into the next shader as a texture.

For example, retain the built-in surface and add a warm tint:

```json
"shaders": [
  { "path": "", "enabled": true, "blendMode": "Auto" },
  { "path": "Assets/Shaders/WarmOverlay.hlsl", "enabled": true, "blendMode": "Alpha" }
]
```

Shader paths use the same resolution as textures: project/working-directory
paths, bundled `Assets/...` paths, absolute paths, and paths beside the material
asset. List entries are saved in `.material` files and scene/prefab component data.
String-only entries are also accepted when loading authored JSON.

Files must provide `VSMain` and `PSMain`, using the engine's mesh input and GPU
binding layout in `Engine/Core/Shaders/Object/Object.hlsl`. Terrain additionally
needs `VSTerrainMain`. The bundled `Assets/Shaders/WarmOverlay.hlsl` is a working
example that includes that layout and preserves skinning, alpha masking, and
portal clipping. Arbitrary shaders written for another engine need adaptation.
Shadow maps continue to use the engine's shadow pass, not the color-pass stack.
Sprites use their existing sprite renderer.

DirectX 11/12 compile the referenced HLSL at runtime. Vulkan builds discover mesh
shader `.hlsl` files under engine/project Assets and compile SPIR-V into
`VulkanShaders/Materials`, preserving their relative paths. Rebuild after adding
or changing Vulkan shader assets; packaged exports copy this binary tree.
For paths outside Assets, supply adjacent `<file>.hlsl.VS.spv` and
`<file>.hlsl.PS.spv`; terrain also needs `<file>.hlsl.VSTerrainMain.VS.spv`.
Compile with DXC `-spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-layout -D VULKAN=1`,
the corresponding `vs_6_0`/`ps_6_0` profile and entry point. Terrain must use
`-fspv-entrypoint-name=VSMain` as well as `-E VSTerrainMain`.

Missing/invalid surface shaders fall back to the built-in surface; invalid
overlays are skipped. The object inspector displays compilation errors. Pipelines
are shared across materials with identical shader paths and render state; changing
the source file timestamp rebuilds the affected cached pipeline. Restart after
editing shared include files or replacing only SPIR-V binaries.
