# Engine roadmap

Last audited against the codebase: 2026-10-12.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

Paint priority: **Required** completes the advertised mesh texture paint workflow or protects its saved results; **Important** improves routine authoring but does not block existing paint modes; **Optional** adds specialized brushes or external-format convenience. Parent items below remain unchecked while any child is open.

## Next priorities

- [ ] **Required, MMD character demo:** Import the Mococo character and `dance1.vmd`, retarget the dance onto a skinned character, and ship a saved `mmd.scene` that plays it. Complete the [MMD import and dance requirements](#mmd-character-import-and-dance-scene) below.
- [ ] **Important, independent material feature:** Add subsurface scattering for skin and other translucent surfaces, with linear-light diffuse scattering, RGB radius, thickness/transmission, and backend/performance checks. See [rendering requirements](skinned-soft-tissue-research.md#separate-feature-subsurface-scattering-and-layered-skin-material).

## MMD character import and dance scene

Complete these steps in order. Source files were inspected on 2026-10-13; engine import and playback are still unverified.

Assets: `Engine/Core/Assets/mmd/Mococo/Mococo (FBX No Outline).fbx`, sibling `Textures/`, `Mococo Abyssgard.pmx` for MMD rig definitions, and `Engine/Core/Assets/mmd/dance1.vmd` for the approximately 10.73-second dance.

The FBX is rigged (269 bones, 55 meshes) and includes facial shapes. Remaining asset issues: weights need normalization, 8,906 vertices exceed eight bone influences, `FacialsExtra_` has no positive skin weights, and textures need assignment. The PMX defines four leg/toe IK chains; five additional IK controls in the motion are absent from this rig.

- [ ] **1. Import the character.** Use the existing FBX importer to generate the prefab, meshes and rig. Confirm all body, finger and accessory bones survive import.
- [ ] **2. Verify deformation.** Check bind poses, mesh transforms, units and facing. Normalize weights, assess eight-influence reduction, and attach or weight `FacialsExtra_` correctly. Rotate representative joints and confirm all meshes follow without distortion.
- [ ] **3. Assign materials.** Bind the supplied textures to the 12 FBX materials; verify hair, eyelashes, alpha and outlines. Save portable paths with correct directory case.
- [ ] **4. Extract the MMD rig profile.** Read or reproducibly extract PMX hierarchy, rest positions, bone axes, inherited transforms and leg/toe IK limits. A full PMX model importer is optional.
- [ ] **5. Parse VMD motion.** Add bounded parsing with CP932-to-UTF-8 names, bone/morph/visibility/IK tracks, and clear errors for malformed files. Safely consume unused camera/light/shadow sections. Handle sparse and duplicate keys; convert frames at 30 fps.
- [ ] **6. Map and retarget bones.** Save a PMX-to-FBX mapping with manual overrides. Convert axes, units and rest-pose offsets; compose root/center/groove and helper transforms correctly. Preserve unanimated bones and choose scene travel or in-place motion. Report unmatched tracks; the original motion source rig is unidentified.
- [ ] **7. Bake the body dance.** Preserve VMD Bezier timing and quaternion interpolation, solve the four leg/toe IK chains, and bake target FK channels within defined error tolerances. Respect IK switches and visibility; report the five unsupported controls. Check feet, knees and root movement without ragdoll/grounded-IK interference. Use deterministic rest/animated accessory poses initially.
- [ ] **8. Add facial animation.** Preserve expression names and mesh ownership through FBX import and mesh serialization. Map VMD expressions to all matching facial meshes; verify default weights and report unsupported expressions/material toggles.
- [ ] **9. Save and expose the animation.** Write `dance1.anim`, link it to the rig and preview prefab, and provide VMD import for a selected character. Save mapping/conversion settings and source metadata; preserve references on reimport and avoid duplicate clips across mesh skins.
- [ ] **10. Build the demo scene.** Save `Engine/Core/Assets/Scenes/Animation/mmd.scene` with Mococo, a floor, lighting and a camera framing the dance. Start `dance1` at time zero on Play; support pause, scrub, speed and restart. Choose looping explicitly and retain package credits.
- [ ] **11. Verify playback and export.** Test the full dance at 30, 60 and variable frame rates, plus scrub/restart, scene reload and play-mode restoration. Check textures, deformation and expressions on DX11/DX12/Vulkan. Export portable dependencies and confirm standalone playback without editor import work.

Add focused parser/conversion checks alongside steps 5-8: malformed files, Japanese names, sparse keys, intermediate Bezier samples, rest-pose conversion and IK switches. Keep existing FBX/glTF import working. Complete the demo before adding full PMX import, toon variants or secondary hair/tail/jacket physics.

Format references: [VMD bone layout](https://github.com/yohawing/maya_mmd_tools/blob/main/mmd_tools/core/vmd_data/bone_frame.py), [VMD curve conversion](https://github.com/mmd-for-unity-proj/mmd-for-unity/blob/master/Editor/MMDLoader/Private/VMDConverter.cs).

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
