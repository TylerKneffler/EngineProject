# Subsurface scattering demo

Open `Engine/Core/Assets/Scenes/Rendering/subsurface_scattering_demo.scene` in the editor. The fixed camera shows
three matching skin spheres: SSS off, short RGB radius, and broad red radius.
The directional key casts shadows; the warm rear light reveals transmission
through the two flattened ear shapes. The right ear uses
`Assets/Textures/sss_thickness_ear.png` to vary thickness across its UVs; the
left ear uses a uniform thickness as a control. Toggle SSS on either material
to compare it with the original PBR result.

The forward shader samples nearby incident directional light along the local
light tangent in linear light. It changes diffuse lighting and leaves specular
and emissive lighting sharp. This is a local surface approximation, not a
depth-aware screen-space or mesh-space BSSRDF. The image comparison test checks
the SSS toggle, radius, and thickness map on DX11. DX12 and Vulkan render the
scene at 640x360 and 320x180, including a resize, and report opaque GPU timing.
