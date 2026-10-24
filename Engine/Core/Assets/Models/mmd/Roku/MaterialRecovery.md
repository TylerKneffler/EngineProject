Roku material recovery
======================

The original FBX does not contain the Blender source's custom skin/latex/hair
node graphs or usable body texture references. Re-exporting the same graphs as
FBX does not restore their appearance.

`Roku_RecoveredTextures` contains baked base color (including hair alpha),
metallic/roughness (G roughness, B metallic), and tangent-space normal maps
recovered from the packed Blender source. The checked-in `.material` assets
already reference these maps. The adjacent `Roku_V1_packed.fbx.materials.json`
also restores the maps on fresh FBX imports, matching materials by their exact
source names. Keep that companion and its textures beside the FBX.

Regenerate with Blender 5.x from the repository root:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' --background --threads 6 --python Scripts/recover_roku_materials.py -- C:/Users/TylerKneffler/Downloads/Roku_V1_packed.blend Engine/Core/Assets/Models/mmd/Roku/Roku_V1_packed.fbx
```

The script preserves the source blend and FBX. Its material conversion is
specific to Roku's authored node groups. It samples each material's original
UV tile, including packed UDIM textures, into ordinary 2D maps. Custom skin
scattering, dual specular lobes, coat, anisotropy, refraction and procedural
strand effects are approximated by the engine's standard PBR material.

For future assets, keep `.blend` as the editable source. Bake complex material
graphs into image textures connected to a Principled BSDF and export glTF 2.0
as GLB with materials, skinning and required animations enabled. GLB bundles
standard PBR textures; it does not preserve arbitrary Blender shader graphs.
