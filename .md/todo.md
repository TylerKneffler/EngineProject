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
- [x] **Initial regression coverage:** The optional `.local/tests/PmxModelDecoderTests.cpp` target checks the bundled Mococo PMX, nonzero vertex morphs, generated `.morph` files, native asset output with rig constraints, material morphs, follow-bone colliders, opt-in dynamic bodies and spring joints; the feature switches with base mesh and skeleton retained; synthetic nested mesh/bone/material group morph import and material playback, bone/material morph controls and animated overrides, ordered append and IK behavior with fixed-axis links and animation switches, local-axis motion basis, external-parent bindings, post-physics append constraints, physics bone feedback, serialized joint frames and impulse response, clip track serialization, saved prefab scale, and gravity-driven motion; and a truncated file. Its CMake target is guarded by source existence.
- [x] **PMX bone behavior implemented:** Weighted append/inherited transforms, limited CCD IK links, manual and VMD IK enable controls, VMD Bezier bone curves sampled at up to 120 Hz, append/IK interleaving by deform layer and original bone order, fixed-axis constraints for IK links and animated rotations, local-axis bases for animated translation and rotation, keyed external-parent bindings to named scene objects in rig Properties, and optional dynamic-body feedback. PMX after-physics append, IK, and external-parent constraints run after the physics step.
- [ ] **PMX bone behavior remaining:** Match MMD's full deform timing and verify local-axis motion, fixed-axis projection, external-parent binding, after-physics constraints, and dynamic-body feedback against MMD. External-parent keys need target objects assigned in Properties. Import reports continue to identify the limitations.
- [ ] **Other PMX morphs:** Vertex, primary UV, first additional-UV XY, bone, and material color/alpha/ambient/specular/shininess morphs work. Nested groups composed of these supported types now carry mesh, bone, and material contributions into their matching VMD tracks and component controls. Specular/shininess use a PBR approximation of MMD lighting. Material edge/texture/tint values remain unavailable. Groups with unsupported children, flip, impulse, additional-UV2/3/4, and unused UV Z/W channels are reported when present. Implement the remaining runtime behavior and verify group composition against MMD, including repeated material operations.
- [ ] **PMX skinning modes:** BDEF1/2/4 weights work. SDEF and QDEF use linear weights and are reported when present. Implement their exact deformation.
- [ ] **PMX physics and materials:** Follow-bone rigid bodies become kinematic primitive colliders with shape, pose, collision filtering, friction, and restitution. Opt-in gravity-driven rigid bodies use engine physics and drive their related bones after simulation. PMX joint types 0–5 now map to Bullet spring 6DOF, 6DOF, point, cone twist, slider, and hinge constraints. Joints with unavailable bodies and soft bodies remain unavailable, and dynamic motion is not yet verified against MMD. Base color/texture, transparency, ambient/specular/shininess, and shadow casting work; toon/sphere shading and edge outlines are reported. Implement the remaining runtime mappings.
- [x] **Import parameters:** The PMX import dialog has controls for scale, morphs, append/IK constraints, follow-bone colliders, and experimental dynamic rigid bodies. Choices are saved in the generated prefab record and displayed in its Properties view. No reimport.

