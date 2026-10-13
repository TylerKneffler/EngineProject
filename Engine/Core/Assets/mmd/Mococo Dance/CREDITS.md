# Mococo dance demo credits

Mococo Abyssgard fan model: 3D model by Antro (@antro3dcg); MMD conversion and facial work by Shinigami3D (@Shinigami_3D). Original character and design belong to hololive. See the original model's [ReadMe.txt](../Mococo/ReadMe.txt) for its full credits and terms.

The source motion is `../dance1.vmd`. Its author and source model are not identified in the supplied files; the VMD declares its model as `black`. The conversion to Mococo is therefore a technical demo pending visual review of artistic compatibility.

The scene is `../../Scenes/Animation/mmd.scene`. The body and facial animation assets remain separate and share the original VMD timeline.

The `Mococo Dance.prefab` is the imported FBX used by that scene. Its 56 meshes
share seven skin palettes stored in `Mococo Dance.rig`, which also carries the
body and facial clip associations. The seven textures are reused from
`../Mococo/Textures` so the demo does not store another copy of the images.
The `.meta` sidecars belong to the engine's asset records for each generated asset.
