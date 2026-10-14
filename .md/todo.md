# Engine roadmap

Last updated: 2026-10-14.

Status convention:

- `[x]` Complete
- `[ ]` Incomplete

## Next priorities
- [ ] **PMX model import:** Implement the staged PMX importer and Properties controls below.

## PMX importer

- [x] **Base PMX decoder:** Read PMX 2.0/2.1 headers, bounded geometry/material/bone records, UTF-8 and UTF-16 names, index widths, and vertex skin weights. Reject truncated files and invalid references. Keep local bone and morph names for MMD keys.
- [x] **Basic model output:** Convert geometry, UVs, normals, base textures, transparency, skeleton hierarchy, linear skinning, and vertex morph offsets through `ImportedModel` and the native asset writer. Reflect Z and triangle winding once.
- [x] **Editor import entry points:** Accept `.pmx` in `ModelImporter`, File > Import, drag-and-drop, Assets Explorer, scene placement, and the headless model import command. Write a prefab, meshes, materials, rig, and supported morph assets.
- [x] **Direct PMX VMD binding:** Selecting an imported PMX prefab when importing a VMD writes body and facial `.anim` clips plus an `.animpack`. Matching PMX bone names receive translated/rotated keys with VMD Bezier curves sampled at up to 120 Hz; matching mesh, bone, and material morph names receive weight tracks. Visibility and matching IK enable keys are carried into the body clip. The saved prefab animation manager loads both clips. Import reports and the Console warn about Bezier sampling limits, unmatched IK switches, and unmatched morph names.
- [x] **Initial regression coverage:** The optional `.local/tests/PmxModelDecoderTests.cpp` target checks the bundled Mococo PMX, nonzero vertex morphs, generated `.morph` files, native asset output with rig constraints, material morphs, follow-bone colliders, opt-in dynamic bodies and all six PMX joint types plus a world anchored joint; the feature switches with base mesh and skeleton retained; synthetic nested mesh/bone/material/impulse group morph import, material playback, impulse application and stop control, bone/material morph controls and animated overrides, ordered append and IK behavior with fixed-axis links and animation switches, local-axis motion basis, external-parent bindings, post-physics append constraints, physics bone feedback, serialized joint frames and impulse response, clip track serialization, saved prefab scale, and gravity-driven motion; and a truncated file. Its CMake target is guarded by source existence.
- [x] **PMX bone behavior implemented:** Weighted append/inherited transforms, limited CCD IK links, manual and VMD IK enable controls, VMD Bezier bone curves sampled at up to 120 Hz, append/IK interleaving by deform layer and original bone order, fixed-axis constraints for IK links and animated rotations, local-axis bases for animated translation and rotation, keyed external-parent bindings to named scene objects in rig Properties, and optional dynamic-body feedback. PMX after-physics append, IK, and external-parent constraints run after the physics step.
- [x] **Bundled Mococo scene physics:** Rebuilt the PMX prefab with all 133 dynamic rigid bodies and 133 joints, retained 24 follow-bone colliders and four IK chains, and rebound the dance VMD. The rendered MMD scene checks those body and joint counts and plays through a full dance and repeated Play resets.
- [x] **PMX 2.1 flip and soft-body milestone:** Flip morphs compose supported child morph channels. Material morphs animate base-texture tint in the object shader. With dynamic physics enabled, triangle-mesh soft bodies create Bullet cloth using PMX mass, stiffness, damping, drag, friction, solver iterations, source pins, and rigid-body-following anchors. Synthetic PMX import and live simulation regression passed. The bundled Mococo prefab now contains the retained texture-tint offsets; its staged dance frames matched the previous render pixel for pixel.
- [x] **Morph channels and playback:** Vertex, primary UV, additional-UV1 XY, bone, material diffuse/alpha/ambient/specular/shininess/base-texture tint, nested groups and flips, plus rigid-body impulses import. Matching VMD morph tracks drive the supported channels. Zero impulse offsets stop velocity. Specular/shininess use a PBR approximation.
- [x] **Linear skinning:** BDEF1/2/4 import with normalized bone weights.
- [x] **Rigid-body and joint import:** Follow-bone colliders preserve shape, pose, filtering, friction, and restitution. Opt-in dynamic bodies drive bones. PMX joint types 0-5 map to Bullet constraints, including world anchors. The bundled Mococo scene contains 133 dynamic bodies and joints and plays without visible detachment or a crash.
- [x] **Triangle soft bodies:** Opt-in PMX cloth imports mass, stiffness, damping, drag, friction, solver iterations, pins, moving rigid-body anchors, and PMX collision group/mask. Synthetic import and runtime tests cover this path.
- [x] **Basic PMX materials:** Base color/texture, alpha, ambient/specular/shininess, shadow casting, and base-texture tint morphs render.
- [x] **Import parameters:** The PMX import dialog has controls for scale, morphs, append/IK constraints, follow-bone colliders, and experimental dynamic rigid bodies. Choices are saved in the generated prefab record and displayed in its Properties view. No reimport.

## PMX remaining features

Implement new runtime behavior through the reusable components below; let the PMX decoder translate file-specific data into them.

### Bone evaluation

- [ ] Compare one MMD reference pose with the imported rig at each deform layer and record the first mismatch.
- [ ] Match animated local-axis and fixed-axis transforms on those reference poses.
- [ ] Verify external-parent targets and after-physics append/IK order with an authored test rig.
- [ ] Compare physics-driven bone feedback over repeated playback and reset.

### Morphs and skinning

- [ ] Import and animate material edge color, opacity, and width offsets.
- [ ] Import and animate material sphere and toon tint offsets, including additive and multiplicative operations.
- [ ] Preserve additional-UV2/3/4 streams in mesh assets and expose their XY morph channels at runtime.
- [ ] Preserve primary and additional UV Z/W values and morph offsets in mesh assets so materials can use all four channels.
- [ ] Verify nested group/flip composition across mesh, bone, material, and impulse targets.
- [ ] Verify repeated material operations and impulse timing against reference playback.
- [ ] Preserve SDEF center and radius data through decoding, mesh assets, and runtime buffers.
- [ ] Implement SDEF deformation and compare reference vertices across animated poses.
- [ ] Implement QDEF dual-quaternion deformation and compare reference vertices across animated poses.

### Soft bodies and materials

- [ ] Build rope soft-body topology from PMX material vertices and render its simulated deformation.
- [ ] Map PMX soft-body cluster settings to Bullet and test a clustered fixture.
- [ ] Implement near-anchor behavior and two-way forces between soft bodies and dynamic rigid bodies.
- [ ] Compare soft-body and rigid-body motion with MMD over playback and reset.
- [ ] Import custom and built-in toon ramps, then evaluate toon lighting in the material shader.
- [ ] Import sphere textures and implement PMX multiply, additive, and additional-UV sphere modes.
- [ ] Render material edge outlines using imported color and width, including skinned meshes.
- [ ] Verify material morph effects on toon, sphere, and edge output with a render fixture.

## Reusable runtime audit and migration

A class used by one bundled scene can still be reusable. Generalize the behavior and API when it is format-independent; keep file decoding and scene choreography in their existing layers.

- [x] **Inventory:** `PmxRigController`, `PmxMaterialMorph`, and `VmdSceneTrack` are format-named runtime components. `AnimationManager`, scene update, and Bullet physics currently refer to PMX component types directly. `Cloth` already exposes reusable soft-body controls. `MmdDanceReplay` is scene-specific and belongs in `Engine/Core/Assets/Scripts/Animation`.
- [ ] Extract append transforms, axis limits, ordered CCD IK, and external-parent bindings from `PmxRigController` into reusable rig constraints under `Engine/Core/Compoonents/Animation`; keep PMX deform-order mapping in the importer.
- [ ] Move physics-to-bone binding and joint descriptors out of `PmxRigController` into reusable animation/physics components; have Bullet consume those components rather than finding a PMX rig.
- [ ] Move bone and impulse morph application from `PmxRigController` into reusable morph controls with named weights and animation inputs.
- [ ] Replace `PmxMaterialMorph` with a reusable material-property morph component covering diffuse, alpha, ambient, specular, shininess, texture tint, and future edge/sphere/toon values.
- [ ] Remove `PmxRigController` and `PmxMaterialMorph` type checks from `AnimationManager` and scene update; dispatch animation weights, IK switches, and pre/post-physics evaluation through reusable component APIs.
- [ ] Convert `VmdSceneTrack` into reusable camera/light animation tracks while keeping VMD frame decoding, coordinate conversion, and Bezier interpretation in the VMD importer.
- [ ] Make `ImportedModel` rig, morph, joint, and soft-body descriptors format-neutral; translate PMX numeric flags and indices in `PmxModelDecoder` before the native asset writer sees them.
- [ ] Replace the `Material.useMmdSpecular` format switch with a named reusable shading model or parameter set; preserve the current PMX appearance.
- [ ] Migrate existing serialized PMX prefabs and clips to the reusable components with backward-compatible type aliases or a versioned conversion; verify Mococo scene playback and non-PMX component authoring.
- [ ] Audit other importer-created runtime components for format-specific names or direct importer dependencies; keep reusable mechanics under `Engine/Core/Compoonents` even if only one bundled scene uses them.
- [x] Keep `PmxModelDecoder`, `MmdDanceAssetBinder`, and `MmdPoseRetargeter` format-specific in `Engine/Editor/Core/Importers`; keep `MmdDanceReplay` as scene choreography in `Engine/Core/Assets/Scripts/Animation`.
