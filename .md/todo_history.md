# History old->new

- [x] **2026-11-08: Build and accept a cheaper intermediate canopy LOD.** Published four coverage-pruned large-oak canopies: **42,498 to 36,297 triangles (14.59% fewer)**, preserving native attributes, silhouette, coverage and pivots. Validated ground/elevated/zoom/motion with **336 matched renderer pairs** across DX11/DX12/Vulkan, including 1080p and the actual installed asset paths; all seven Python checks and four selector CTests pass. Measured 96 projected-size/hysteresis cases and nine paired GPU processes (12,960 frames); full-forest visible primitives fall 3.65-4.18%. Retain **28/90 m**, existing dithering and fixed-band-1 shadows. DX12/Vulkan paired costs are generally lower; DX11 timing is mixed, so no universal GPU/FPS speedup is claimed. Original LOD1 assets remain available; regeneration selects approved canopies. Evidence: [intermediate-canopy-lod.md](intermediate-canopy-lod.md), `.local/canopy-lod-full/completion.json` (`complete=true`).

- [x] Complete texture residency profiling and opt-in conventional mip streaming (2026-11-08). Gather projected UV-footprint demand across instances, views and directional-shadow cascades; budget pinned/fallback/resident/pending/retiring allocations, prefetch finer mips and hold recent demand with pressure overrides. Replace blocking DX12/Vulkan uploads with fence-polled submissions, retain frame owners and recycle descriptors safely. All 12 targeted Release CTests pass. Three backends validate 4,860 uninterrupted turn/teleport/backtracking frames within allocation budgets, 27 settled and 72 transient cutout comparisons (minimum transient IoU 0.996775), offscreen caster demand and 3,600 descriptor replacements with zero runtime scene resource-drain callbacks. The synchronized real-oak regression passes 54 mask comparisons and six supported DX12/Vulkan allocation gates; DX11 dedicated allocation queries remain unavailable. Retain opt-in budgets and defer virtual texturing until conventional streaming has a measured limitation. Evidence: [texture streaming report](texture-streaming-budget.md), `.local/texture-streaming-validation/summary.json`, `.local/texture-streaming-final-ctest.log`, `.local/texture-streaming-oak-regression/summary.json`.

- [x] Complete the material-cost study with independent mip-bias and distant-shading controls, source texture-read counts and shader/material-state inventory. Nine alternating-order Release/DX11/DX12/Vulkan processes at 1920x1080 with shadows validate 972 color and 972 full-cascade shadow samples with matched executed geometry and draws. All 81 still pairs, 540 camera/wind pairs and 27 rendered-shadow pairs preserve alpha coverage; near RGB is unchanged, far simplification is exercised, wind turnover matches and the lighting continuity gate passes. Twenty regression tests pass. Retain authored defaults because the measured candidate benefit is not consistent across APIs. Evidence: `.md/material-costs.md`, `.local/material-study-complete-v2/completion-validation.json` (`complete=true`).

- [x] Complete cold Stop construction follow-up: preserve separate prefab-baseline and immutable-publication snapshots; copy resolved material values/texture owners into independent batch components; cache one completed DX12 command bundle per factory with exclusive leases and fence-checked reuse. Six matched-input Release/DX12 captures show cold rebuild median **1082.730 to 870.159 ms**, material work **201.612 to 16.729 ms**, and command recording **24.316 to 4.230 ms**; all 85 cold uploads reuse completed bundles. Full stopped-frame median is 5.3% lower but ranges overlap; callback remains roughly unchanged. Add `play_stop` tagging through delayed intervals and keep polling separate. Release Editor/regression targets and Debug regression targets build; 13 selected Release checks and three Debug checks pass, including concurrent GPU byte readback and GPU validation. Preserve the initial relink/file-lock diagnostic and Debug 60-second CTest timeout; clean relinked-target and direct 123.717-second Debug forest reruns pass. Details: `.md/forest-performance.md`, `.local/performance-logs/cold-followup-comparison.json`.

- [x] Complete the three-backend texture sampling audit, including continuous camera/wind sequences and isolated rendered-shadow controls. Fix the audit harness to wait for GPU completion before reading capture memory; discard earlier unsynchronized results. Final Debug checks pass 162 static mask pairs, 1,080 motion pairs, 54 normal-incidence shadow pairs and 108 static-caster wind comparisons. Record the anisotropy mismatch (DX11 8x, DX12/Vulkan trilinear), alpha-mip coverage limits and sampling-versus-residency distinction. A synchronized 45-degree sun stress case fails shadow presence at the far 80-degree view on every backend, including the opaque control; preserve its reproducer and captures. Retain color/shadow alpha cutouts and runtime defaults. Evidence: [texture sampling audit](texture-sampling-audit.md), `.local/texture-sampling*/`, and validation/build logs.

- [x] Attribute cold Stop serialization callers and split worker XML conversion and batch material costs. Two grass calls serve the prefab baseline and immutable scene publication; the third publishes 2,048 tree placements. Preserve both snapshots and ownership. Reserve XML object members using the existing classification scan; matched-input conversion samples improve but total Stop speedup is not established. Final batch material deserialization is 189.231 ms / 340 calls versus 9.426 ms serialization and 0.125 ms graphics hook; investigate reconstruction before deferred upload batching. Three captures retain identical non-executable inputs/settings, and final Release Editor/Oak builds and six targeted checks pass. No Debug validation or steady FPS gain claimed. Evidence: `.local/performance-logs/cold-stop-investigation.json`, `.local/cold-stop-final-release-tests.log`, and `.md/forest-performance.md`.

- [x] **2026-11-07: Generalize screen-size mesh LOD.** Added serialized opt-in mesh/layer policy, projected conservative bounds, per-camera/per-placement hysteresis, distance overrides, GPU-culling integration, CPU fallback and scripted-grass propagation. Shadows retain independent distance/fixed-proxy selection. Release Engine/ProjectScripts builds, numerical checks, DX11/DX12/Vulkan geometry and forest tests, separate-camera/instance-set regressions, and paired ground/elevated/zoom/moving captures at 0.5/1/2 resolution pass. Illustrative pixel thresholds visibly change elevated canopy appearance, so retain the authored forest 28/90 m transitions and fixed LOD1 shadows. Evidence and rollout limits: [screen-mesh-lod.md](screen-mesh-lod.md); final captures: `.local/screen-lod-capture-final-tests.log` and `.local/screen-lod-validation/`.

- [x] Complete stable-layout capture support and matched placement-object validation. Nine identity tests and six Release/DX12 1080p captures pass; 54 non-executable inputs, graphics settings and profile call counts match. Placement-record medians: initial grass **326.499 to 244.781 ms**, Stop aggregate **580.805 to 491.792 ms**. Callback timing also varies with parsing; no exclusive callback attribution or steady FPS claim. Exact layout snapshots retained, default workspace layout behavior preserved, and optimized source/final Editor restored after the baseline build. Evidence: `.local/performance-logs/capacity-fixed-comparison.json`, manifests, stage logs and `.local/fixed-layout-ctest.log`.

- [x] Add an empty-object member-capacity hint and use six slots for placement JSON records only. Preserve default factories, values/order, replacement/erase, deep-copy/move ownership and growth beyond the hint. Paired 65,536-record benchmark, seven alternating measured samples: Release construction median **64.960 to 59.370 ms**, Debug **1362.710 to 1019.150 ms**. Both Editors and JSON compatibility tests pass; shared-prefab regression passes in Release and on the Debug diagnostic rerun. First Debug run hit an existing grass-accumulation assertion; enhanced diagnostics and 1,000 isolated checks per configuration did not reproduce it, so no production grass fix is claimed. Full capture comparison is rejected: scene/prefab identities match, but the user layout seed changed across runs. Preserve layout/grass assets; no matched Stop-time or FPS gain claimed. Evidence: `.local/json-object-capacity-*-benchmark.log`, validation logs and `.local/performance-logs/json-object-capacity-comparison.json` (`comparison_valid=false`).

- [x] Add bulk placement snapshot prepared/record/total phases and restore signature/bake scopes. Reuse the freshly normalized local signature for baked-data validation, avoiding a second ID/hash pass on baked layers while retaining validation and edit detection. Current grass asset has 324,147 placements and no authored bake, so do not attribute whole Stop differences to this candidate. Matched-input Release pair retains all 54 non-executable fingerprinted identities: Stop signature 64.769 ms, baked load 1.165 ms; placement record construction aggregates 635.810 ms across three serialization calls (not solely the source snapshot). Regression covers normalized IDs, valid/stale/corrupt bakes and legacy fallback; Debug/Release Editors build. Evidence: `.local/performance-logs/placement-signature-comparison.json`, asset-shape JSON, capture/stage logs and `.local/placement-signature-*-test.log`. Grass asset preserved; no steady FPS or isolated timing gain claimed.

- [x] Parse the existing mutable prefab file buffer in place with pugixml instead of copying it into a second native-encoding parser buffer. Preserve JSON fallback, encoding conversion, independent JSON string ownership and source file bytes. Extend format coverage with UTF-16 BE, UTF-32 LE and entity decoding. Debug/Release Editors build; prefab/cache validation logs use `.local/xml-inplace-*`. Short matched-input Release/DX12 1080p pair: Stop read/parse **852.597 to 822.980 ms**, callback **2355.69 to 2278.89 ms**. All 54 non-executable fingerprinted inputs match; current 113,905,987-byte UTF-8 grass prefab is preserved. One pair, overlapping scopes, no steady FPS claim. Evidence: `.local/performance-logs/xml-inplace-comparison.json` and capture/input/stage logs. Remaining value-tree and batch work stays open.

- [x] **Particle and deformation jobs:** Parallelize independent ballistic and fluid particle updates, soft tissue preparation, CPU skin/morph fallback, portal mesh deformation, particle render data, and nonlinear warp sampling from copied volume definitions. Collect bounded particle collision sweeps on the simulation owner against one stable physics world before applying responses. Particle and fluid regressions, a 2,048-point warp snapshot comparison, serial execution, and the Debug build pass.
- [x] **Terrain seam normals:** The first distant cell now copies the adjoining detailed cell's baked surface normal at every shared 4 m edge sample, including saved far patches. The regression checks referenced vertices on all four edges. The high-view 1080p capture changed 6,335 pixels by at most five 8-bit levels; the coarse-to-textured color transition remains open.
- [x] **Shared-grid submission evaluation:** Existing 4x4 batching remains the measured choice. At 1080p the latest low-view sweep has 60 draws, 12.1 ms GPU p95, and 12.9 ms render CPU p95; conservative occlusion rejected no whole draws. Halving far-tile width raised draws to 107 and frame p95 to 18.3 ms in adjacent runs. Instancing or indirect patch submission needs a demonstrated CPU submission or visibility win before replacing the batches.
- [x] **PMX authored rig and physics reset:** Extend the synthetic rig fixture to check an external-parent target after physics, post-physics IK before a later-layer append, and a simulated body driving its bone. Reset physics and restart the same scene, then verify that bone/body alignment and the sampled playback height repeat. The Debug `PmxModelDecoder` test passes.
- [x] **PMX material edge outlines:** Correct the outline shader's object-buffer layout so it reads imported edge color and width, and apply morph normals before skinning and extrusion. All Vulkan outline shader variants compile; a visual render audit remains open under material morph verification.
- [x] **PMX rope soft bodies:** Import rope topology from the selected material's welded mesh vertices, build Bullet links without faces, and update the rendered mesh from simulated nodes. A synthetic rope fixture checks links, wind deformation, anchors, and reset. Rope mass uses per-node distribution to avoid Bullet's face-area assertion; transient render updates no longer rebuild the body every frame.
- [x] **PMX sphere textures and modes:** Decode BMP-backed `.sph` and `.spa` textures, retain the PMX sphere mode through native material import, and shade multiply, additive, and additional-UV1 modes. Normal-based mapping now uses camera view rotation without projection distortion, including portal views. Synthetic imports verify all modes and copied textures; a DX11 visual audit confirms distinct rendered output for the three modes.
- [x] **Reusable rig components:** Split ordered append, axis, CCD IK, and external-parent evaluation into `RigConstraints`; physics bone feedback and joint descriptors into `RigPhysicsBinding`; and named bone/impulse morph controls into `RigMorphControls`. The PMX writer and bundled Mococo prefab use the new components, Bullet reads joint descriptors from the physics binding, and old serialized `RigController` data migrates on load. The Debug Editor builds; PMX decoder/import, runtime migration, and VMD scene import tests pass.
- [x] **PMX full UV channels:** Preserve four components for primary UV and additional UV1-4 in native mesh assets. Import UV morphs for every set, including Z/W, and apply them in nested group morphs, CPU posed vertices, and GPU morph buffers. Expose the five sets to the general object shader. Native mesh/morph readers retain compatibility with older versions; a focused PMX fixture verifies save, reload, and animation offsets.
- [x] **PMX material morph offsets:** Import edge color, opacity, and width plus sphere and toon tints; apply additive and multiplicative morph operations during animation. Pass the resulting values to the general material renderer for sphere, toon, and outline output. Decoder and DX11 scene checks pass; exact MMD appearance remains under the separate material verification tasks.
- [x] **Persistent Bones toggle:** Save the header/viewport bone visibility preference in editor `imgui.ini` settings, restore it at startup and apply it to every newly opened Scene View. Both controls update the same preference and schedule automatic saving without changing scene or rig assets. A disk round-trip test verifies hidden/visible states across fresh editor contexts, synchronization of new views and defaults for old/invalid settings. The Debug Editor build and focused persistence test pass.
- [x] **One Mococo skeleton:** Consolidate 11 mesh-binding palettes into one shared Skeleton with 205 unique joints across all 56 meshes. Remap vertex influences and store each mesh's bind transform separately; retain bone parentage, materials, morphs, animation clips, replay script and asset IDs. FBX imports merge palettes sharing joints while keeping independent armatures separate. Compare every posed vertex before/after at frames 0, 120 and 322 before publishing staged assets; maximum difference is 0.000000482 meters. Import, prefab reload, saved-scene/replay and multi-rig writer tests pass, as do DX11 rendering and Play restores. The Debug Editor builds. Bone groups were left out at the user's request.
- [x] **3D header tool icons:** Replace Hand/Move/Rotate/Scale text controls with compact vector icons, tooltips and an active-tool highlight. Add icons to tool settings, bind-pose and Animation Editor actions. Keep tool labels and text fallbacks for menu actions, and size the header sections for the icon controls. The Debug Editor build passes.
- [x] **MMD dance replay script:** Attach `MmdDanceReplay` to the saved MMD scene and include it in scene regeneration. When the selected body clip reaches its end or playback stops, restart its clock and all facial-layer clocks together and immediately apply the opening pose. The script is included in the Editor and reloadable project module. Saved-scene tests verify three repeats at both 30 and 60 fps and recovery from stopped playback; DX11 rendering and Play/restore tests pass.
- [x] **Mococo texture orientation:** Correct FBX UV0/UV1 from Assimp's bottom-left origin to the engine's top-left image origin and reverse tangent handedness for the reflected V axis. A rendered comparison confirmed that reversed UVs caused the missing blue eyes and misplaced clothing, skin, stockings and shoe details. Refresh all 56 existing native meshes without changing prefab/rig/animation references or asset IDs. An independent source comparison verifies all 186,842 triangles, material slots and converted UVs; the saved scene verifies every mesh's UVs and texture/alpha binding. Prefab reload, saved-scene animation, full-dance DX11 rendering and three Play/restore cycles pass; the Debug Editor builds.
- [x] **Scene View header and bone visibility:** Restore transform, grid, render-mode and UI controls inside Scene View, aligned with the orientation cube, even when the global toolbar is enabled. Add a synchronized Bones toggle in the viewport and global View header. Hiding bones suppresses their overlays and picking in Object mode and their extra overlays in Skeleton Edit/Weight Paint, while keeping authored rig settings intact. Reserve space for the cube in narrow panels. The Debug Editor build passes.
- [x] **Mococo units and front camera:** Read FBX `UnitScaleFactor` and convert to engine meters once at the prefab root, keeping source mesh, bone and clip coordinates intact. Mococo uses a 0.01 root scale automatically on import; the scene uses that same absolute root scale, without a second conversion. Place the camera at `(0, 0.95, 2.7)` looking toward `(0, 0.75, 0)` so the standing character faces it. Prefab reload and saved-scene motion checks pass.
- [x] **Mococo rendered playback and memory reduction:** A hidden DX11 test renders samples across the full dance, restores the saved scene and repeats Play/rendering three times. Static morph deltas use GPU-only DX11 buffers, removing 402 MiB of upload shadows while facial weights remain writable. Other backends retain their initialized upload-buffer fallback. Private memory after scene load fell from about 2.8 GiB to 2.3 GiB. The Debug Editor builds successfully; the originally reported Editor crash did not reproduce, so its diagnosis remains the first open TODO.
- [x] **Mococo clean scene rebuild:** Removed the redundant `Assets/Mococo (FBX No Outline)` import and regenerated `mmd/Mococo Dance` from the corrected FBX and original `dance1.vmd`. The scene uses one fresh prefab with 56 meshes, 11 skin palettes, separate body/facial clips, and a camera framing the standing character. The IK bake now updates only affected parent/inheritance dependencies after each correction; comparison with full-rig evaluation produces identical source matrices at the checked frames. `MMD_REBUILD_DEMO=1` on the import test tool reproduces asset and scene generation without running unrelated regression loops.
- [x] **Mococo FBX transform correction:** Preserve authored mesh scale, rotation and translation. The previous scale-100 heuristic removed the transforms that assemble the meter/Z-up meshes onto the centimeter/Y-up rig, leaving body parts separated and the character face down. Regression checks now compare every decoded node against the source FBX before validating prefab save/reload and skinned vertices. Existing character and dance prefabs were repaired with per-mesh bind corrections while retaining their rig, material, morph and animation references. Rendered appearance still needs visual review.
- [x] **Required, animation-pack memory peak:** Memory checkpoints showed that the ordered test retained several full prefab scenes and clip copies across later pack checks. Scoping those checks and releasing binder clip copies before rig refresh reduced the measured process peak from about 1.8 GiB to 976 MiB for the full Mococo steps 1-10 run. A separate headless saved-scene, raw-FBX/VMD, and pack round-trip run peaked at 883 MiB. Both pack load and save completed without raising that isolated peak.
- [x] **Required, saved MMD scene headless comparison:** A regression loads `mmd.scene`, starts its imported character, and compares sampled body transforms, all four facial meshes, and visibility against freshly decoded `dance1.vmd` and FBX data. Across frames 0, 30, 60.5, 120, 240.5, and 322, maximum observed differences were 0.0000002 position units, 0.0007 radians, and zero facial weight; playback reached frame 322 at both 30 and 60 fps. The test also loads, saves, and reloads the animation pack. The FBX decoder now gives morph targets stable order and neutral default weights, and the saved morph/facial assets were refreshed to match. Camera/light VMD behavior remains in the scene-fidelity roadmap item because this VMD has no such keys.
- [x] **Required, MMD FBX bind-transform prerequisite:** Attach `FacialsExtra_` to the head, preserve authored mesh transforms, resolve duplicate bone names against the skin bind transform, and rebuild inverse binds from runtime TRS rest transforms. The ordered test passes facial and sampled whole-character bind checks plus head, finger and tail joint-motion checks. Runtime rendering remains a manual review.
- [x] **Required, MMD material mapping prerequisite:** An adjacent FBX material sidecar maps all 7 decoded materials to the supplied sibling textures with correctly cased relative paths and blended facial alpha. The ordered test passes material mapping; imported-prefab and renderer checks remain in MMD step 3 and manual review. The separate outline textures are unused by the supplied no-outline FBX.
- [x] **Required, MMD PMX rig profile prerequisite:** `Scripts/extract_pmx_rig.py` reproducibly extracts `Mococo.mmdrig.json` from the PMX. The profile records 239 bones, global and parent-relative rest positions, axes, inherited transforms, and four leg/toe IK chains with limits. The ordered test validates the profile and a separate test checks exact regeneration.
- [x] **Required, MMD VMD parser prerequisite:** `VmdMotionDecoder` parses CP932 names, bone/morph/visibility/IK and camera/light/shadow tracks with bounded counts, 30 fps timing, last-key-wins duplicate handling and clear malformed-file errors. The focused test covers Japanese names, sparse keys, duplicate keys and nonzero camera/light/shadow frames. The ordered test parses `dance1.vmd` through frame 322.
- [x] **Required, MMD retarget mapping prerequisite:** `Mococo.mmdmap.json` maps all 239 PMX bones to decoded FBX nodes: 214 rest-position matches within 0.01 world units and 25 reviewed root/twist/IK/helper mappings. It records the measured PMX-to-FBX world conversion (scale 8, Z sign flip), root composition order, rest-pose policy, nine motion-name aliases and an explicit report of 705 unmatched VMD bone tracks (none with more than two keys). The ordered test validates names, indices, rest positions, conversion metadata and unmatched-track counts. `Scripts/generate_mmd_map.py` regenerates the map from the FBX node audit. The motion declares its source model as `black`; compatibility with Mococo remains unverified.
- [x] **Required, MMD rest-pose retarget conversion:** `MmdPoseRetargeter` evaluates PMX/VMD key poses through the saved map, applies measured scale/axis conversion and target bind-pose corrections, composes all-parent/center/groove/waist/lower-body scene travel, and preserves unmapped FBX local transforms. It evaluates inherited rotation/translation in PMX space, including shoulder and waist cancellation and weighted eye/twist helpers. The ordered test verifies the full neutral bind matrix, five-control root translation, synthetic helper poses on source and FBX joints, representative real limb/torso movement and frame-120 travel. Helper keys without distinct FBX targets must remain within a near-neutral tolerance; animated ones fail clearly. IK controls and VMD Bezier sampling are covered by step 7. The source motion declares model `black`, so final visual compatibility remains unverified.
- [x] **Required, MMD motion-to-rig IK reach adaptation:** `MmdPoseRetargeter` projects unreachable leg/toe goals onto each chain's physical reach while preserving scene travel and reporting the discarded target distance. A bounded extra toe refinement handles projected one-link goals. Across frames 0-322, target drift stays under 0.605 PMX units, error to original VMD goals under 0.699, and error to projected goals under 0.125. The ordered test checks all six leg/toe segment lengths and knee hinge angles, plus a synthetic IK off switch. This is an explicit contact-drift policy for the motion's `black` source rig, which has goals beyond Mococo's reach.
- [x] **Required, MMD body dance bake:** `MmdPoseRetargeter::BakeBody` produces 55 independent 120 Hz target FK channels with portable node paths and continuous quaternion signs. The ordered test measured at most 0.0021 world-unit translation error and 0.0465-radian rotation error between samples, verified IK switches and reach adaptation, and reported five unsupported source IK controls. Clip visibility keys hide character meshes without stopping animation updates; a focused playback and serialization test passes. Body motion stays separate from camera/light/facial tracks. The clip is saved in step 9; rendered playback is a manual review.
- [x] **Required, MMD facial animation prerequisite:** FBX expression names survive mesh and prefab serialization. Duplicate `Body_` primitives get distinct stable scene paths. The facial bake maps 40 VMD expression names to 160 targets across four meshes, emits independent mesh-specific weight channels, and reports 94 unmatched source expressions, including names with no corresponding shape or material behavior. A focused test verifies independent facial-layer playback alongside body movement and reset on disable. The ordered test ends after the saved demo scene check.
- [x] **Required, full Mococo prefab import memory blocker:** Imported morph geometry now lives in binary `.morph` assets and the prefab stores references and weights instead of expanding millions of numbers into XML. The FBX importer shares matching joint palettes and bind matrices; the 56 Mococo meshes use seven skin sets in one `.rig` asset, with no duplicate source textures in the generated demo. All 56 mesh reloads, four facial morph mesh reloads, and prefab/rig binding pass in the ordered test. The observed test process stayed below 1 GB during scene reload; rendered appearance remains a manual review.
- [x] **Required, MMD dance asset binding:** `MmdDanceAssetBinder` saves separate body and facial `.anim` assets and a self-contained `.animpack`, adds available clips to every rig, configures a body clip and facial layer on the prefab, records map/profile/source and unsupported expressions, and updates the assets without duplicate rig paths or layers. The selected-character VMD import flow and pack round-trip pass in the ordered test. Independent application of raw character and scene tracks remains in the roadmap.
- [x] **Required, independent VMD camera/light scene tracks:** The decoder selects typed tracks on their original 30 fps timeline and sorts camera/light/shadow keys with last-key-wins duplicates. `.vmd` import returns a motion-only `.animpack` and creates separate optional camera and light prefabs; explicit prefab mode also saves a combined camera/light scene prefab. Each uses `VmdSceneTrack` for independent play, pause, seek and restart. Camera distance/orbit, position, rotation, FOV and projection, and light position/color survive prefab reload. The source VMD is copied into the package. A mixed-track prefab regression verifies that seeking one track does not move the other. Camera interpolation uses the six VMD Bezier channels; light keys interpolate linearly.
- [x] **Required, source VMD animation pack:** Every VMD import writes a self-contained `.animpack` with each present Body, Facial, Camera, Light, Shadow, and Properties source track on its original timeline, even when no character is selected. Camera and light also remain independent prefabs and `.vmdtrack` assets. A source track can be extracted from the pack as a `.vmdtrack`; selected-character import also adds retargeted body/facial clips to a character pack while retaining source tracks. Binding extracted tracks to editable components remains in the roadmap's manual VMD track application task.
- [x] **Important, FBX scene import completeness:** The FBX decoder retains the complete node hierarchy, separate mesh objects, skins and all Assimp animation clips. It now maps Assimp cameras and point/directional/spot lights to child objects under their animated source nodes in the generated prefab, preserving camera projection and light color/cones where engine components have matching fields. Animation assets are written once per clip and shared by all mesh skins. A prefab save/reload regression covers multiple objects, camera, spot light, two skins and two clips.
- [x] **4. Extract the MMD rig profile.** Read or reproducibly extract PMX hierarchy, rest positions, bone axes, inherited transforms and leg/toe IK limits. A full PMX model importer is optional.
- [x] **5. Parse VMD motion.** Add bounded parsing with CP932-to-UTF-8 names, bone/morph/visibility/IK tracks, and clear errors for malformed files. Preserve separately typed camera/light/shadow tracks for later independent scene binding. Handle sparse and duplicate keys; convert frames at 30 fps. Manual source-track application and scene fidelity remain open in the roadmap.
- [x] **6. Map and retarget bones.** Save a PMX-to-FBX mapping with manual overrides. Convert axes, units and rest-pose offsets; compose root/center/groove and inherited helper transforms. Preserve unanimated bones, use scene-travel root motion, and report unmatched tracks. The original motion source rig remains unidentified; visual compatibility remains a manual review.
- [x] **7. Bake the body dance.** VMD bone Bezier sampling and a four-chain PMX IK pass with link limits and IK switches produce 55 independent 120 Hz FK channels. Unreachable foot/toe goals are projected onto chain reach with under 0.75 PMX-unit contact drift. The ordered test checks leg/toe lengths, knee limits, root travel, IK switches, five unsupported source controls, quaternion continuity and between-sample reconstruction below 0.1 world unit / 0.1 radian; measured maxima are 0.0021 / 0.0465. Character visibility keys serialize and play while animation continues. Accessory bones retain deterministic rest or mapped motion; clip saving follows in step 9 and rendered playback is a manual review.
- [x] **8. Add facial animation.** FBX expression names and mesh ownership survive import, mesh serialization and prefab reload. The VMD facial bake maps 40 names to 160 targets across four meshes, with independent mesh-specific channels that play as a layer alongside body motion. A focused playback test verifies default-weight restoration. It reports 94 unmatched source expressions; material toggles without matching engine shape/material behavior remain unsupported.
- [x] **9. Save and expose the animation.** `dance1_body.anim` and `dance1_facial.anim` ship with the full character prefab, are linked through every rig, and survive repeated binding without duplicates. Selecting an imported FBX character before importing VMD resolves its matching map and profile, writes separate body and facial `.anim` assets plus a stacked `.animpack`, binds them for playback, and refreshes scene instances. The ordered test verifies metadata-driven binding and pack load/apply. AnimationManager exports and imports self-contained packs through its Properties controls; packs can also be dropped onto an editable model object. Normal AnimationManager controls handle pause, scrub, speed and restart.
- [x] **High, two-bone IK controls:** Add a reusable arm/leg solver with a hand or foot target, elbow or knee pole target, position and rotation weights, target offset, and stable behavior near full extension. Expose and manipulate its controls in the Animation Editor.
- [x] **High, matched FK/IK switching:** Let each limb switch or blend between keyed joint rotations and IK targets. Snap the incoming controls to the evaluated pose before switching so the limb does not jump; key the switch and bake the result to ordinary bone channels for export.
- [x] **High, viewport rig controls and presets:** Show selectable root, pelvis, limb target, and pole handles. Offer a humanoid setup preset that binds controls to an existing skeleton while allowing manual mapping for other rigs.
- [x] **Medium, aim and transform constraints:** Add weighted look-at/aim, parent, position, and rotation controls for heads, eyes, weapons, and hand-held props. Define evaluation order and prevent dependency cycles.
- [x] **Medium, pose library and symmetry:** Save and apply whole-rig or selected-control poses; blend a saved pose with the current pose, mirror left/right using an editable bone map, and optionally key the result.
- [x] **Later, control space switching:** Key changes between local, world, body, and prop spaces while preserving the control's world pose at the switch frame. The Animation Editor exposes space buttons for limb IK controls and a scene-object picker for prop references.
- [x] **Later, chain and full-body IK:** Support longer chains and multiple effectors for spines, tails, reaching, and terrain adaptation after the two-bone workflow is stable.
Reference workflows: [Unity Animation Rigging constraints](https://docs.unity.cn/Packages/com.unity.animation.rigging%406.6/manual/ConstraintComponents.html), [Maya IK/FK blending and matching](https://help.autodesk.com/cloudhelp/2022/ENU/Maya-Animation/files/GUID-C5ACDDBC-9E30-443C-8419-F07FC41E9A86.htm), [Unreal Control Rig](https://dev.epicgames.com/documentation/unreal-engine/control-rig-in-unreal-engine), [Unreal pose tool](https://dev.epicgames.com/documentation/unreal-engine/animation-editor-mode-in-unreal-engine?lang=en-US).
- [x] Cache compatible clip mappings and per-bone key times, and cull timeline rows and key markers outside the visible area.
- [x] Reuse preview clips during playback and scrubbing, updating playhead times and layer weights in place.
- [x] Filter cached rig assets while searching; rescan after asset changes or an explicit refresh.
- [x] Commit clip-duration drags as one undo entry when the drag ends.

- [x] C++17 engine library, standalone game runtime, editor application, project files, project loading, project hub, standalone builds, portable exports, versioned Windows packages, and quick-start documentation.
- [x] Scene objects, transforms, reusable components, lifecycle dispatch, scene serialization/deserialization, synchronous and asynchronous scene loading, scene switching, and play-mode scene restoration.
- [x] Game, Scene, Hierarchy, Properties, Assets, Console, Preferences, Terminal, and Problems views with dynamically managed panels, persistent ImGui layouts, and a package-neutral editor UI facade.
- [x] Editor camera controls, object picking, hierarchy creation/deletion/reparenting, drag-and-drop, prefab workflows, component workflows, and configurable undo/redo history.
- [x] Hierarchy Ctrl-toggle and Shift-range multi-selection. The Properties view shows common components, displays mixed values as `-`, and broadcasts edits to every selected object.
- [x] Asset records with source paths, stable IDs, import settings, browser assignment workflows, and throttled/filesystem-notified script refresh instead of recursive scanning every frame.
- [x] DX11, DX12, and Vulkan renderer selection for editor and game hosts, including startup probing and DX11 editor fallback.
- [x] One deterministic scene/export clock now drives gameplay, skeletal and sprite animation, physics, cloth, camera tracks, moving portals, controller movement, and audio transport. Export-relevant
- [x] glTF import through the engine model representation, including skinning, animation, morph targets, vertex colors, UV1, and FBX conversion into the same representation.
- [x] Materials and PBR inputs, alpha modes, normal/height/occlusion/emissive textures, HDR/EXR/TGA/KTX2 processing, mip generation, and corrected advertised texture support.
- [x] Semantic texture color spaces: base-color and emissive textures use sRGB; metallic/roughness, normal, height, occlusion, light/environment data remain linear on DX11, DX12, and Vulkan.
- [x] GPU morph blending with persistent delta/weight buffers and conservative bounds on all three renderers.
- [x] Cached animation-node/joint bindings, reusable pose/sample/palette storage, revision-aware skeleton bindings, and dynamically growing ordinary/skinned/bone buffers instead of fixed 512/64 limits.
- [x] 2D scene editing, sprites, sprite sheets, sprite animation, retained UI layout, UIImage modes, and runtime TTF/OTF text.
- [x] WAV/OGG/MP3 decoding, audio components, mixing, and spatial audio integration.
- [x] Rigid bodies, primitive and mesh colliders, cloth/soft-body simulation, cached cloth meshes, component revision-based rebuilds, and physics/editor transform synchronization.
- [x] Dirty-driven editor rendering, scene edit revisions, cached transform/world matrices, shared per-frame Scene/Game render discovery, precomputed render keys, and cached component-picker entries.
- [x] Persistent and range-bounded light/object/bone uploads, DX11 persistent constant-buffer arena support, deferred-safe DX12/Vulkan growth, and used-range object uploads separated from portal capacity.
- [x] Packed indexed terrain vertices with separate CPU, upload-shadow, and GPU memory reporting.
- [x] Non-blocking frustum/occlusion behavior on DX11, DX12, and Vulkan, including deferred query recycling and conservative fallback when results are unavailable.
- [x] Non-blocking GPU timestamp telemetry for terrain, portal, opaque, and transparent stages on DX11, DX12, and Vulkan, with CPU submission/presentation reported separately.
- [x] Central cache storage used by textures and reusable simulation resources; Vulkan texture-system shutdown drains GPU work and invalidates wrappers before device destruction.
- [x] Scene lighting renders to linear FP16 targets on DX11, DX12, and Vulkan. A dedicated fullscreen composition pass applies exposure, tone mapping, and final sRGB encoding for game output and editor views.
- [x] Symmetric portal connection/disable handling, canonical validated aperture frames, source-to-target relative transforms, bidirectional crossing, parity-aware pose mapping, and rigid-body velocity/orientation remapping.
- [x] Post-physics swept traversal with aperture contact tests, anti-jitter traversal state, deterministic overlapping-volume ownership, and matrix-overlay ownership/priority rules.
- [x] Multiple stencil-isolated portal views, recursive views with cycle limits, scissoring, target-plane clipping, mapped cameras, and connection validation.
- [x] Persistent local/remote split render instances, separate collision pieces, welded cap generation, restoration, and bounded rebuild behavior.
- [x] Unified rendering/physics/raycast/audio/camera/gameplay spatial-query contract and finite recursive portal-ray segmentation used by editor picking.
- [x] Gate portal teleportation with a swept collision-shape-versus-inset-aperture test. Use the solid rim as physical feedback instead of testing only the rigid-body centre.
- [x] Define explicit portal content-region ownership through a scene layer, scope root, or spatial chart, then restrict rendering and queries to the connected region.
- [x] Directional light replacement for the legacy Ambient type with backward-compatible serialization.
- [x] Serialized per-light shadow controls and project-wide shadow/cascade/PCF/distance/portal policies exposed in the editor.
- [x] Reusable one-to-four-cascade directional shadow atlas with practical splits, transition blending, texel snapping, tile-safe PCF, receiver-normal bias, and far-distance fade.
- [x] Matching DX11, DX12, and Vulkan depth targets, transitions, descriptors, depth passes, resource reuse, and deferred-safe shadow draw data.
- [x] Shadow casters support static meshes, alpha masks, double-sided materials, GPU skinning, GPU morphs, terrain, matrix layers, spatial mappings, and portal-view atlas policies.
- [x] Deterministic directional-shadow selection and GPU data for shadow indices, cascade matrices/splits, bias, filter scale, and shadow strength.
- [x] CPU-built Forward+ clustered light lists for main views with configurable tiles, logarithmic depth slices, caps, matching shader consumption, and safe global-list portal fallback.
- [x] GGX importance-filtered HDR environment mip generation and a generated split-sum BRDF integration LUT across DX11, DX12, and Vulkan.
- [x] Serialized None/ACES/Reinhard output operators and exposure applied consistently in the current material/sky shader path.
- [x] Fixed-timestep DX11/DX12/Vulkan GPU image tests for directional shadows, alpha masks, skinning, morphs, terrain, moving casters, and bounded backend parity. Tests wait for terrain generation and skip when a required backend is unavailable.
- [x] Add deterministic light importance sorting for global/portal fallback lists so irrelevant first-in-scene lights cannot displace visible lights.
- [x] Direct-to-FFmpeg DX11 export for MP4, WebM, and MOV with fixed output rate, fixed scene update delta, configurable dimensions/duration/codec/quality, camera-track completion, and synchronous RGBA capture.
- [x] The exporter configures the scene clock before `Start()` and advances integer-indexed fixed frames; animation, sprite animation, first-person control, moving portals, physics, cloth, camera tracks, gameplay, and audio consume that clock.
- [x] Replace linear animation-layer node-mask searches with indexed bitsets and share compatible channel samples between layers.
- [x] Remove duplicate per-frame light collection/upload and retain reusable scene scratch arrays, opaque sort storage, component lookup results, and no-portal fast paths.
- [x] Replace the policy-only portal parity test with actual DX11, DX12, and Vulkan GPU-frame validation covering aperture depth/stencil isolation, recursive views, occlusion, and distinct target-side content without a skybox fallback.
- [x] Expose a Bullet-backed portal raycast-hit API and test regular objects, split pieces, aperture rims, and recursive portal hits.
- [x] Add bounded adaptive nonlinear-warp ray paths for gameplay/physics; a single origin/tangent Jacobian cannot represent curved paths or volume-boundary crossings.
- [x] Support indexed meshes, submeshes/material slots, skinned meshes, and morph changes in CPU portal cuts. The current triangle-stream clipper cannot preserve all imported topology.
- [x] Define ownership of scripts, joints, children, animation, and serialization when a connection disappears while an object remains split.
- [x] Define realtime/baked mixed-lighting behavior so static receivers do not double-shadow while dynamic objects retain realtime shadows.
- [x] Add shadow debug views for atlases, cascades, bounds, selected lights, caster counts, and occupancy in the Performance window.
- [x] Add visual regressions for shadow bias, cascade seams, shimmer, far fade, thin/reversed geometry, large coordinates, portals, and spatially mapped casters/lights.
- [x] Stress maximum realtime lights and shadow budgets; verify deterministic degradation without stalls or per-frame resource churn.
- [x] Add spot lights with range attenuation, inner/outer cone angles, serialization, editor gizmos, PBR evaluation, and shadow policy.
- [x] Define physical light units and inverse-square attenuation with a smooth range cutoff and compatibility mode.
- [x] Add per-object light layers/channel masks.
- [x] Add directional/spot cookies and optional IES profiles.
- [x] Linear FP16 scene color and a configurable composition pass replace UNORM scene lighting and in-material output transforms across DX11, DX12, and Vulkan.
- [x] Route every existing export-relevant update through the explicit scene/export clock and remove wall-clock animation and script timing.
- [X] Give the video exporter an off-screen render target and asynchronous readback instead of capturing the DX11 game swap chain.
- [x] Route every existing export-relevant update through the explicit scene/export clock and remove wall-clock animation and script timing.
- [X] Give the video exporter an off-screen render target and asynchronous readback instead of capturing the DX11 game swap chain.
- [x] Render an explicit frame at `t = 0` before advancing simulation.
- [x] Add repeatability tests that compare multiple exports of the same scene.
- [x] Separate output frame rate from physics simulation rate.
- [x] Add an export physics rate, accumulator substeps, maximum substeps, and rigid-body solver iterations.
- [x] Verify equivalent physics outcomes across output frame rates.
- [x] Separate encoder quality from render quality in CLI/editor settings.
- [x] Add an off-screen export render target and asynchronous GPU readback.
- [x] Add spatial supersampling/MSAA, temporal accumulation, samples per frame, subframe sampling, and shutter controls.
- [x] Add export-specific shadow, lighting, reflection, and post-process quality.
- [x] Add FP16/FP32 render targets and genuine HDR/high-bit-depth output.
- [x] Add a compatibility-checked simulation bake/cache for rigid bodies, cloth deformation, and animation-visible transforms/meshes.; runtime object-topology changes invalidate a bake.
- [x] Support PNG and EXR image sequences.
- [x] Write an export manifest with frame range/settings/completion state and resume missing or selected frames.
- [x] Encode completed sequences as a separate final step while preserving direct-to-FFmpeg as the fast path.
- [x] Convert equirectangular HDRIs into cached cubemaps with diffuse irradiance and prefiltered specular products.
- [x] Linear FP16 scene color and a configurable composition pass replace UNORM scene lighting and in-material output transforms across DX11, DX12, and Vulkan.
- [x] Render audio against the export clock, write offline float PCM, and mux it into MP4/WebM/MOV without re-encoding video.
- [x] Replace the Assets explorer's scene/prefab special cases and OS-shell fallback with an internal editor registry keyed by normalized extension and, where necessary, probed asset type. Scene and prefab files open as independent documents, XML is routed by its probed root type, registered material/texture/mesh/model/skeleton/animation/audio/script types route to their typed document, and unsupported types retain OS associations. Dedicated authoring controls remain tracked separately below.
- [x] Name every internal editor tab/window from the opened filename, including its extension. Keep the normalized canonical path as the stable hidden window identity and save target, expose it in a tooltip, allow equally named files from different folders to remain open, and append the dirty marker consistently.
- [x] Separate reusable panel type names from asset-document titles and give each open document its own dirty state, undo/redo history, save strategy, selection state, and dependency-refresh path.

- [x] Route supported assets through internal document editors selected by normalized extension and, where needed, probed asset type; preserve OS associations for unsupported types.
- [x] Name document tabs/windows from the opened filename including extension; use normalized canonical paths for hidden identity and save target, expose paths in tooltips, allow same-named files from different folders, and show dirty markers.
- [x] Separate reusable panel names from document titles and give each document its own dirty state, undo/redo history, save strategy, selection, and dependency refresh path.
- [x] Open scene/prefab documents, individual mesh documents, and imported model skeleton documents in independent transient editing scenes. Object child-hierarchy and Skeleton ghost visibility can be toggled; imported model documents can switch between Object and Skeleton focus.
- [x] Object mode edits an isolated prefab/object root with an Include child hierarchy toggle. For non-imported prefabs, Hierarchy context actions can add children and delete non-root descendants; viewport/world creation and external asset drops are disabled. Imported model hierarchy mutation remains disabled to protect node bindings.
- [x] Mesh mode displays one mesh, edits per-vertex position/UV/color and normalized skin influences, exposes its material component in Properties, and saves native `.mesh` data (OBJ edits save to a sibling `.mesh`).
- [x] Mesh view provides a 2D UV island/wire preview with vertex picking, direct UV dragging, and a context action to reset the selected vertex UV.
- [x] Skeleton mode filters hierarchy and selection to bones; supports a non-selectable associated skinned-mesh ghost, bone overlays, Apply Rest Pose, and temporary animation preview with clip selection, playback, looping, speed, scrubbing, and snapshot restoration on stop.
- [x] `SceneView` accepts document-scoped selection, creation, drop, transform, and tool-drawing controls; Hierarchy and Properties use the focused document/selection.
- [x] Keep `AssetPreviewCache` thumbnail-only. Interactive single-asset editing reuses the independent `SceneView` document path rather than adding a second viewport implementation.
- [x] Add a dedicated Mesh Edit stage with vertex, edge, and face selection; box/lasso selection; transforms with snapping and configurable pivots; extrude, inset, bevel, loop cut, weld, split, delete, duplicate, bridge, and fill operations; and undo/redo.
- [x] Expose Mesh Edit mode in scene and prefab views with one hierarchy-driven active mesh, a searchable Mesh Tools panel, vertex/edge/face modes, move, extrude, inset, edge split, face delete, shared-asset save, and undo/redo.
- [x] Add an authored-geometry transaction separate from runtime `SetDeformedVertices`. It validates topology and attributes, persists indexed native mesh data, updates bounds/GPU resources and authored revisions, atomically saves, refreshes dependents, and retains indexed undo data. This is the canonical mesh validation and persistence task.
- [x] Add Skeleton mode to scene and prefab editors with nested Edit Bone, Add Child, Remove Bone, and Weight Paint tools. Paint the selected bone's vertex weights with an opaque gray heat-map override, circle/square brushes, adjustable size, strength, and falloff, and add/subtract/replace/smooth/normalize actions. Save weight edits to the shared mesh asset with stroke undo/redo.
- [x] Finish Skeleton Edit viewport controls with hierarchy-driven active skeleton highlighting, configurable snapping and pivots for bind-pose transforms, and unified undo/redo across bone and weight asset edits.
- [x] Add bind-pose bone authoring: create, delete, duplicate, rename, reparent, and mirror bones; edit joint position, orientation, and bone roll; validate parent cycles and joint references; update inverse bind matrices and dependent skinned meshes when the bind pose changes.
- [x] Extend Weight Paint with bone locking and mirrored painting, and add precise painting across sparse meshes and overlapping surfaces while retaining normalized weights and one undo step per stroke.
- [x] Lock individual bone influences during painting and normalize the remaining unlocked weights; finish a stroke before switching bones or meshes.
- [x] Select a second bone for mirrored strokes, map the brush through both bones' orientations and normalized lengths, keep lateral brush size unchanged, and record both sides in one stroke.
- [x] Add skin binding tools to assign or rebind a mesh to a skeleton, generate initial weights, assign selected vertices to bones, inspect and repair unweighted vertices or missing joints, and save/reload the authored skeleton and weights. Keep shared mesh references and existing animation joint references valid, or report mappings that require repair.
- [x] Add regression tests for save/reload, hierarchy edits, skin binding, weight operations, and undo/redo, including the static default-pose view in Skeleton Edit and Weight Paint.
- [x] Build a separate Animation Editor for posing the skeleton and creating or editing animation clips. Keep its pose transforms and timeline separate from the Skeleton Edit bind pose and skin weights.
- [x] Open Animation Editor from an explicitly selected skeleton or skinned mesh. Resolve that rig's model, Animation Manager, and all meshes bound to its skeleton; show the chosen rig in the editor. Support prefabs with multiple rigs without silently choosing the first skeleton or manager.
- [x] Search the project Assets directory from the Animation Editor for rigged prefabs and open a chosen rig. Generate standalone `.rig` assets on skeletal model import or animation save, and reference them from Skeleton and Animation Manager components.
- [x] Move bundled skeletal prefab bind data into `.rig` assets and load those rigs through scene prefab references; stop writing duplicate inline joint and inverse-bind arrays for rig-backed prefabs.
- [x] Validate a clip's referenced bones against the selected rig before previewing or editing it. Allow clips that key only some bones. Report missing or ambiguous bone mappings and require repair before applying unresolved channels; do not treat matching node indices alone as proof that clips from different models are compatible.
- [x] Add regression tests for explicit rig selection, multiple rigs in one prefab, sparse bone channels, incompatible clip mappings, and mapping repair across save/reload.
- [x] Cache timeline clip mappings and per-bone key times until clip data or rig bindings change. Cull off-screen rows and key markers so paused-editor work scales with the visible timeline.
- [x] Cache compatible preview clips instead of copying and remapping clip data on every playback or scrub sample. Update only playhead times and layer weights per frame.
- [x] Filter the cached rig asset list while typing in the Animation Editor search field. Refresh the asset scan only when project assets change or the user requests it.
- [x] Record one undo entry when a clip-duration drag ends, avoiding a full clip copy and scene serialization on every drag update.
- [x] Build reusable brush assets and richer surface/weight painting controls in the editor.
  - [x] Add a separate Paint section to the main editor toolbar with Cursor, Brush, Eraser, and Bucket selection; Cursor can return to viewport selection.
  - [x] Add project `.brush` assets that can be created in Assets, edited in the asset inspector, and loaded, saved, or duplicated from the weight paint toolbar. Keep preset settings separate from bone, paint weight, and mirror target.
  - [x] Apply weight brush size, hardness/falloff, strength, opacity, flow, spacing, square rotation, and scatter. Add eraser and connected-face bucket fill with tolerance and opacity, preserving locked-bone and normalization rules.
  - [x] Test preset save/reload and invalid settings, connected bucket regions, and locked-bone erasure. The Debug Editor build and focused tests pass.
  - [x] Add a paint mode for both mesh surfaces and image assets (starting with `.png`): paint vertex colors or texture targets on meshes, and open image files as editable 2D canvases for drawing. Provide visible stroke preview, undo/redo, and save/reload for both targets.
    - [x] Open PNG assets as editable canvases with Cursor, Brush, Eraser, and Bucket in the editor Paint header; show a brush outline, save strokes to the PNG, and reload pixels during undo/redo.
    - [x] Load, create, save, and duplicate shared `.brush` assets from the PNG paint toolbar.
    - [x] Test deterministic PNG stamping, erasure, connected fill, and encoded PNG reload.
    - [x] Paint vertex colors directly on a mesh with Brush, Eraser, and connected Bucket tools, a viewport brush outline, shared mesh undo/redo, and Save Mesh persistence.
    - [x] Verify vertex color snapshot undo/redo and native mesh save/reload with a focused test.
    - [x] **Required:** Paint a mesh texture target directly on the surface, using its selected UV set and respecting texture wrap at UV boundaries. Include target selection, stroke preview, undo/redo, and save/reload so this meets the parent paint-mode criteria.
    - [x] **Important:** Add mesh UV unwrap with seams and island packing; keep the existing manual UV editor for adjustments. Texture painting can use authored UVs before unwrap exists.
  - [x] Share the preset library across weight paint, mesh paint, and image asset paint, with graphical brush previews in Assets and the paint toolbar.
    - [x] Share `.brush` presets across weight, vertex color, and PNG paint; show graphical analytic tip previews in Assets and their paint toolbars.
    - [x] **Required:** Use shared `.brush` presets in mesh texture painting; apply supported settings and report unsupported ones before a stroke.
    - [x] **Optional:** Apply imported image tips in mesh texture painting and preview them in Assets, where only analytic tip previews are currently shown.
  - [x] Apply image tips and pen pressure, and add smear and average behaviors plus further symmetry/masking controls. Bucket mirroring is still unsupported.
    - [x] Add Blur, Smear, and Average to PNG image painting.
    - [x] Apply image tip alpha or opaque luminance to PNG Brush, Eraser, Smear, and Average strokes; preview tip images in the brush inspector and PNG toolbar.
    - [x] Add PNG X/Y mirror placement and Alpha Lock for non-bucket strokes; report unsupported bucket mirroring.
    - [x] Map Windows pen pressure to size and strength responses in PNG, vertex color, and weight paint.
    - [x] **Optional:** Apply image tips to weight and vertex painting, and add mesh paint symmetry and masking. Keep unsupported-tip warnings until each mode implements them. Consider bucket mirroring separately.
  - [x] Import image brush tips and Photoshop `.abr` brush packs as project brush assets. Investigate Blender `.blend` brush assets and provide a direct importer or documented Blender export/conversion path. Preview imported brushes, map supported settings, and report unsupported settings without silently changing the stroke. Keep imported source files and reusable presets in the project Assets workflow.
    - [x] Import an image tip from the PNG paint toolbar into `Assets/Brushes/Tips`, preview it, and apply its alpha to PNG paint strokes.
    - [x] Document a manual Blender brush conversion path that recommends retaining source `.blend` files in project Assets.
    - [x] **Optional:** Import `.abr` packs and map their supported settings with explicit unsupported-setting reports. A direct `.blend` importer is also optional while the documented conversion path is available.
  - [x] **Required with new paint modes:** Add focused tests for mesh texture strokes across repeating UV seams, saved-result reload and pixel-history restoration, Alpha Lock, UV unwrap, and imported preset behavior. PNG, vertex color, preset serialization, and weight bucket regression tests already exist. Use [Blender weight paint brushes](https://docs.blender.org/manual/en/latest/sculpt_paint/weight_paint/brushes.html), [Blender brush asset management](https://docs.blender.org/manual/en/5.0/sculpt_paint/brush/brush_management.html), and [Photoshop brush imports](https://helpx.adobe.com/photoshop/desktop/apply-painting-techniques/brushes-presets/import-brushes-brush-packs.html) as feature and format references.
- [x] Add localized stretch and compression for skinned mesh surfaces, driven by bone motion and mapped collision impulses. Preserve skin weights, cap deformation, and restore the current animated shape after impacts. See [research and acceptance criteria](skinned-soft-tissue-research.md).
  - [x] **Required foundation:** Add post-physics evaluation and a transient per-vertex deformation path after morphing/skinning; keep CPU contact/portal geometry equivalent to rendered geometry and leave authored mesh data intact.
  - [x] **Required response:** Bind a reusable soft-tissue component to the skinned surface; support pose-driven strain, mapped collision impulses, response masks, damping, displacement/strain caps, and clean reset/rebind.
  - [x] **Required verification:** Test impact localization, rest-contact stability, decay to the moving animated pose, preserved weights/topology, CPU/GPU parity, portal behavior, and fixed-step stability.
    - [x] Automated surface, scene-impact, moving-pose decay, skin-binding, and portal traversal regression tests pass.
    - [x] Render the deformed demo scene on DX11, DX12, and Vulkan; compare the CPU posed stream with the uploaded vertex stream and portal triangle stream.
- [ ] **Next priority, portable OBJ and sprite import dependencies:** Prefab mode saves OBJ and sprite animation scene objects, but copied `.obj` and `.spriteanim` files still use the existing single-file copy path. Package OBJ material/texture files and sprite sheet/image files with stable relative references so imported prefabs remain usable after moving or exporting the Assets directory. Add a dependency round-trip regression for both formats.

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
- [x] Import and animate material edge color, opacity, and width offsets.
- [x] Import and animate material sphere and toon tint offsets, including additive and multiplicative operations.
- [x] Preserve additional-UV2/3/4 streams in mesh assets and expose their XY morph channels at runtime.
- [x] Preserve primary and additional UV Z/W values and morph offsets in mesh assets so materials can use all four channels.
- [x] Preserve SDEF center and radius data through decoding, mesh assets, and runtime buffers.
- [x] Implement SDEF deformation and compare reference vertices across animated poses.
- [x] Implement QDEF dual-quaternion deformation and compare reference vertices across animated poses.
- [x] **Inventory:** `PmxRigController`, `PmxMaterialMorph`, and `VmdSceneTrack` are format-named runtime components. `AnimationManager`, scene update, and Bullet physics currently refer to PMX component types directly. `Cloth` already exposes reusable soft-body controls. `MmdDanceReplay` is scene-specific and belongs in `Engine/Core/Assets/Scripts/Animation`.
- [x] Extract append transforms, axis limits, ordered CCD IK, and external-parent bindings from `RigController` into reusable `RigConstraints` under `Engine/Core/Compoonents/Animation`; keep PMX deform-order mapping in the importer. `RigController` remains as a hidden compatibility payload for older serialized scenes.
- [x] Move physics-to-bone binding and joint descriptors into reusable `RigPhysicsBinding` under `Engine/Core/Compoonents/Physics`; Bullet consumes it directly.
- [x] Move bone and impulse morph application into reusable `RigMorphControls` with named weights and animation inputs.
- [x] Replace `PmxMaterialMorph` with a reusable material-property morph component covering diffuse, alpha, ambient, specular, shininess, texture tint, and edge/sphere/toon values. `MaterialPropertyMorph` is editor-addable and the Mococo prefab has been migrated.
- [x] Remove PMX component type checks from `AnimationManager` and scene update; dispatch animation weights, IK switches, and pre/post-physics evaluation through `RigConstraints`, `RigMorphControls`, `RigPhysicsBinding`, and `MaterialPropertyMorph`.
- [x] Convert `VmdSceneTrack` into reusable camera/light animation tracks while keeping VMD frame decoding, coordinate conversion, and Bezier interpretation in the VMD importer. `SceneCameraLightTrack` stores normalized Bezier controls and authored frame rate; no checked-in scene uses the removed legacy component.
- [x] Make `ImportedModel` rig, morph, joint, and soft-body descriptors format-neutral; translate PMX numeric flags and indices in `PmxModelDecoder` before the native asset writer sees them. Rigid-body shape and motion, joint kind, and soft-body topology now use named descriptors; the PMX decoder maps validated source values before writing native assets.
- [x] Replace the `Material.useMmdSpecular` format switch with the named `specularModel` setting (`MetallicRoughness` or `BlinnPhong`); migrated the bundled materials and Mococo prefab while preserving its specular values.
- [x] Migrate existing serialized PMX prefabs and clips to the reusable components with an on-load conversion; verify Mococo scene playback and non-PMX component authoring. The bundled Mococo prefab, rigid-body names, and materials use the new names. Old rig/material/VMD track data convert to the new components, and headless playback advances both Mococo body and facial clips. Authored rig constraint and material morph tests pass.
- [x] Audit other importer-created runtime components for format-specific names or direct importer dependencies; current model and VMD writers create generic runtime component types under `Engine/Core/Compoonents`.
- [x] Keep `PmxModelDecoder`, `MmdDanceAssetBinder`, and `MmdPoseRetargeter` format-specific in `Engine/Editor/Core/Importers`; keep `MmdDanceReplay` as scene choreography in `Engine/Core/Assets/Scripts/Animation`.
- [x] Build rope soft-body topology from PMX material vertices and render its simulated deformation.
- [x] Map PMX soft-body cluster settings to Bullet and test a clustered fixture.
- [x] Implement near-anchor behavior and two-way forces between soft bodies and dynamic rigid bodies.
  - [x] Use Bullet anchors for two-way force exchange; a synthetic PMX fixture verifies that cloth wind moves an anchored dynamic body.
    - [x] Record Bullet/PMX behavior and verify engine reset and body-rebuild anchor survival in `.md/near_mode_comparison.md`.
- [x] Import sphere textures and implement PMX multiply, additive, and additional-UV sphere modes.
- [x] Render material edge outlines using imported color and width, including skinned meshes.
- [x] **PMX model import:** Implement the staged PMX importer and Properties controls below.
- [x] Verify external-parent targets and after-physics append/IK order with an authored test rig.
- [x] Compare physics-driven bone feedback over repeated playback and reset.
- [x] **High, visible point-controller rig:** Draw joint position, segment direction, and segment twist points on resolved skeletons. Ctrl-click selects multiple joint points for grouped movement. Mesh and bone picking remain separate input modes. The Debug Editor build passes; live interaction review remains below.
- [x] **High, static controller activation and persistence:** Active points use soft full-body targets; inactive points remain unconstrained; fixed position/rotation pins remain hard targets. The viewport shows active and inactive point colors, and controller targets plus pin state serialize on AnimationBone. The focused serialization test and Debug Editor build pass.
- [x] **High, keyed controller state and sampling:** Animation clips store per-bone controller keys for active state, target, and fixed position/rotation pins. The Animation Editor's Key controllers button records the current rig at the playhead. Scrubbing switches to the latest key at or before the sampled time; before the first key the controller is inactive. Clip serialization and step sampling pass the focused regression test, and the Debug Editor builds.
- [x] **High, controller Auto Key:** With Auto Key enabled, dragging a point records its controller target at the playhead; Active and pin toggles also key their new state. Controller state is captured before pose keying resamples the preview, and it shares the pose edit's undo snapshot where available. The Debug Editor build and focused controller test pass.
- [x] **High, controller timeline editing:** Controller key times appear on their bone rows and in clip summaries. The existing timeline drag and delete actions move or remove the controller key at a selected bone/time alongside any ordinary bone keys there. The focused controller regression covers move and delete; the Editor source compiles, but linking the executable was blocked because `Editor.exe` was in use.
- [x] **High, controller export:** The Animation Editor's explicit bake action samples controller and pin targets at 30 fps and at control keys, writes the solved local poses to ordinary bone channels, and clears baked control tracks. New keys can use Body space to follow a moved model root or World space to stay fixed; older keys retain World space. The Debug Editor build and focused controller regression pass.
- [x] **High, whole-body pose response:** Full Body drags retain the starting joint pose as weighted preferences while solving the selected target, other active targets, and fixed pins; pins receive the final correction pass. Dragging the pelvis moves the root while the connected chains correct planted targets. Synthetic regression cases cover reaching, stepping, pelvis movement, and off-center pulls; the Debug Editor build passes. Live GUI interaction review remains in the separate interaction-validation item.
- [x] **Medium, controller-level rig behavior:** Controllers now store IK/FK mode, hierarchy-scope override, ancestor-chain depth, and optional world-target facing with a local forward axis and weight. Follow scope uses the viewport Selected/Body Part/Full Body setting; IK promotes Selected to Body Part; FK moves the selected point directly. Body Part uses the configured ancestor count, while Full Body solves active points and fixed pins together. The Scene view can add model-bound or scene-only control points without skinning weights for hands, fingers, props, and other rigs; model-bound points can be keyed in animation. Serialization tests and the Debug Editor build pass.
- [x] **Medium, constraints and visual feedback:** Maintain intended distances between connected points, show stretched or compressed connections, and provide a way to restore their configured lengths. Add the body coupling and balance constraints needed for natural full-body dragging.
- [x] **Initial CPU emitter foundation:** Add a registered, serializable `ParticleEmitter` with point/box/sphere spawning, deterministic seed and stable IDs, a bounded particle pool, 60 Hz updates, lifetime/force integration, local/world space, and Properties controls. A focused regression covers replay, capacity, expiry, and settings round-trip. Rendering and the remaining workflow are tracked below.
- [x] **Initial visible particle pass:** Render CPU particles as untextured camera-facing quads with lifetime fade and additive blending in Scene and Game views on the shared DX11/DX12/Vulkan pipeline. Use one instanced draw and per-backend frame-slot upload storage. A DX11 image regression checks the drawn result; Vulkan shaders compile. Follow-up rendering work is tracked below.
- [x] **Particle texture and blend controls:** Add a serialized texture path, opacity, and additive/alpha blend choice to the emitter. Batch particles per emitter texture and blend mode, and sample RGBA textures in the shared shader. A DX11 image regression checks texture tint, zero opacity, and alpha rendering. Alpha particles currently draw in emitter order; depth sorting, soft depth fade, and portal views remain in the rendering item below.
- [x] **Shared particle state and first solver boundary:** Move common state into `ParticleState` and run the existing ballistic integration through `IParticleSolver` at the emitter's fixed step. Solvers can own separate data keyed by stable particle ID, reset with the emitter, and mark particles inactive for lifecycle culling. Regression coverage checks force consumption, solver replacement, reset, and expiry. A fluid solver remains open below.
- [x] **Bounded emission and particle bounds:** Add a serialized finite duration and loop option, with emission windows quantized to the fixed 60 Hz clock. Completed one-shot emission leaves existing particles alive; restarting or looping queues the start burst again, and pause holds the emission clock. Expose conservative world bounds for live billboard particles, including local-space emitters. Regression coverage checks replay, loop boundaries, pause/resume, serialization, and bounds.
- [x] **Particle editor controls and bounds integration:** Show live count/capacity and Play, Pause, Restart, Clear, Burst, and fixed-step preview controls in Properties without treating playback actions as serialized edits. Draw the selected emitter's point/box/sphere spawn shape, show live bounds in viewport bounds overlays, and include particle bounds when focusing the editor camera. The editor builds and focused emitter/render regressions pass. Continuous paused-editor preview and particle picking remain open.
- [x] **CPU fluid neighbor search reference:** Add a spatial grid over active `ParticleState` positions with cell width equal to smoothing radius. Radius queries return deterministic source-index order; large-radius queries scan directly. A regression compares cell and boundary queries, negative coordinates, coincident positions, exclusions, inactive particles, and rebuilds against brute force.

- [x] **High, particle-state and update contract:** Define position, velocity, age/lifetime, color, size, rotation, stable ID, and active/dead state. Use a fixed simulation step, bounded capacity, deterministic spawn order/seed, and explicit reset behavior for Play, scene reload, and scrubbing. Reserve extensible solver data without requiring fluid-only fields for ordinary effects. `Seek` resets and replays 60 Hz ticks, with an optional per-tick transform sampler for moving emitters; invalid or over-budget seeks preserve live state. Properties exposes a non-serialized preview time control. Regression coverage checks seeded IDs/state, backward and forward seeks, Play resume, scene reload, and moving-emitter replay.
- [x] **High, reusable ParticleEmitter component:** Add serialized rate, burst, max count, lifetime, initial speed/direction/spread, gravity/drag, local-versus-world simulation space, start/stop/loop controls, and deterministic seed. Support point, box, and sphere emission shapes. Expose controls in Properties and make presets/prefabs round-trip without changing existing scenes. Play now queues the configured start burst once even when Play on start is disabled. Regression coverage checks every serialized setting through a component preset copy, prefab save/load, and scene reload; runtime particles remain transient.
- [x] **High, CPU particle simulation and lifecycle:** Implement allocation/recycling, emission, force accumulation, integration, expiry, and bounds updates behind a solver interface. Keep simulation independent of the draw code so another solver can consume the same particle state. Define behavior when the per-emitter or scene-wide particle budget is reached. Dense live storage recycles expired slots while stable IDs keep increasing; solvers receive removal callbacks to release per-ID data. Per-emitter capacity and the scene's default 100,000-particle CPU budget preserve existing particles and discard excess spawns without backlog. Scene order decides which emitters retain particles if the budget is lowered, and the scene budget can be changed at runtime. Regression coverage checks force accumulation, expiry, recycled slots, bounds, dropped bursts, solver cleanup, and budget changes across emitters.
- [x] **High, particle rendering on DX11/DX12/Vulkan:** Batch camera-facing quads with texture, color, size, alpha blending, depth test, and optional soft-particle depth fade. Support Scene and Game views, portal views, and editor selection/bounds without creating one scene object per particle. Document sorting and transparency limits. Portal draws now use the connected camera, stencil aperture, content scope, and exit clip plane. A DX11 image regression checks main and portal particles, including exit-plane clipping; Vulkan particle shaders compile. [Rendering contract](particle-rendering.md) records alpha ordering and why optional soft-particle depth fade is currently disabled.
- [x] **Medium, emitter editor workflow:** Create Particle Emitter objects from Hierarchy or Scene view, or add the component to an existing object through Properties. The selected emitter has point/box/sphere gizmos, live count and scene budget statistics, visible defaults, and Play/Pause/Restart/Clear/Burst/Step/Scrub controls. Edit mode Play advances with the emitter's runtime 60 Hz clock; Burst materializes immediately while paused, and scrubbing uses deterministic reset/replay then holds at the chosen time. Preview state remains transient and does not trigger serialized scene changes each frame.
- [x] **Medium, collision and interaction hooks:** Start with scene collider response or kill-on-contact plus configurable bounce/friction. Expose events or batched hit data for gameplay scripts without per-particle scene components; define how moving colliders and portal traversal affect simulation-space positions and velocities.
- [x] **Medium, backend-neutral GPU compute foundation:** Add compute pipeline creation, writable storage buffers, dispatch, and resource synchronization to the shared graphics interfaces and all three backends. Keep particle state on the GPU when simulating and drawing there; provide a deliberate readback path only for features that require CPU results. The shared contract and three backend implementations are in place; DX11 compute/readback runs in regression, while DX12 and Vulkan compile. [GPU path and limits](particle-gpu.md).
- [x] **Medium, GPU particle path:** Port emission, integration, compaction/free-list management, and batched drawing to compute. Preserve the CPU path as a reference and fallback, and compare fixed-seed runs for counts, lifetime, bounds, and broad visual behavior across backends. Opt-in world-space ballistic GPU emission, integration, dense compaction, batched drawing, and on-demand count/age/bounds snapshots run on DX11, DX12, and Vulkan. Fixed-seed CPU/GPU regressions compare count, age, bounds, budget, and expiry on all three backends. Each backend captures rendered and expired frames; a pixel-difference regression compares particle silhouettes across all three. [GPU path and limits](particle-gpu.md).
- [x] **GPU fluid neighbor search:** Port the verified CPU grid/query contract to the GPU compute path. Compare neighbor sets and boundary cells with the CPU reference before scaling particle counts.
- [x] **First fluid solver:** Implement a small 2D/3D SPH or position-based-fluids prototype using fixed substeps, density/pressure or density constraints, viscosity, gravity, and static boundaries. Expose fluid-specific parameters separately from general emitter controls; validate stability, mass/count behavior, and reproducible reset.
- [x] **Fluid rendering and coupling:** Add a fluid material/visual mode, then evaluate screen-space smoothing or surface reconstruction for coherent water. Add rigid-body coupling and moving boundaries only after the basic solver is stable; define whether interactions affect gameplay physics or visual effects only.
- [x] **Verification and examples:** Keep focused regression tests in `.local/tests` and guard their optional CMake targets with source-file existence checks. Create one ordinary VFX scene and one fluid test scene; place their scripted demonstrations in `Engine/Core/Assets/Scripts/Particles`.
- [x] Add a reusable Terrain component with configurable X/Y cell counts and N/M cell dimensions, editable height samples, and generated mesh geometry.
- [x] Persist terrain dimensions, height data, and material layers with scene serialization.
- [x] Show terrain tools when a selected object has Terrain: brush size/strength and raise, lower, average, blend/smooth operations in the scene viewport.
- [x] Support a list of layers with height based assignment and manual painting; preview the weights through vertex colors.
- [x] Load terrain layer material assets and blend base color textures using painted weights in the viewport shader (up to six layers); use material or preview colors when a texture is unavailable.
- [x] Integrate terrain creation and inspection into editor component workflows.
- [x] Add focused headless regression tests for terrain mesh generation, brush editing, painted weights, and persistence.
- [x] Add editor brush ray integration coverage and a DX11 rendered viewport regression for painted material blending.

Target: a roughly 225–230 km² world (about 15 km across) that can be traversed end to end. At 128 m per cell, a 15.36 km square has 120 × 120 = 14,400 cells. The current 16.5 km test scene has a 20,000-cell cap, enough to populate its full grid as the camera traverses it.

Earlier baseline (Release DX11, 960×540, this machine, dirt/grass textures, before far-field LOD): radius 2 / 256 m / 25 cells averaged about 108 FPS after loading; radius 3 / 384 m / 49 cells averaged about 91 FPS. With a two-cell load budget, radius-2 streaming averaged 13.6 ms per frame and peaked at 21.8 ms in the measured route. Rebenchmark with far-field LOD enabled before quoting an FPS target.

- [x] Store world cells as compact coordinate runs, load unedited heights from a saved indexed archive, and retain separate sparse data only for edited cells. Verify the full 16,641-cell scene without resident meshes or inline JSON payloads.
- [x] Cache generated static cell heights, painted weights, and mesh geometry on disk as cells are visited; restore them on reload and regenerate when terrain settings or source materials change.
- [x] Add an offline full-map bake. The scene ships a roughly 40 MB indexed height and paint archive; the optional compact CPU mesh cache for all 16,641 cells takes about 0.75 GiB.
- [x] Persist a compact world manifest with row runs for procedural cells and independent sparse height/paint edit files. Save and reload edits at opposite map edges without rewriting the other edge's data.
- [x] Add distance-based LOD meshes and coarse far-field terrain so hills remain visible several kilometers away while nearby cells stay editable at full resolution. Stitch LOD borders without cracks.
- [x] Cap synchronous cell loads and unloads per update and load nearby cells first, reducing sudden full-radius rebuilds.
- [x] Generate noise, meshes, and cell data asynchronously; cap GPU uploads and prioritize cells in the travel direction. A short Release 960×540 radius-2 streaming run averaged 10–12 ms with one upload per frame; a frame above 30 ms remains to profile on the full-map route.
- [x] Reduce terrain draw calls and material bindings through batching or instancing; add frustum, distance, and shadow-pass culling. Profile CPU and GPU costs before and after.
- [x] Preserve cross-cell brush editing and saved edits through LOD changes and load/unload cycles; add regressions at cell, LOD, and world-coordinate boundaries.
- [x] Keep terrain coordinates stable across at least 15 km with double-precision transform positions, world matrices, camera picking rays, terrain cell placement, and brush coordinates. Release regressions cover camera position and scene round-trip, ray picking, sub-millimeter brush selection and shared borders at opposite edges, plus physics raycasts near ±7.68 km.
- [x] Define bounded CPU/GPU terrain allocation and mesh-cache budgets for a 15.36 km route; measure peaks while traversing, teleporting, and backtracking. The Release radius-8 route peaked at 38.8 MiB terrain CPU, 28.6 MiB terrain GPU buffers, and 831.2 MiB mesh cache; process working set peaked at 421.2 MiB. Shared textures and renderer allocations are outside the terrain allocation totals.
- [x] Add a repeatable Release 1080p performance test in `.local/tests` that samples full-map traversal, teleport, and backtracking and records frame, streaming, visibility, resident-cell, CPU/GPU, and memory metrics.
- [x] Compare 5 km and 25 km game-camera far planes at 1080p from one viewpoint while looking around, walking at 1.4 m/s, and running at 5 m/s.
- [x] Bake a compact full-map distant visual mesh and keep detailed terrain streaming local. The 25 km game camera can see terrain across the map while the scene camera keeps its shorter edit view.
- [x] Bring the 1080p fixed-view test near 100 FPS with kilometer-scale terrain visibility. At a 25 km far plane and 16-cell detailed radius, the distant mesh reached 22.5 km; the cold-cache run measured 5.8 ms median frame, 6.3 ms p95, and 4.5 ms median GPU time. The performance scene opts into fast directional diffuse terrain lighting while the scene editor and other terrain scenes retain their existing shading.
- [x] Rebuild the distant visual grid from saved terrain edits during an offline bake. Sparse edited-cell patches preserve interior sculpted silhouettes after restarting the scene without enlarging the base grid.
- [x] Split the baked far grid into 32-cell mesh tiles with per-tile bounds and frustum culling. A camera-cell crossing updates only affected tiles; the test crossing rebuilt one tile in 0.6 ms and uploaded 294 KiB. Saved sculpt patches and the 22.5 km visible silhouette remain. The 1080p fixed-view run measured 7.3 ms p95, 4.7 ms median GPU time, and 58 peak draws versus 6.3 ms p95, 4.5 ms median GPU time, and 35 peak draws for the former single mesh. The tiles improve update locality but increase draw and scene traversal cost.
- [x] Separate cell-boundary frame cost into manager streaming, far-tile updates, scene rendering, and GPU work. Keep existing terrain batches while an incomplete 4×4 group fills, so arriving cells render individually and the group uploads once when ready. The paced 5 m/s crossing loaded 15 cells in one second with no batch reuploads; fixed-view p95 stayed near 7.0 ms.
- [x] Reduce cell-arrival render cost without shrinking the 25 km far plane. Far-cell batches stay visible across LOD changes until an upload slot can replace them, and the next slot is reserved for pending batch work. At the last eighth of a cell, the async worker prepares the next cell band but holds GPU uploads until crossing. In the same 25 km ridge benchmark, crossing draw p95 fell from 153 to 50, frame p95 from 26.3 ms to 15.3 ms, and the worst streaming stall from 21.3 ms to 7.3 ms. The 27 replacement batch uploads increased manager CPU p95 to 5.2 ms; the broader viewpoint and PIX work above will determine whether further batch work is worthwhile.
- [x] Add optional 1080p rendered captures for ground-level range, a high viewpoint, and a cell boundary. Comparing lit, unlit, shadow-disabled, and sun-disabled images identified the dark strip as the 1 km realtime-light cutoff on detailed terrain. One selected directional sun now lights both detailed cells and the distant grid at every view distance; the far grid shades vertices, while detailed terrain samples at most one near shadow texel and fades shadows out by 96 m. The high-view capture checks near and far sunlight.
- [x] Stitch the detailed-to-distant geometry boundary. The first far cell now retains every 4 m height sample along its shared edge and tapers to the coarse grid across one cell; saved far sculpt patches use the same edge heights. A geometry regression checks all 33 samples on each of the four edge directions, and the high-view capture no longer shows the white gaps. The crossing tile upload rose from about 294 KiB to 710 KiB and its rebuild from 0.5 ms to 3.3 ms.
- [x] Add Bake actions to standalone Terrain and the map TerrainManager. A completed map bake writes one indexed archive with heights, paint weights, normals, colors, and LOD topology; play mode reads individual cell ranges and swaps the baked LOD topology without generating it. The performance scene no longer has a Perlin component, and play mode requires the bake, with no source-builder fallback. The saved height archive remains an editor and bake input. The full 16,641-cell bake was packed on 2026-10-27 into a 1.82 GB archive; all packed payloads were verified against the earlier files before those files were removed. The benchmark checks and automatically bakes before measuring when needed. Generated bake output is ignored by Git.

- [x] Limit far-tile rebuilds after the initial reveal using `TerrainManager.maxUploadsPerUpdate`; prioritize nearby tiles and continue deferred work on later updates. Keep existing tile geometry visible until a replacement is ready.
- [x] Add a focused scheduling regression test for the upload cap, deferred updates, and settled state.
- [x] Add a configurable soft CPU time budget for far-tile rebuilds after initial reveal; always process at least one pending tile.
- [x] Index far patches by tile once, avoiding a full patch scan during each tile rebuild.
- [x] Read the far-grid archive on a background task and apply it on the scene thread when ready.

- [x] Add a two-level hierarchy of far terrain tiles. Use 64-cell, half-resolution parent tiles outside the detailed radius and 32-cell tiles near the viewer or terrain patches; switch each group when replacement geometry is ready.
- [x] Cache seam-free tiles at both far LOD resolutions with a versioned mesh format. Key by source signature, appearance, dimensions, local heights, and patch heights; changed tile data causes a local cache miss while unchanged tiles remain reusable.
- [x] Apply saved terrain edits as far-grid patches on background load, reading only edited full-resolution cells; changed patch heights invalidate affected tile entries.
- [x] Refresh distant heights and paint during a live editor brush stroke: load far tiles for the edit preview, update changed cell patches and colors, and rebuild affected tiles on the next editor view update.
- [x] Persist both far-tile resolutions in a versioned cache keyed by terrain source, appearance, tile heights, and edit patches. Load seam-free coarse tiles directly from cached meshes; changed saved edits invalidate affected entries on the next play-mode load.
- [x] Select far-tile resolution by projected screen size and camera visibility, with a configurable error threshold and hysteresis to avoid rapid LOD switching.
- [x] Make near/far and parent/child transitions gap-free: retain the old tile until its replacement is ready, match or stitch shared edges, and avoid rebuilding an entire far tile when only a small region changes.
- [x] Move far-tile generation and cache reads off the render thread; prioritize visible and nearby tiles, and cap CPU work, GPU uploads, and memory per frame. Reuse the existing terrain streaming budgets where practical.
  - [x] Prepare seam-free, unpatched distant tiles and read/write their caches on a bounded background worker; prioritize visible and nearby tiles, cap far uploads per runtime frame, and account for prepared and resident far meshes in memory usage.
  - [x] Snapshot near seam surfaces and edit patches so their full tile builds also run on the worker; keep urgent gap repairs and small in-place patch updates responsive.
  - [x] Share one upload allowance between near terrain and far tiles during a runtime frame.
- [x] Rebenchmark at increasing view distances against the current radius-based terrain path. Check steady frame time, traversal hitches, cache warm-up, visual quality, and memory before raising the supported radius.
  - [x] Correct the manual benchmark frame clock so its shared upload allowance resets each rendered frame; aim the startup camera at the measured viewpoint and add a center-of-map case. The 9 km center run now settles all 20 desired far tiles and reaches 8966 m.
- [x] Record separate terrain streaming, render CPU, and GPU step times with far tiles enabled and disabled. See `.local/performance-logs/terrain-step-summary.md` and the paired JSON/CSV traces.
- [x] Reduce shadow render CPU work. The shadow-only pass now prepares casters without ordinary lighting/material data, culls cascades once per caster using bounds, and skips scene preparation when no shadow light is selected. Added Off, Low, Medium, High, and Ultra shadow quality presets. The 1080p Debug terrain median fell from 6.48 to 2.09 ms at High, 1.55 ms at Low, and 0.17 ms at Off. See `.local/performance-logs/shadow-optimization-summary.md` for phase timings, captures, and checks.
- [x] Eliminate near-cell LOD conversion during main-thread commit. The worker chooses the prefetched destination center outside the interactive radius; stale prepared meshes are converted on the worker. Crossing commit p95 fell from 4.42 to 2.41 ms with far tiles and from 4.30 to 2.09 ms without them; commit LOD conversion is 0.00 ms p95 in both runs. See `.local/performance-logs/terrain-lod-commit-summary.md`.
- [x] Split far-driver boundary time into selection, preparation, and mesh upload. Mesh validation and bounds were the measured main-thread cost; they now run on the far-tile worker. Coarse tiles remain available during detailed-tile rebuilds, and the crossing benchmark asserts gap-free near/far coverage. See `.local/performance-logs/terrain-far-boundary-summary.md` for frame times and appearance latency.
- [x] Extend the far-tile hierarchy to larger maps, expose independent per-level screen-error and far view-distance controls, and add per-level resident-memory and distance-band benchmark logs. A 385-cell-wide test selects stride 8. Release DX11 runs at matched 3, 9.5, and 16 km clip distances found zero gaps in tested moving/turning frames and held near-cell residency at 9; the radius path reached 100, 361, and 841 cells respectively while covering less ground. After this comparison, the `TerrainManager` code default interactive radius was reduced from 2 to 1 cell. See `.md/terrain-hierarchy-distance-bands-20261029.md`.
- [x] Complete the single distance-driven terrain LOD policy in `TerrainManager`: unify near-cell and far-tile requests in one queue with explicit cross-type priority and shared upload/memory budgets. The current paths share distance/travel scoring and a frame upload budget, but still schedule through separate queues.
  - [x] Add a quarter-resolution 128-cell far-tile level above the existing half-resolution 64-cell and detailed 32-cell tiles. Select all three levels recursively by projected error and near coverage, and retain the previous representation during transitions. The camera far plane bounds far-tile requests; tile stride is selected internally rather than exposed as a view-distance setting.
  - [x] Retire resident full cell objects outside the interactive radius only after a visible far tile covers them; keep them loaded during tile preparation.
  - [x] Extend the far-tile hierarchy beyond 128-cell tiles for larger maps. A 385-cell-wide synthetic map selects stride 8; the 3, 9.5, and 16 km rendered runs each kept near-cell residency at 9 with a one-cell interactive radius.
  - [x] Expose the interactive radius, per-level screen-error thresholds, and maximum far view distance independently. Log visible coverage, tile/node count, draw count, per-level resident memory, and frame times by distance band in `.md/terrain-hierarchy-distance-bands-20261029.md` and its linked JSON traces.
  - [x] Check gap-free coverage while looking, moving, turning, and crossing cell boundaries at matched 3, 9.5, and 16 km camera clip distances against the radius path. No gaps were observed in the tested frames; after the comparison, the code default resident radius was reduced from 2 to 1 cell. The saved performance scene was already at 1.
- [x] Publish immutable scene snapshots at defined commit boundaries; execute worker object, hierarchy, and component mutations through queued commands and publish serial owner edits at the next boundary.
  - [x] Publish value-owned scene snapshots and add stable-ID mutation commands committed on the scene owner.
  - [x] Migrate direct editor and script object/component writes to commit commands or a declared serial owner path; enforce commit boundaries for every mutation.
- [x] Move the native scene/game loop onto a dedicated simulation thread, separate from window/input/UI handling and graphics submission, with synchronized input snapshots and bounded render handoff.
- [x] Move graphics submission to a dedicated owning render thread using buffered render packets, frame-safe resource retirement, and bounded frame latency.
  - [x] Route standalone graphics submission, resize, and idle waits through one render thread with a bounded one-frame handoff.
  - [x] Resolve native UI layout on the simulation owner after the object commit and hand its prepared text ordering to the render thread.
  - [x] Add DX11, DX12, and Vulkan frame-completion tokens; fence-retire replaced scene buffers, shader pipelines, and shadow atlases; bound submitted GPU frames and drain before native object removal.
  - [x] Fence-retire mesh vertex, index, morph, and posed buffers when a live component replaces them.
  - [x] Move the prepared native draw list and mesh buffer references into a simulation-published frame packet; consume and fence-retain that packet on the render owner.
  - [x] Replace the borrowed live-scene draw data with owned render packets and retire GPU resources only after their frame fences complete.
  - [x] Overlap simulation preparation for frame N+1 with render-thread submission of frame N while retaining bounded frame slots and scene commit barriers.
  - [x] Restore GPU occlusion-query feedback on the recorded native path without reading backend query state from the simulation owner.
- [x] Coordinate fixed-step updates, Play/Stop, pause, scene loading, hot reload, and shutdown across simulation, workers, and rendering without stale jobs or partially published scenes.
  - [x] Read asynchronous scene files on one bounded worker and commit the latest result with callbacks on the scene owner; cancel stale results on scene changes and shutdown.
  - [x] Restore failed scene loads and publish only the finished replacement or restored scene snapshot at the owner commit boundary; defer editor Play/Stop scene mutations until after frame submission, drain GPU work before object destruction, and cancel pending reads on hot reload.
  - [x] Fence scene-generation jobs and script-module callbacks before Play/Stop, hot reload, and shutdown; verify fixed-step and pause transitions across both hosts.
- [x] Schedule native update phases as dependent jobs: component updates, spatial scripts, transforms, animation/constraints, physics preparation, simulation, post-physics processing, and render snapshot publication.
- [x] Add component update dependencies and explicit read/write access so independent updates can run concurrently while existing scripts retain a compatible serial path.
- [x] Add explicit thread-affinity and parallel-safety policies for native component and script callbacks; move eligible Update work onto workers and marshal window, graphics, and shared-state operations to their owners.
- [x] Run Start, Enabled, Disabled, and OnDestroy on the owning simulation thread with deterministic ordering, once-only lifecycle delivery, and completion of jobs referencing affected objects before destruction.
- [x] Separate CPU preparation in deserialization and lifecycle callbacks from graphics resource creation, submitting resource work to the render thread.
- [x] Route reloadable script-module jobs through the host-owned scheduler and complete or cancel module jobs before unloading their code.
- [x] Prepare render items, material records, visibility, sorting, and compatible draw batches on workers from stable snapshots.
- [x] Cache static render items, updating only changed objects instead of rebuilding preparation every frame.
- [x] Share camera and shadow preparation across views and cascades; schedule independent visibility and batch preparation without unnecessary per-loop barriers.
- [x] Move terrain-cell loading, decoding, instance preparation, and bounds construction onto workers with bounded queues, cancellation, and scene-generation checks.
- [x] Parallelize independent animation channel sampling and isolated rig evaluation; join pose jobs before IK, transform constraints, physics, and final-pose publication.
- [x] Parallelize physics preparation and post-processing where independent; preserve simulation ownership and synchronized scene publication.
- [x] Add worker command recording through backend-specific contexts: benchmark DX11 deferred contexts and support independent DX12/Vulkan command lists or buffers.
- [x] Move expensive editor data preparation and asset processing onto workers while keeping window, input, and UI construction on their owning thread.
- [x] Record per-stage main-thread busy time, worker execution, queue delay, dependency waits, render-thread stalls, and frame latency in the Performance view and recordings.
- [x] Validate serial and parallel modes with matched-route captures, frame-time percentiles, worker scaling, and regression checks for scene loading, Play/Stop, streaming, transforms, shadows, and all renderers. See [validation results](serial-parallel-validation-20261103.md).
- [x] Replace the fixed three-worker render pool with a configurable engine job scheduler supporting dependencies, workload-sized batches, and execution/queue/wait measurements.
- [x] Parallelize dirty transform preparation for stable hierarchies, retain static matrices, and preserve owner validation during streaming commits.
- [x] Cache unchanged world bounds and prepare changed bounds from copied matrices on workers.
- [x] Retain spatial hierarchy topology and refit changed bounds, using worker jobs for large updates and rebuilding on membership or quality changes.
- [x] Keep spatial entries keyed to persistent draw slots across LOD fades and temporary visibility changes.
- [x] Prepare compatible draw batches from copied keys on workers and reuse compatibility assignments across draw-order and visibility changes.
- [ ] Cache unchanged mesh/material eligibility and compatibility snapshots to reduce owner-side batch key gathering.
- [x] Advance DX11 frame resources and persistent-cache retirement independently of GPU timing controls.
- [x] Append DX11 compute constants to the shared constant arena; retain fallback uploads and invalidate cached ranges after cross-context arena discards.
- [x] Add opt-in DX11 native-call timing by camera/shadow pass, including call counts, wall-time maxima, thread cycles, recording fields, and capture evaluation.
- [x] Compare GPU stage changes only for captures with matching renderer, benchmark route, view size, and shadow settings; label unverified comparisons.
- [x] Split grass GPU timing from ordinary opaque draws across all renderers, recordings, the Performance view, and the capture evaluator.
- [x] Add generic persistent GPU instance pools for saved terrain-layer and prefab placements.
- [x] Keep object and instance IDs stable across visibility, sorting, LOD, and streaming changes; key batch caches by render compatibility instead of submission order.
- [x] Compact visible instance IDs and indirect LOD draw arguments on the GPU for camera and shadow passes without rebuilding placement transforms.
- [x] Upload only new or edited placement ranges and changed object metadata; preserve unchanged placement storage during camera movement.
- [x] Bake tile indexes and render-ready instance records with static placement data to avoid runtime regrouping and transform conversion.
- [x] Stream cell instance allocations with reusable capacity, bounded residency, and GPU-fence-safe retirement.
- [x] Make procedural runtime placement generation explicit opt-in and preserve intentionally empty authored layers.
- [x] Update component registries and render-item caches incrementally without invalidating unrelated scene data when tiles enter or leave.
- [x] Maintain active physics work lists and synchronize static colliders only when changed; skip suspended terrain bodies during routine setup and post-step work.
- [x] Record instance-cache miss reasons, buffer allocations, and placement upload bytes by cause and camera/shadow pass.
- [x] Add matched-route movement and streaming benchmarks for upload spikes, frame-time percentiles, and CPU/GPU costs at fixed viewport and quality settings.
- [x] Break down retained UI, portal/overlay work, worker execution, synchronization waits, and record packing in recordings and the Performance view.
- [x] Centralize grass updates with viewer queries over cached terrain-field bounds and retire fields leaving the active region.
- [x] Prepare visibility, fallback LOD classifications, and copied material/object records on workers.
- [x] Retain grass placement buffers and stable member identities; reuse unchanged batches across camera and LOD draws.
- [x] Add GPU grass visibility, LOD classification, complementary fades, and indirect camera/shadow draws across DX11, DX12, and Vulkan.
- [x] Buffer immutable render records and overlap worker packing with graphics setup and GPU culling; keep scene mutation and graphics calls on their owning thread.
- [x] Validate editor frame time, percentiles, main-thread usage, worker utilization, and GPU timings with a warmed full-editor capture.
- [x] Add a fresh-process foliage landscape baseline runner with fixed and moving DX11 routes and separate foliage-off, grass-shadows-off, and full-foliage cases. At 790 x 589 Release, the moving route measured 9.08, 9.99, and 19.37 ms mean intervals respectively over 240 frames per case; a second shadow pair confirmed the shadow cost with run-to-run variation. The 1920 x 1080 foliage-off case measured 33.21 ms and repeated at 33.69 ms. These render-helper captures exclude editor UI and physics; see `.md/foliage-landscape-baseline-20261103.md` for percentiles, GPU stages, and limits.
- [x] Batch compatible grass tiles by species, LOD, material, and render state in camera and shadow passes.
- [x] Cull shadow casters against cascade receiver silhouettes while preserving off-screen shadows and cascade fades.
- [x] Add grass caster distance to shadow quality presets and project preferences.
- [x] Use cached audio source lists and process each source once.
- [x] Use cached light and spatial-volume lists; reuse view visibility and simplify bounds preparation.
- [x] Reuse grass and shadow instance buffers, constants, and CPU staging storage.
- [x] Prepare large instance batches, cascade visibility, and fallback LOD classifications with worker jobs.
- [x] Record shadow submission wall time, active thread cycles, and slowest batches.
- [x] Compare empty, terrain-only, grass without shadows, and full grass with a fixed route, viewport, build, and quality.
- [x] Automatically instance compatible repeated meshes and prefab mesh components in camera and shadow passes.
- [x] Share immutable GPU mesh buffers across duplicate asset, generated terrain, and prefab usage.
- [x] Cache material and render preparation shared across draws, views, and shadow cascades.
- [x] Use component registries and cached update lists for scene-level rig, constraint, portal, tissue, and particle processing.
- [x] Reuse cached transforms and preparation; upload only changed object and instance records.
- [x] Add hierarchical spatial culling for terrain cells, objects, and instance batches.
- [x] Cull instanced shadow batches against each cascade and reuse caster preparation.
- [x] Add GPU instance visibility, optional resident LOD selection, and indirect draws across DX11, DX12, and Vulkan.
- [x] Record separate CPU timings for render culling, sorting, material preparation, data uploads, and draw submission.
- [x] Preserve frame-safe render records and clipping for multiple batched camera views.
- [x] Expose current batching, upload, grass fade, shadow workload, frame-percentile, and process/thread CPU measurements in the Performance view and recordings.
- [x] Add a repeatable capture evaluator with GPU sample deduplication and measurement-scope checks.
- [x] Replace CPU-expanded grass tiles with GPU instances of shared species/LOD meshes.
- [x] Share compact species placement buffers across each tile's LODs; avoid uploads when changing LOD.
- [x] Apply a scene-wide grass preparation/upload budget and retain nearby tiles beyond the visibility radius.
- [x] Record visible grass instance counts, placement upload bytes, and shared GPU memory without double counting.
- [x] Share grass species geometry across terrain cells to avoid repeated source loading.
- [x] Prevent flat terrain triangle edges from kicking the player capsule upward.
- [x] Keep terrain collision active only under the player and nearby edges/corners, with at most four active cells per landscape.
- [x] Move runtime LOD caching, hysteresis, buffer switching, and memory accounting into each object's Mesh component.
- [x] Reduce CPU overhead in DX11 shadow draw submission.
- [x] Cache grass placement data by tile and remove repeated full-placement scans and hashing.
- [x] Switch resident grass LODs without rebuilding or uploading geometry.
- [x] Support gameplay and editor cameras as explicit grass viewers.
- [x] Record grass scanning, tile generation, upload time, pending work, and rebuild counts in Performance recordings.
- [x] Preserve accurate shadow draw counts after the main render pass.
- [x] Reduce Play/Stop scene transition stalls by excluding generated grass geometry and retiring legacy preview objects.
- [x] Extend thread accounting to UI, simulation, rendering, workers, and writer queues; separate recurring frame work from import/save/bake/startup hitches in recordings and benchmarks.
- [x] Move scene/prefab file reads, parsing, snapshot encoding, save writes, and undo-history comparisons onto workers from versioned snapshots; commit live scene changes on the simulation owner and retain edit revision ordering.
- [x] Compile and cache shader bytecode and decode textures on workers; prewarm required variants and queue graphics resource/pipeline publication without blocking render preparation on first use.
- [x] Move CPU particle simulation, surface preparation, skin/morph fallbacks, mesh deformation, and warp sampling onto jobs or existing GPU paths; batch collision requests against a safe physics snapshot.
- [x] Run model/motion import, asset copying/remapping, lighting bakes, and generated-asset writes as cancellable background jobs with progress and bounded result publication.
- [x] Move hot-reload module/symbol copying and fallback filesystem scans off the frame loop; budget build-output processing and publish module replacement only after affected callbacks and jobs finish.
- [x] Schedule preview CPU preparation through the same dependency system as gameplay.
- [x] Decode and preload audio files asynchronously, including the current synchronous OGG path; publish playback commands through the audio owner's queue while preserving real-time mixing.
- [x] Pipeline export readback, image encoding, and output writes with bounded buffers; keep GPU readback operations on the graphics owner and preserve deterministic frame ordering.
- [x] Accelerate editor picking with cached spatial/triangle indexes and worker-prepared skinned geometry; validate asynchronous hover results against camera, pose, and scene revisions.
- [x] Move performance-record formatting and file writes to a bounded ordered writer queue; capture immutable timing data on its owner, report queue overflow, and drain recordings cleanly on stop.
- [x] Replace edit-preview whole-scene scans with typed particle-emitter and animation registries.
- [x] Cache asset-browser directory listings, file metadata, and filtered/sorted search results on workers, refreshed through filesystem notifications with a polling fallback.
- [x] Prepare visible asset thumbnails asynchronously with bounded jobs, private decoding, stale-result cancellation, and graphics-owner publication.
- [x] Fit directional shadow cascades in camera-local space with double-precision projection rays and exact slice depths. Keep radius and world texel-grid phase stable under fixed-position yaw/pitch and translation; record cascade radii.
- [x] Fix camera-sorted shadow record uploads to cover the highest changed persistent slot. Add DX11 GPU readback checks for moving/rotating cameras, one/four cascades and ordinary/native packets; record shadow enable, cascade and atlas state for future captures.
- [x] Add real shadow-receiver fade coverage and repeated-frame pixel checks for ordinary/native-recorded rendering on DX11/DX12/Vulkan. Fix the small fixture's receiver normals and depth bias so it measures visible shadows.
- [x] Restore DX12 standalone game color/depth targets, viewport and scissor after depth-only shadow passes.
- [x] Replace grass tile shadow-distance toggles with GPU distance fading and per-instance shadow-range culling while retaining resident placement buffers. Reusable meshes expose transient caster range/fade settings; color-pass visibility stays independent.
- [x] Add timings within render-item preparation for material/shader preparation, mesh state, bounds, LOD metadata, and draw-item construction. Baseline: 9.87 ms foliage-only versus 0.14 ms terrain-only at 1920 x 1080 in Debug DX11.
- [x] Cache prepared rigid instanced render records, including foliage. Invalidate on geometry, placement, or transform edits; refresh material state, fades, and GPU LOD parameters on reuse. Preserve full preparation for CPU LOD transitions, deformation, previews, and portal/warp paths.
- [x] Read prepared transform revisions during the stable render phase so cached records avoid repeated parent-chain validation.
- [x] Share shader source resolution and file checks across materials using the same authored path and terrain/mesh variant each frame; retain per-frame hot reload and direct script edits.
- [x] Cache normalized material validation inputs and resident texture preparation by texture/provider state. Detect direct script edits, pending uploads, invalidation, replacement, and provider changes without relying solely on revision notifications.
- [x] Reuse resident foliage placement buffers with GPU instance visibility, LOD selection and indirect counts. Register each source once per view, cache camera-independent tile records, and reuse them across LODs and shadow cascades; keep coarse CPU tile rejection and explicit CPU fallback paths.
- [x] Build changed instance placement and tile render records from immutable snapshots, with worker-local output for large batches and owner-side publication/upload. Preserve retained-reader lifetimes and skip worker scheduling for unchanged data and small edits.
- [x] Isolate remaining ordinary material/object record assembly from mutable material, probe and graphics-resource caches before expanding worker preparation beyond the instance record paths.
- [x] Reduce average foliage render-item preparation below 2 ms and validate 100+ FPS in the stationary full editor/gameplay loop at 1920 x 1080 with unchanged visual quality. Debug DX11 result: 0.68 ms item preparation, 162.0 FPS, frame P95 7.12 ms.
- [x] Record UI construction by panel in the Performance view, recordings and evaluation reports, including panel totals and remaining editor UI work.
- [x] Skip clipped percentage-grid, history-graph and asset-thumbnail drawing while preserving layout and interaction geometry.
- [x] Partition command recording into compute, scene snapshot/resource retention, view recording and remaining bookkeeping; expose the breakdown in Performance view, recordings and evaluation reports.
- [x] Cache directional-only clustered-light grids per GPU frame-slot buffer and layout. Skip recurring grid construction/uploads; invalidate on local lights, layout changes and buffer replacement.
- [x] Replace recurring shader-source resolution/checks with change-driven invalidation while retaining hot reload, direct path edits and missing-file recovery (latest preparation cost: 0.33 ms).
- [x] Partition command recording into compute, scene snapshot/resource retention, view recording and remaining bookkeeping; expose the breakdown in Performance view, recordings and evaluation reports.
- [x] Cache directional-only clustered-light grids per GPU frame-slot buffer and layout. Skip recurring grid construction/uploads; invalidate on local lights, layout changes and buffer replacement.
- [x] Replace recurring shader-source resolution/checks with change-driven invalidation while retaining hot reload, direct path edits and missing-file recovery (latest preparation cost: 0.33 ms).
- [x] Implement true tile/page-based directional shadow updates (dirty-region invalidation + per-page redraw) instead of whole-atlas refresh in [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp).
- [x] Add explicit shadow caster budgeting (max casters per cascade + priority scoring by distance/screen size/importance) in [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp).
- [x] Add far-distance shadow replacement for foliage/trees (proxy casters + optional no-shadow beyond strict cutoff) wired through [Mesh.h](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Compoonents/Obj/Mesh.h) and [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp).
- [x] Add HLOD/impostor pipeline for distant vegetation clusters and integrate with terrain streaming in [TerrainManager.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Compoonents/Obj/TerrainManager.cpp).
- [x] Add distance-based material/shader quality tiers (disable expensive features by band: normals/parallax/subsurface/reflections/shadow filtering) in [LightingData.h](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Model/LightingData.h) and [Object.hlsl](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Shaders/Object/Object.hlsl).
- [x] Add dynamic internal render scale / temporal upscaling mode for heavy scenes with quality target controls (75%/67%/50%) across renderer backends.
- [x] Add hard scene budgets and runtime diagnostics (visible triangles, shadow casters, overdraw proxies, material complexity counters) with alerts in [PerformanceView.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Editor/Core/View/Views/PerformanceView.cpp).
- [x] Lower shadow resolution scale clamp floor in [RealtimeLightingPipeline.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Rendering/Lighting/Pipelines/Realtime/RealtimeLightingPipeline.cpp) so scene-authored values below `0.25` can actually take effect for aggressive perf presets.
- [x] Add/verify project-level low-performance shadow preset tuning in [LightingData.h](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Model/LightingData.h) (directional resolution, cascade count, directional distance, grass caster distance, PCF radius) for large foliage scenes.
- [x] Optimize PCF shader path in [Object.hlsl](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Shaders/Object/Object.hlsl) by specializing small-radius kernels and removing unnecessary loop/branch work per pixel.
- [x] Add shadow-caster proxy workflow for heavy foliage/terrain (low-poly or primitive caster meshes) and route runtime shadow pass to proxies where enabled.
- [x] Investigate temporal/tiled shadow update strategy to avoid full cascade redraw every frame in [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp).
- [x] Revisit terrain shadow path in [SceneRendering.cpp](C:/Users/tyler/source/repos/EngineProject/Engine/Core/Scene/Rendering/SceneRendering.cpp) to reduce forced double-sided shadow cost where safe.

- [x] Add generic distance texture LOD controls to the built-in renderer: scene defaults, per-material overrides, adjustable transition distances and mip bias, and smooth per-fragment transitions. Oak uses 12-48 m and up to +4 mip bias.
- [x] Fade and skip distant normal, metallic/roughness, occlusion and parallax detail, and fade environment reflections. Save controls in scene and material assets.
- [x] Blend direct lighting from PBR to diffuse-only at distance while retaining light attenuation, photometry and realtime shadows.
- [x] Investigate texture streaming or limiting resident mip levels to reduce GPU memory usage; see `texture-residency.md` and `Scripts/Assets/audit_texture_residency.py`. Oak RGBA8 mip payload estimates: 288 MiB full, 96 MiB at 2K, 24 MiB at 1K.
- [x] Implement an opt-in static resident texture dimension cap (`ENGINE_TEXTURE_MAX_DIMENSION`), preserving CPU/editor source pixels and existing panorama limits.
- [x] Validate capped foliage alpha coverage on DX11, DX12 and Vulkan: 2K and 1K caps match baseline silhouettes at three distances with shader LOD off/on. Validate dedicated GPU allocations on DX12 and Vulkan; see `texture-residency.md`.
- [x] Normalize generated and cached terrain winding and enable top-only color/shadow culling. A DX11 image test verifies that the top remains visible and the underside is culled, overriding double-sided materials.
- [x] Extend terrain culling regression checks to streamed-cell/far-LOD seams and directional shadows across DX11, DX12 and Vulkan. Fixed Vulkan atlas preservation; all three synthetic regressions and actual oak entrance/overlook checks pass. See `terrain-culling.md`.
- [x] Reject masked pixels before normal and metallic/roughness sampling. Keep mask coverage at hardware-selected quality while biasing color mips so distant leaves do not disappear.
- [x] Restore the flat scene grass in oak as a terrain-child prefab, matching triangle heights and normals, excluding the painted footpath, and retaining nearby tile streaming and LODs. DX11 validation: 22,277 placements, 70 active tiles and 2,700 resident instances at the entrance.
- [x] Convert the 2,048 oak trees to shared terrain prefab placements and remove tree colliders/rigid bodies. The object brush stores prefab references; base-prefab collider edits propagate to configured LOD variants. Headless sharing and propagation checks pass; see `terrain-prefabs.md`.
- [x] Add runtime visual LOD selection to shared terrain prefab trees as the player/camera moves, replacing the forest's starting-camera LOD assignments. Reuse shared resident geometry and placement records, add hysteresis or fades, and integrate very cheap far meshes or impostors. Verify camera and directional-shadow behavior across DX11, DX12 and Vulkan. Implemented resident near/middle/impostor bands, dither fades and shared placement records; camera and visible directional-shadow checks pass on all three backends. See `tree-runtime-lod.md`.

- [x] Avoid rebuilding placement groups during unchanged one-second prefab polls. Check signatures/LOD definitions and cached distinct source identities/timestamps first; reuse resolved paths and detect same-timestamp overrides. In 1,200-frame Release DX12 captures, refresh-event mean drops **6.95 to 2.74 ms** (60.5%); range **6.47–8.43 to 2.68–2.82 ms**. Final Debug capture measures **6.21–7.09 ms** at 1080p. Debug/Release builds and invalidation regressions pass, including in-place placements/LOD chains, LOD-only file edits, collider changes, fallback and disappearance/reappearance. Final DX11/DX12/Vulkan forest checks pass. Evidence: `.local/performance-logs/prefab-refresh-comparison.md` / JSON. No authored quality changes or claim of sustained 100+ FPS.
- [x] Avoid world-preserving reparenting and broad registry reconciliation when constructing new layer-local prefab batches. Use existing local parenting; verify identity child transforms under rotated/nonuniformly scaled layers and retain runtime ownership. Matched Debug 1080p transition captures reduce cold prefab preparation **6.62 to 3.01 s** and the first stopped frame **11.13 to 7.45 s**. Runtime-batch construction falls **4.56 to 0.90 s**. Release's change is modest (**1.214 to 1.163 s** cold preparation); do not extrapolate Debug gains. Both configurations and DX11/DX12/Vulkan forest checks pass. Opt-in prefab profiling now exposes construction stages. Evidence: `.local/performance-logs/prefab-cold-comparison.md` / JSON.
- [x] Split cold source geometry into copy/expansion, transform, validation/bounds and buffer-creation scopes; hoist the invariant tangent matrix out of the vertex loop. Short Release DX12 capture: last cold build spends 526.9 ms in buffer creation, 117.0 ms validation/bounds, 35.9 ms copying and 15.2 ms transforms across 85 sources. Buffer creation includes hashing/cache lookup and upload; this does not isolate GPU transfer time. Evidence: `.local/prefab-geometry-release-stages.log` and `.local/performance-logs/prefab-geometry-release.jsonl`. No measured FPS gain claimed.
- [x] Split shared-buffer costs and reduce serial content-hash work. Mix eight-byte words with safe unaligned reads and retain full byte equality checks. Matched short Release DX12 protocol: cold 85-source hashing **144.45 to 20.63 ms** (85.7% reduction), buffer creation **325.40 to 214.12 ms**; backend creation **150.62 to 158.43 ms**. No steady-play FPS claim. Evidence: `.local/performance-logs/mesh-buffer-comparison.json` and before/after stage logs. Cache regression covers unaligned inputs, tails, identical reuse and every changed byte for sizes 1-63.
- [x] Separate DX12 static-buffer allocation/staging copy, command recording, submission and fence wait costs with opt-in profiling. Cold Release 85-source capture: **91.93 / 23.47 / 5.88 / 21.10 ms** respectively, within 170.84 ms backend creation. Allocation/copy dominates the measured children; do not attribute the entire backend cost to waits. Evidence: `.local/dx12-upload-stages.log`, `.local/performance-logs/dx12-upload-breakdown-release.jsonl`.
- [x] Split DX12 destination/staging allocation/data copy and reuse bounded staging scratch after completed uploads. Retain at most one 16 MiB buffer per factory; larger uploads remain temporary, and concurrent uploads never share in-flight scratch. Matched short Release cold capture: staging allocation **30.07 to 2.70 ms** across 85 sources (one fresh allocation after reuse); backend creation **179.31 to 124.52 ms**. Other scopes vary; do not attribute the whole backend change to reuse or claim steady FPS gains. Evidence: `.local/performance-logs/dx12-staging-comparison.json`, before/after captures and stage logs.
- [x] Instrument Stop pipeline drain, background cancellation/fencing, editor restoration, object teardown and reconstruction. Short Release DX12 capture: restoration root **343.09 ms**, pipeline drain **11.38 ms**, cancellation/fence **0.02 ms**, scene restoration **331.49 ms**; nested clearing **33.19 ms**, reconstruction **237.39 ms**. Parsing/source-snapshot scopes are nested, not additive. Evidence: `.local/stop-restore-stages.log`, `.local/performance-logs/stop-restore-release.jsonl`. This does not explain the prior Debug-only 4.1 s outside rendering.
- [x] Read prefab files once with a sized binary read; parse XML directly when identified, preserving JSON and XML encoding fallback. Release restoration read/parse **111.70 to 74.91 ms**, reconstruction **237.39 to 203.12 ms** in matched short protocol; total restoration **343.09 to 326.18 ms** with varying teardown. Format regression covers JSON, whitespace XML, UTF-8 BOM, UTF-16 XML, empty/malformed files. Evidence: `.local/prefab-read-once-stages.log`, `.local/performance-logs/prefab-read-once-release.jsonl`. No steady FPS claim.
- [x] Split source-snapshot capture from encoding and use compact JSON for internal prefab baselines. Preserve existing pretty document output, numeric precision and string escaping. Short matched Release protocol: encoding **32.77 to 20.06 ms** (38.8% lower), capture **56.46 to 55.42 ms**, parent snapshot **98.66 to 83.82 ms**. Compatibility regression checks pretty output, nested round trips, precision/escaping and actual prefab override detection/revert. Evidence: `.local/performance-logs/source-snapshot-comparison.json`, before/after capture and stage logs. No steady FPS claim.
- [x] Fix recurring preparation path-resolution spikes and expose missing timing scopes.
- [x] Audit all 17 oak varieties and verify independent GPU LOD selection and resident placement reuse on DX11, DX12 and Vulkan.
- [x] Evaluate 55 m impostors. Reject the visibly flatter middle-distance canopy and retain the authored 90 m transition.
- [x] Put static DX12/Vulkan vertex geometry in GPU-local memory and enable optimized DX12 Release shaders.
- [x] Remove CPU comparisons against mapped GPU upload memory. Compare a CPU mirror paired with each frame/pass buffer, including shadow records, resizing and external writes. Combined record packing dropped from ~28-33 ms to below 0.4 ms before grouping changes.
- [x] Reduce generated forest CPU mesh components from thousands to 340 using coarse 256 m groups. Keep all 2,048 placements, 85 shared sources and per-placement GPU culling/LOD. Real forest captures/shadows and LOD regressions pass on DX11, DX12 and Vulkan; all 10 checks pass.
- [x] Validate optimized 1080p performance **with sun shadows disabled**. Full Editor Play: 600 measured frames, **144.70 FPS**, P95 **8.17 ms**, P99 **13.92 ms**. Moving camera: 1,200 measured frames, **144.54 FPS**, P95 **15.06 ms**, P99 **17.82 ms**. Static diagnostic views average 151/140/123 FPS with P95 below 10 ms. See `.md/forest-performance.md` for reproduction and limits.
- [x] Restore authored forest sun shadows and normal resolution/filter scales in both the scene and generator. Require authored shadow selection and casters in DX11/DX12/Vulkan capture tests; all three pass. Terrain receives shadows without casting them.
- [x] Apply cheaper shadow LODs to independent prefabs and synchronize GPU selection with fragment fades; preserve visible 28/90 m LOD distances.
- [x] Reduce shadow filter reads using atlas-clamped gathers, skip unused direct-light shadow evaluation, and tighten static shadow caster bounds without shrinking wind-deformed color bounds.
- [x] Separate shadow/color tile-cache entries to prevent alternating LOD policies from rebuilding roughly 176 static tiles each frame.
- [x] Stagger moving-camera cascade refreshes: prioritize the near cascade, rotate farther tiles, preserve atlas contents, and fully refresh on camera cuts or projection footprint changes.
- [x] Correct standalone DX12 color capture timing and reject blank captures; disregard earlier blank shadow diagnostic images as visual evidence. All 10 final Release checks pass, including motion/cut/zoom invalidation on DX11, DX12 and Vulkan; three corrected 1080p captures contain the rendered forest.
- [x] Pin forest shadows to one LOD1 proxy with camera-independent leaf cutouts and no shadow LOD crossfade. Keep visible 28/90 m LODs, full 2048/four-cascade atlas and 90 m caster range. Add an inspector/serialized layer setting and shorter-chain fallback. Shadow draws per refreshed cascade fall from 85 to 34.
- [x] Remove look-direction dependence from directional shadows. Use camera-centered radial cascade coverage, radial receiver selection, a world-snapped light texel grid and full light-space caster bounds (including off-screen trees). Retain staggered tiles only while they cover the current receiver sphere. Actual fixed-receiver side/back look comparisons pass on DX11/DX12/Vulkan, alongside sphere coverage, upstream caster and texel-phase tests. All 12 Release checks pass; see `.md/forest-performance.md`.
- [x] Restore asynchronous DX12 Editor GPU timestamp resolution. Bind the submission command list and call context-factory `FinalizeFrame()` before closing it; consume readback after frame-slot fences. New 1,200-frame moving Release run has 1,200 unique valid GPU samples: opaque **6.95 ms**, shadows **2.13 ms**, grass **0.53 ms**, terrain **0.14 ms** on average. These measured GPU scopes total **9.75 ms** and overlap CPU work; they exclude uninstrumented work.
- [x] Isolate forest color costs with controlled shadows-off static cases. At 1920x1080 entrance: leaves **5.21 ms GPU**, bark **1.12 ms**, trees **5.89 ms**; at 960x540 leaves still cost **3.40 ms**. These are separate visibility experiments, not additive contributions; resolution also affects mip/depth behavior. Leaf rendering dominates, with significant work remaining at low pixel counts. Reports: `.local/forest-opaque-baseline/report.md` and `.local/forest-full-coverage-dither/report.md`.
- [x] Skip the geometry-fade dither hash when the coverage interval spans all possible hash values. Preserve partial LOD/range fades and authored assets/materials. Isolated tree/bark/leaf/terrain captures are byte-identical. Latest 1,200-frame shadow-enabled 1080p Release route: **105.62 FPS**, **9.47 ms average**, P95 **11.55 ms**, P99 **17.41 ms**, 1,200 resolved GPU samples and zero streaming GPU drains/queue waits. The route is variable-time and short static timings are mixed; do not attribute the full observed difference to this shader change. Validation: **11/12 pass**; the DX12 diagnostic shadow-toggle failure reproduced at that stage and is addressed by subsequent correctness fixes.
- [x] Remove moving-route GPU drain spikes for streamed built-in render leaves. Detach runtime Mesh/Material leaves immediately and retain their complete ownership graph through the existing submission-fence retirement queue. Unknown components, scripts, authored objects and subtrees keep the synchronous path. The 1,200-frame Release route records zero simulation GPU waits (previously 178), **96.32 FPS**, P95 **12.46 ms**, P99 **17.35 ms**. Added lifecycle, inactive-LOD ownership, fallback and shutdown coverage. See `.md/forest-performance.md`.
- [x] Fix the recurring DX12 shadow-toggle capture failure using API/GPU validation. Set triangle topology for indirect/indexed draws; match new atlas initial state to the depth-pass barrier; transition standalone swap-chain buffers to PRESENT; explicitly branch inactive instance-buffer paths; bind a same-heap null SRV for empty textures instead of leaving root arguments stale/uninitialized. GPU validation previously caught a 48-byte placeholder instance-buffer overread and an uninitialized shadow-map root argument. The corrected repro passes three consecutive ordinary runs and all **13 Release checks**, including a permanent GPU-validation test (skips with code 77 only if the debug layer is unavailable). Logs/report: `.md/forest-performance.md`.
- [x] Correct small-sun-motion shadow invalidation. The real DX12 forest repro refreshed only two of four cascades after a 0.05-degree sun step. Tighten direction cache reuse to normalization roundoff and refresh all cascades on meaningful sun changes; retain reuse for unchanged lighting. Expose the actual refreshed-cascade mask in frame traces. Regression passes on DX11/DX12/Vulkan, including three successive sun changes and restoring the sun; DX12 GPU validation and radial coverage/texel-phase checks pass. Release Editor builds. Full walking receiver-patch validation remains open.
- [x] Build Vulkan material wrappers whose entry points come from included shaders. Follow local quoted includes with cycle protection for entry-point discovery and build dependencies; track include-content changes for CMake regeneration. This generates the previously missing `GrassFootBend.hlsl` vertex/pixel/terrain variants. The real Vulkan forest regression now passes; shader failures in the harness include the actual compiler error.

- [x] Extend real-forest walking coverage checks to matched frozen/animated paths. Each backend validates 24 frames per phase, retained receiver coverage, bounded refresh age and fixed radial-probe crossings at all four splits. Save immediate single-frame captures and camera/cascade traces without intervening warmup frames. DX11/DX12/Vulkan and DX12 GPU validation pass; all 144 saved trace frames retain tiles, minimum coverage clearance is 0.728 m and maximum cascade age is two frames. All 36 images have valid dimensions and nonuniform content. Evidence: `.local/tree-lod-oak-validation/walk-validation.json`. Reprojected terrain-patch/filter/blend/caster-fade validation remains open; these elevated-view geometric probes do not establish visual temporal stability.

- [x] Add fixed-terrain temporal measurements with matched unshadowed controls. New `OakGroundCapture_*` cases retain the complete forest shadow pass while isolating terrain color; export world-space terrain geometry and use ray-checked reprojection plus bilinear 3x3 patches in `Scripts/analyze_shadow_ground.py` (numpy). All three backend captures pass, including the corrected DX11 path; DX12 terrain isolation passes GPU validation and all five analyzer tests pass. Each backend yields 256 visible samples, 34 shadowed samples and three receiver-band crossings. Seven frozen-wind samples vary by more than eight RGB levels despite stable unshadowed controls. Measurements and worst world positions are in `.local/tree-lod-oak-validation/*-ground-analysis.json`; do not mark temporal visual stability complete.
- [x] Fix frame identity for DX11 diagnostic screenshots. Add opt-in pre-Present RGBA8 scene capture and synchronous readback, reset on resize and reject reads until a requested composition completes. Verify a red/green/blue sequence and use the new request/read API throughout the oak harness. Legacy post-Present swap-chain reads are not valid evidence for exact walking-frame comparisons. Release Editor/harness build; DX11 terrain capture regression passes.
- [x] Attribute and fix the accumulated-depth component of the seven frozen-wind outliers. Partial atlas updates did not clear refreshed tiles. Add regional depth clearing on DX11/DX12/Vulkan, full-refresh fallback for unsupported contexts, capability forwarding and replay in recorded command packets. Six matched phases verify layer-based 90 m versus 100,000 m diagnostic ranges, retained/full updates, repeats and fixed wind time. At the overlook route's 256 samples, extended-range retained/full errors are 0.0242 RGB on DX11 and zero on DX12/Vulkan. Authored-range excursions above eight levels drop from seven to two; both remain with full refresh at 90 m and disappear at extended range. All backend capture/reference checks, DX12 GPU validation, eight analysis checks and packet replay regression pass; Release Editor builds and a 30-frame normal Play smoke retains partial updates. No FPS claim or authored quality change. See `.md/forest-performance.md`.
- [x] Expose serialized prefab shadow fade width on terrain layers, preserve the legacy 1 m default and propagate it through streaming and generated meshes. Add matched 4/8 m frozen-wind captures with full/retained references and effective-width assertions. GrassField retains its separate existing distance fade control.
- [x] Correct the animated-walk validation scope: authored oak meshes have zero wind, and shader wind is intentionally excluded from shadow passes. Record effective wind-enabled oak counts and add a fixed-camera diagnostic that enables runtime wind, proves visible oak movement and checks fully refreshed ground shadows remain stable. Preserve authored assets and the existing static shadow policy.
- [x] Add a separate authored entrance walking route alongside the existing elevated overlook route, with separate artifacts and explicit route identity checks. Run identical fade/range/refresh/wind-policy diagnostics; report control-stable shadowed samples for every phase. DX12 entrance results retain four stable-control excursions even with full refresh and extended caster range, so the overlook's range attribution is not universal.
- [x] Compare fixed terrain-plane footprints against display-space patches at identical accepted centres and common stable-control samples. Add visibility/triangle checks, projected minor-axis diagnostics and world-analysis CTests for both routes/all backends. At a 0.25 m radius, 75/77 entrance patches become subpixel in one direction and none of DX12's 12 common-control samples remains at least one pixel wide throughout. Variation persists; the saved grazing-view images do not establish a renderer defect. Retain both measurements and current quality defaults.
- [x] Add matched 1920x1080 entrance captures with bounded capture scale and separate artifacts. Reuse the exact 77 reference terrain patches; reject mismatched route, camera/projection/time, aspect, caster policy, wind counts and terrain geometry. All three backend capture/comparison pairs pass. DX12's fully refreshed baseline excursions fall from ten to seven, but 71/77 patches remain subpixel in one direction. Keep the 1 m default; capture execution is not a performance benchmark.
- [x] Add process/build/backend provenance for performance comparisons. Record executable path, exact CMake configuration and assertion state in recording sessions and frame details; include the active backend and renderer scale/dynamic-scale/upscaling settings per frame. Add `--renderer` to the capture script. Release Editor builds; recording regression passes in Debug/Release; short DX11/DX12/Vulkan Edit/Play/Stop captures validate all 66 frame identities per backend. Evidence: `.local/performance-provenance/validation.json`. The older 20:30 capture reconciliation and sustained-performance acceptance remain open; these short checks establish metadata correctness only.
- [x] Fix the last toolbar section refusing to expand: clamp divider and Shift-grip resizing to the containing toolbar region, rather than the group's current requested span. Layout allocation still prevents neighboring groups from overlapping. Interactive verification remains pending.
- [x] Analyze user capture `performance-20261107-205641-769.jsonl`. Confirmed Debug/assertions/DX12, 2560x1369, scale 1, four cascades/2048 atlas. Count `frame_details` only (summary `frame` records duplicate updates): 1337 Play updates average **33.13 FPS**, interval P95 **38.57 ms**, P99 **48.62 ms**. After explicit first-120 Play warmup exclusion: **33.33 FPS**, P95 **38.43 ms**, P99 **45.71 ms**. No scene Stop transition. Report/JSON are adjacent to the capture; do not compare directly with Release 1080p or unmatched older routes/assets.
- [x] Instrument grass Refresh with exclusive reference/source/placement/visibility/retirement/scheduling/build/accounting/other phases, export them to JSONL and the Performance panel, and publish timing on early-return paths. Existing scan/generation/upload scopes remain nested diagnostics. Debug and Release Editors build; regression and 600-measured-frame captures pass in both configurations. Phase sums cover all 752 records per capture (Debug maximum rounding residual 0.0005 ms); pre/post primary scene/grass hashes match. Measured maxima are Debug 1.2232 ms and Release 0.4625 ms; these automated 1080p routes do not reproduce the user's 40-49 ms spikes. Evidence: `.local/performance-logs/grass-phases-*.json*`. No FPS gain claimed.

- [x] Mark Play-start transition work with `play_start` activity and `recurring_frame=false`: tag the request, pending-start frames, scene Start, first following simulation/recording update and the subsequent sample carrying its update interval. Clear the followup on Stop. Debug/Release Editor builds and recorder tests pass. Zero-warmup 60-frame captures in each configuration tag the first three Play records and leave the remaining 57 recurring; Debug startup work **202.12 ms** and its delayed interval **203.83 ms** are both excluded from recurring selection. Evidence: `.local/performance-logs/play-start-final-{debug,release}.jsonl` and `play-start-validation.json`. Keep transition-inclusive and explicitly warmed summaries distinct.

Target: sustained **100+ FPS** in the 2,048-tree forest at **1920x1080** on the current machine using an optimized build. Budget **10 ms per frame**; track average, P95 and P99 separately. Latest DX12 correctness run: **100.26 FPS average**, P95 **12.42 ms**, P99 **17.48 ms**. The preceding fade-fast-path run averaged 105.62 FPS; route coverage is variable-time and not matched. Sustained 100+ FPS with shadows remains the priority.
- [x] Remove implicit deep-copy returns from terrain and placement-layer serialization: finish `Set` calls, then return the local value directly. `Set` returns an lvalue reference, which previously copied the completed tree. Short Release cold capture: value-tree capture **55.42 to 35.17 ms** (36.5% lower), snapshot parent **83.82 to 64.84 ms**, encoding similar (**20.06 / 20.83 ms**). Evidence: `.local/performance-logs/source-capture-move-comparison.json` and `.local/source-capture-move-stages.log`. Serialized values and deep-copy ownership remain unchanged; no steady FPS claim.
- [x] Audit other named-local chained serialization returns in `Engine/Core`. Remove 12 implicit deep-copy return paths across 11 files: animation clips/layers/model nodes/rig morphs/camera-light tracks, skeletons, skin weights, material morphs, mesh morphs (both branches), cloth anchors and terrain-manager state. Preserve values, order and ownership; follow-up search finds no remaining named-local `return value.Set/Push(...)` expressions. No asset-specific timing or forest FPS improvement claimed for this audit.
- [x] Repeat restoration profiling in Debug once linking becomes available. Current 1080p DX12 short capture: Stop callback **3.262 s**, reconstruction **2.195 s**, prefab read/parse **0.952 s**, source capture **0.747 s**, clearing **0.107 s**. Nested scopes overlap. First stopped frame **6.484 s**, including subsequent render **2.991 s** and prefab preparation **2.673 s**. Evidence: `.local/stop-current-debug-stages.log`, `.local/performance-logs/stop-current-debug.jsonl`. The historical 4.1 s gap is not a matched current measurement.
- [x] Evaluate reserving terrain/placement JSON arrays. Reject the unproven candidate: Debug source capture **747 to 738 ms**, reconstruction **2.195 to 2.201 s**, first stopped frame **6.484 to 6.720 s**. Capacity overload and call-site changes were removed. Evidence: `.local/performance-logs/array-reserve-debug-comparison.json`. Do not claim a gain or extrapolate this experiment to Release.
- [x] Remove nested placement-record copies in `TerrainObjectLayer::Serialize`: move completed position/normal arrays and placement objects into their destination values. Final Debug standalone regression completes directly, including all 2,048 forest placements, 85 sources and 340 batches. No timing gain measured for this candidate yet; repeat the Debug capture when its Editor executable can be relinked.
- [x] Move cold worker prefab documents into the owner cache through existing `PreparedPrefab::Publication`, avoiding the worker-cache result copy and owner-cache insertion copy. Retain worker parsing, owner-thread component construction, the eight-entry owner cache and metadata invalidation. Release read/parse **1112.31 to 968.10 ms**, reconstruction **2419.94 to 1998.47 ms** in the latest pair; do not compare directly with earlier small-document results. Current authored grass prefab is modified and **113,905,987 bytes**; it was preserved. Evidence: `.local/performance-logs/prefab-transfer-comparison.json`, capture/stage logs and post-capture input metadata/hash JSON. No steady FPS claim.
- [x] Automatically record scene/prefab input identities before capture and recheck afterward in `Scripts/profile_tree_memory.py`. Discover literal XML/JSON scene/prefab references recursively with cycle handling; record resolved paths, sizes, timestamps and SHA-256, plus the executable/layout seed and repeatable `--input` files. Retain `.inputs.json` on capture failure, reject changed/incomplete input identity for comparisons, and leave historical manifests untouched in summarize-only mode. Seven tests pass under `PerformanceInputIdentity`. Live 30-frame Release validation fingerprints 55 files with complete, unchanged pre/post identity: `.local/performance-logs/input-identity-validation.inputs.json`. Scope excludes implicit mesh/material/texture/script dependencies unless explicitly supplied. Older post-capture-only metadata remains historical, not a pre-capture hash.
- [x] Remove temporary owned XML input strings during scalar classification and encoded numeric-array parsing. Borrow pugixml storage and retain independent ownership for JSON strings, including large baked placement payloads. Add coverage for a 1 MiB payload after XML document destruction, booleans, numbers, overflow/partial numeric text, empty strings and numeric arrays. Debug/Release Editors build; shared-prefab regression evidence is in `.local/xml-borrow-*-test.log`. Authored grass asset preserved; no timing or FPS gain claimed.
- [x] Append ordinary JSON string spans in bulk while preserving escaping and exact pretty/compact output. `JsonWriterCompatibility` passes in Debug/Release; both Editors build. Isolated Release 16 MiB payload writing **28.679 to 14.355 ms**, Debug **133.023 to 52.110 ms**; Debug dense-escape case is slightly slower (**4.780 to 5.353 ms**). Short Release/DX12 1080p pair matches all 54 non-executable fingerprinted inputs: Stop baseline encoding **336.713 to 286.592 ms** (14.9% lower), callback **2394.48 to 2336.12 ms**. Other stages vary; this is one pair, with no steady FPS claim. Evidence: `.local/performance-logs/json-span-comparison.json`, input manifests, capture/stage logs and `.local/json-span-*-benchmark.log`. Grass asset preserved.
- Added bundled-oak bark/leaf/card/other-opaque GPU timing categories, opt-in
  asynchronous DX11/DX12 VS/PS/IA counters, and transient classification for
  transformed shared-prefab meshes. Preserved aggregate `gpu_opaque_ms`.
- Added fixed-geometry resolution, unlit alpha-tested, zero-cutoff and near-LOD1
  controls, with 32-frame case warmup. Added executable/content/driver/settings
  provenance and recording-integrity gates to static matched comparisons.
- Validated 936 measured frames on each of DX11, DX12 and Vulkan, plus six Python
  comparison regressions and Release audit/editor-validation builds. Vulkan
  counters and independent vertex/fragment elapsed cost remain open; see
  `.md/forest-performance.md` for evidence and limits.
- Closed the opaque GPU profiling todo. Added Vulkan pipeline statistics with
  physical-draw query scopes across render-pass pauses, plus zero-fragment
  bark/leaf controls and counter-verified vertex/front-end versus incremental
  raster/fragment cost reports on DX11/DX12/Vulkan.
- Completed moving-route provenance and comparison gates, including loaded
  binaries, content, external inputs, quality/settings and exact measured
  camera trajectories under opt-in deterministic simulation steps.
- Release audit/Editor builds pass. Validated 1,080 static frames per backend,
  four cost pairs each, two matching 1,080-frame moving routes and 20 Python
  regressions. Supersedes the earlier entry's open Vulkan and cost-isolation
  status. See `.md/forest-performance.md` for costs and measurement limits.
- [x] Add unified, serialized graphics quality presets with independent controls for geometry LOD, vegetation/view distance, texture quality, material quality, shadows and render scale. Separate asset-authored limits from renderer policy and user overrides. Expose effective settings in the editor and performance logs; validate matching behavior on DX11, DX12 and Vulkan. Preserve current authored quality as the reference preset.
- [x] Generalize screen-size-based mesh LOD selection beyond the intermediate-canopy experiment above. Use projected bounds with camera FOV/zoom and viewport resolution accounted for, transition hysteresis and conservative wind bounds. Keep explicit distance overrides and independent shadow LOD policy. Compare ground/elevated views, zoom, camera motion and multiple render scales before replacing the forest's authored 28/90 m transitions. Implemented and validated; see [screen-mesh-lod.md](screen-mesh-lod.md). Forest defaults retained.
- [x] Audit automatic texture mip selection and filtering across all three backends. Verify complete mip chains, derivative-based sampling, anisotropic filtering and existing distance mip-bias controls; distinguish texture sampling quality from actual resident memory savings. Check foliage alpha coverage and shimmering through transitions and oblique views; retain alpha cutouts in color and shadow passes unless a replacement passes coverage comparisons.
  Completed: [texture sampling audit](texture-sampling-audit.md). Synchronized Debug captures pass 162 static mask pairs, 1,080 camera/wind motion pairs, 54 isolated rendered-shadow pairs and 108 static-caster wind comparisons across DX11/DX12/Vulkan. Existing bias reduces measured RGB variation without changing masks. DX11 uses 8x anisotropy; DX12/Vulkan use trilinear filtering. A separate 45-degree sun stress case loses both masked and opaque shadows at the far 80-degree view on all three APIs; the audit records this coverage finding and its failing reproducer. Alpha cutouts and runtime defaults remain unchanged; no replacement is approved.
- [x] Profile texture residency and design budgeted mip streaming if memory pressure justifies it. Report resident/requested bytes, mip demand, upload volume and evictions; select demand from projected texture footprint with a memory budget, prefetch and eviction hysteresis. Avoid synchronous GPU drains during uploads/retirement and validate rapid turns, teleports and limited-memory cases. Evaluate virtual texturing only after conventional mip streaming has a measured limitation.
  - Implemented opt-in conventional mip streaming on DX11/DX12/Vulkan, projected UV-footprint demand, budgeted admission, prefetch/temporal holds, small cutout fallbacks, asynchronous upload publication, frame-safe retirement and descriptor recycling. All 12 targeted Release tests pass, including 4,860 budget-constrained route frames, 27 settled/72 transient cutout comparisons, offscreen caster demand and 3,600 descriptor replacements; the synchronized oak regression passes 54 masks and all six supported allocation gates. Enable with `ENGINE_TEXTURE_STREAMING_BUDGET_MB`; retain the full-quality default because the isolated reference test did not establish pressure. Virtual texturing is deferred. See [measurements and validation](texture-streaming-budget.md).
- [x] Profile material costs separately from texture size and triangle count: count material slots/draws, shader variants and texture reads, and measure GPU pass time in controlled comparisons. Evaluate cheaper material variants for small/distant objects using existing shading-simplification controls, preserving alpha coverage, wind and lighting continuity. Lower texture resolution alone must not be reported as reduced shader complexity or draw count. Completed: [material cost study](material-costs.md). Nine alternating-order Release processes at 1080p with shadows across DX11/DX12/Vulkan validate 972 color and 972 full-shadow GPU samples with unchanged geometry, 170 draws, 340 slots and three material feature states. Source map-read counts distinguish mip bias from shading complexity. All 81 still, 540 camera/wind and 27 rendered-shadow pairs pass coverage/continuity checks; artifact identity verifies. No consistent timing benefit is established, so authored defaults remain. Twenty comparison/visual regressions pass.
- [x] Complete matched-layout validation of placement-object reservation. Add `--layout-seed`, retain the exact seed sidecar/hash and guard input/output aliasing; nine identity tests pass. Three baseline and three optimized Release/DX12 1080p captures match all 54 non-executable inputs, effective settings and profile call counts. Placement-record median falls **326.499 to 244.781 ms** at initial grass load and **580.805 to 491.792 ms** across Stop serialization calls (15.3% lower). Callback median **2646.220 / 2277.870 ms** also varies with parsing; do not attribute the full difference to this change or claim steady FPS gains. Evidence: `.local/performance-logs/capacity-fixed-comparison.json` (`comparison_valid=true`) and `.md/forest-performance.md`. Final optimized Editor rebuilt; grass asset/workspace layout preserved.
- [x] Reduce remaining cold Stop construction work: identify both grass snapshot callers, profile XML conversion and definition/batch construction, reserve XML object members, copy resolved prototype materials into independent batch components, and reuse completed DX12 allocator/list/fence/event bundles. Three baseline and three candidate captures match all 54 fingerprinted non-executable inputs/settings/call counts. Cold forest rebuild median **1082.730 to 870.159 ms** (19.6% lower), batch material work **201.612 to 16.729 ms**, command recording **24.316 to 4.230 ms**. Full first stopped-frame median is 5.3% lower with overlapping ranges; restoration callback is roughly unchanged. Preserve both grass snapshots, prefab edit/relocation detection and runtime ownership; retain no duplicate prototype geometry. Stop and delayed interval samples now carry `play_stop` and are excluded from recurring analysis. All 13 selected Release checks and three Debug checks pass, including the full Debug forest via a direct rerun beyond CTest's existing timeout; GPU validation passes. Signature/path/stat polling and the earlier intermittent grass diagnostic remain separate. Evidence and limitations: `.md/forest-performance.md` and `.local/performance-logs/cold-followup-comparison.json`.
- [x] Split opaque GPU time into tree bark/leaf and other opaque geometry, with controlled geometry/alpha-overdraw experiments and vertex/fragment workload counters. Completed on DX11, DX12 and Vulkan: asynchronous VS/PS/IA counters, same-shader zero-fragment vertex/front-end controls with counter-verified geometry equality, and measured incremental raster/fragment cost. Static and moving-route comparisons require complete build/backend/content/settings identity; moving runs also verify the deterministic camera trajectory. Validated 1,080 static frames per backend and two matching 1,080-frame moving-route captures. The historical ~71% opaque/~22% shadow split is not treated as a matched comparison to these diagnostics. See `.md/forest-performance.md` for measured costs and limits.
- [x] **2026-11-08: Build and accept a cheaper intermediate canopy LOD.** Published four coverage-pruned large-oak canopies: **42,498 to 36,297 triangles (14.59% fewer)**, preserving native attributes, silhouette, coverage and pivots. Validated ground/elevated/zoom/motion with **336 matched renderer pairs** across DX11/DX12/Vulkan, including 1080p and the actual installed asset paths; all seven Python checks and four selector CTests pass. Measured 96 projected-size/hysteresis cases and nine paired GPU processes (12,960 frames); full-forest visible primitives fall 3.65-4.18%. Retain **28/90 m**, existing dithering and fixed-band-1 shadows. DX12/Vulkan paired costs are generally lower; DX11 timing is mixed, so no universal GPU/FPS speedup is claimed. Original LOD1 assets remain available; regeneration selects approved canopies. Evidence: [intermediate-canopy-lod.md](intermediate-canopy-lod.md), `.local/canopy-lod-full/completion.json` (`complete=true`).
- [x] Evaluate dedicated low-cost shadow proxy assets instead of using visible LOD1 directly. Preserve crown silhouette, trunk contact, ground coverage and camera-independent cutouts; benchmark vertex/alpha-test cost and validate temporal stability before changing the forest default. Keep shadow distance/resolution decisions separate from visible tree quality.
  Progress 2026-11-09: TerrainObjectLayer now supports optional `objectLods.shadowMeshes` chains and shadow-only generated prefab batches, so shadow proxy assets can be authored without changing visible tree LODs or shadow distance/resolution policy. Added coverage in `OakBackendCapture` for dedicated shadow-only batch generation and kept existing fixed-shadow-Lod fallback checks; `TerrainPrefabAndOakTests.exe shared-prefabs` passes after the change. Forest defaults are unchanged;
  
- [x] **Map terrain-specific behavior to reusable responsibilities before refactoring.** `TerrainObjectLayer`/`TerrainManager` continue owning terrain placement data, surface sampling, terrain-cell streaming, and view-distance residency. Generic mesh LOD profiles/providers and instance batch preparation are reusable for non-terrain objects; render visibility remains in the scene renderer. Preserve current scene-file compatibility through the existing terrain provider adapter. **Engine connection:** this split is reflected in `TerrainObjectLayer`, `TerrainManager`, `TerrainPrefabInstances`, `ObjectLodProvider`, and the render-scene preparation path. **Reference connection:** [Unreal Level Instancing](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-instancing-in-unreal-engine) separates reusable object arrangements from the world/runtime placement workflow; terrain remains the owner of its own streaming policy.

- [x] **Add a generic LOD source-provider contract.** `ObjectLodProvider` exposes alternate mesh chains without a terrain dependency; `TerrainObjectLayer` resolves provider profiles, `MeshLodProfile` provides them for ordinary meshes, and `GrassField` implements it while the old terrain provider remains as an adapter. Provider mesh and shadow chains now participate in terrain source preparation and batch generation, and profile distance/screen settings flow into generated meshes. **Reference connection:** [Unreal's instanced static mesh model](https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-static-mesh-component-in-unreal-engine?application_version=5.8) applies LOD chains across general static-mesh instances; this contract lets non-terrain components supply those chains to placement systems.

- [x] **Extract generic mesh instance-layer data and batch construction.** `MeshInstanceLayerData` owns explicit placement transforms, stable IDs, a revision, and shared renderer instance data; it builds mesh bounds through `MeshInstanceBatchBuilder` and is now consumed by terrain prefab preparation and `GrassField`'s authored and procedural placement paths. **Reference connection:** [Unreal ISM/HISM documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-static-mesh-component-in-unreal-engine?application_version=5.8) uses reusable per-instance transforms and stable per-instance rendering data across static mesh instances.

- [x] **Add a generic explicit-transform instance-layer component for single static meshes.** `MeshInstanceLayer` serializes stable-ID transforms, groups placements spatially, builds conservative bounds and shared instance records, loads asset LOD chains incrementally, publishes runtime-only render groups, and removes them with the component lifecycle. It consumes `ObjectLodProvider` profiles and works on ordinary scene objects without terrain cells. **Reference connection:** [Unreal ISM/HISM documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-static-mesh-component-in-unreal-engine?application_version=5.8) applies instancing and per-instance LOD/culling to general static meshes; the group span preserves a tunable culling granularity.

- [x] **Complete the reusable instance-layer component/runtime lifecycle.** `MeshInstanceSourceSet` now owns reusable keyed geometry parts, per-part LOD representations, local source transforms, and material snapshots. Terrain prefab prototypes cache and consume these shared parts; `MeshInstanceLayer::SetSourceSet` accepts provider-prepared multi-part sets and publishes each part with its own material and LOD chain. Placement groups remain bounded to 1,024 transforms and publish one group per preparation pass, with staged replacement, failure cleanup, provider-aware reloads, and unload on destruction. `SplineInstancePlacementProvider` supplies non-terrain transforms through the same `MeshInstanceLayer` input. Terrain continues to own terrain sampling, cell ownership, and view-distance streaming. **Verification:** Release Editor build; streamed-prefab regression and dense-regression tests. **Reference connection:** [Unreal ISM/HISM documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-static-mesh-component-in-unreal-engine?application_version=5.8) applies instancing and per-instance LOD/culling to general static meshes, while hierarchical grouping is useful for large static instance sets.

- [x] **Create a mesh/object LOD profile independent of terrain foliage.** Partial implementation: `MeshLodProfile` and `ObjectLodProvider` provide ordered static-mesh asset chains, distance or screen-size thresholds, hysteresis, shadow chains/range, and generic instance-layer policy. The authored dither toggle and normalized transition width control generic instance LOD overlap. `KeepLast` remains the default final representation; opt-in `Cull` with a final distance rejects ordinary meshes and per-placement instances from color and shadow work while terrain cell residency stays owned by `TerrainManager`. Profiles can now author additional keyed mesh-part chains for `MeshInstanceLayer`; the existing source-set lifecycle publishes each part with its own geometry and material snapshot. Profile preparation scans each alternate's CPU bounds one mesh per preparation pass and unions them into the runtime aggregate before enabling LOD selection, so nonresident LODs remain covered by screen-size selection and range culling. **Verification:** Debug Editor build; streamed-prefab and dense streamed-prefab regressions pass. Remaining work is shared renderer-set selection for ordinary multi-object prefabs and an explicit low-detail mesh or billboard/impostor final choice. Keep foliage-specific coverage/wind behavior as an optional policy/capability. **Engine connection:** builds on `Mesh::GetRuntimeLodMesh`, `Mesh::GetRuntimeLodAggregateBounds`, `MeshInstanceSourceSet`, `ProjectedLodPixels`, scene draw selection, and instance compute culling; ordinary meshes and generic instance layers share the policy while terrain retains placement residency. **Reference connection:** [Unity LOD Group](https://docs.unity3d.com/6000.0/Documentation/Manual/class-LODGroup.html) models LOD levels as arbitrary renderer sets and exposes transition zones; its documentation also makes clear that cross-fading renders both levels, so transition cost should remain an explicit policy choice.

- [x] **Apply shared LOD profiles to ordinary static Mesh objects.** The attachable `MeshLodProfile` component loads alternate static mesh assets incrementally through the graphics-preparation queue, then configures the regular scene renderer's distance or screen-size LOD selection. It also implements `ObjectLodProvider`, so placement systems can read the same asset chain and policy. **Reference connection:** [Unity LOD Group](https://docs.unity3d.com/6000.0/Documentation/Manual/class-LODGroup.html) applies renderer-set LOD levels to ordinary scene objects; the engine connection is the generic runtime LOD path in `Mesh::StoreRuntimeLod` and `SceneRendererLodSelection`.

- [x] **Make culling, bounds, batching, and shadows consume generic capabilities rather than terrain/foliage identity.** Use shared instance records with per-source bounds and optional deformation padding; batch by compatible mesh/material/pipeline/render policy; apply camera and cascade visibility to all eligible static instances. Replace checks based on `TerrainObjectLayer`, `grassShadowCaster`, or special prefab LOD bands with explicit capabilities or policy data where practical. Preserve conservative fallbacks for skinned, morphing, transparent, warped, and editor-selected objects. **Reference connection:** [Unreal ISM/HISM guidance](https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-static-mesh-component-in-unreal-engine?application_version=5.8) describes per-instance LOD and hierarchical culling across static meshes; [AMD GPUOpen's performance guide](https://gpuopen.com/learn/unreal-engine-performance-guide/) emphasizes balancing batch size and culling granularity. **Verification:** Debug Engine build succeeds. Terrain-layer lookup remains only for editor selected-placement identification.



- [x] **Tie Vulkan render-finished semaphores to acquired swap-chain images.** `VulkanRenderCore` now creates one render-finished semaphore per swap-chain image, signals and presents with the semaphore indexed by the acquired image, and destroys/recreates those semaphores with swap-chain resources after device idle. Image-available semaphores and submission fences remain indexed by in-flight frame slot. **Reference connection:** [Vulkan's semaphore reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html) explains why presentation wait semaphores follow swap-chain image lifetime rather than submission-fence lifetime.

- [x] **Add phase-level frame timing before changing queue depth.** The opt-in `ENGINE_COMMAND_RECORD_BENCHMARK` CSV now records standalone simulation/update wait, scene preparation, command recording, shadow recording/replay, clear, scene replay, end-frame, queue-submission, presentation, and per-pass GPU timings alongside frame and update-to-present intervals. Rows include scene path, backend, actual GPU name, and current resolution. Editor Play's existing JSONL profile now records actual GPU, scene path, render dimensions, queue submission, and presentation timing with its existing detailed simulation, preparation, view, UI, and GPU-pass fields. Keep GPU validity explicit and compare CPU render time, presentation, and GPU pass samples separately before changing queue depth. **Reference connection:** [Unreal's rendering overview](https://dev.epicgames.com/documentation/unreal-engine/graphics-programming-overview-for-unreal-engine) separates game, render, and GPU timing; the engine's existing frame profiler and performance captures are the starting point.

- [x] **Introduce a small backend-independent frame plan, then migrate passes incrementally.** `RenderFramePlan` declares logical resource reads/writes, infers RAW/WAR/WAW dependencies, and stably orders passes without owning backend commands or allocations. Standalone Game uses separate particle simulation, shadow rendering, scene color/depth, and post-process/composition/present passes. The editor Game View and Scene View path runs inside the existing dirty-gated `RenderIfNeeded` callback, with clear, particle simulation, view rendering, UI construction, and a renderer-owned UI-overlay handoff represented in the plan. Current editor view rendering remains one aggregate pass around the existing view API; split its shadow/color/composition phases as those view and backend interfaces are migrated. **Reference connection:** [Frostbite FrameGraph](https://www.gdcvault.com/play/1024045/FrameGraph-Extensible-Rendering-Architecture-in) models passes and resources as a graph; [Unreal RDG](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine) describes pass scheduling and transient lifetimes.

- [x] **Standardize dynamic GPU data around frame contexts and upload rings.** `Scene::FrameUploadContext` now owns the native scene uploads and per-render-pass object/shadow upload regions for each prepared frame slot; its serial-bound counters reset only when that slot is selected for a new frame, after the backend's slot synchronization. Existing immutable native render packets and fence-tagged retirement are unchanged, and persistent mesh/material resources remain outside these dynamic contexts. The context accounts object and draw records, lights, bones, clusters, and portal records, plus context buffer allocations and capacity/pass-region growths. The Performance view and editor JSONL profile report those counters; native submission telemetry separately reports resource-creation calls and fence-wait time. DX11's existing constant arena remains backend-managed, while DX12/Vulkan retain their API-compatible slot resources behind the shared scene frame-context owner. These counters establish the baseline for replacing individual buffer bindings incrementally. **Reference connection:** [D3D12's application responsibilities](https://learn.microsoft.com/en-us/windows/win32/direct3d12/important-changes-from-directx-11-to-directx-12) require explicit dynamic-resource lifetime and synchronization.

- [x] **Audit transient render-target pooling against current resource lifetimes.** Current scene-color post-process targets are renderer-owned and reused across frames; shadow maps and temporal/history data persist by design; editor view outputs remain live while the UI samples them. The frame plan's migrated scene and editor passes currently expose no pair of compatible temporary render targets with non-overlapping lifetimes, so pooling them now would add allocator complexity and memory accounting without reducing live allocations. Keep these resources persistent and fence-safe; introduce a descriptor-keyed transient pool with peak-memory/lifetime diagnostics when a frame-plan pass actually creates short-lived intermediate targets whose lifetimes can overlap safely. **Reference connection:** [Unreal RDG transient resources](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine) describes transient allocation and aliasing for non-overlapping lifetimes.



- [x] **Add visibility and LOD diagnostics.** The performance view and trace report camera/shadow visibility totals, BVH visits, item and placement candidates, non-terrain instance tiles, draw/material groups, CPU-selected draw LODs, and CPU screen-size-selected instances/triangles by LOD. GPU cull results are copied into small readback buffers and consumed only after their frame-slot fence retires (DX11 uses a nonblocking event query); results report selected instances and conservative transition-band placement counts by LOD with the originating view identity. Camera and portal views have individual identities in the trace. GPU stage attribution is recorded by shadow, terrain, grass, portal, opaque, and transparent stages. **Reference connection:** [AMD GPUOpen's performance guide](https://gpuopen.com/learn/unreal-engine-performance-guide/) recommends diagnosing visibility and draw-call costs separately; the [Oak LOD study](../.local/performance-logs/oak-lod-study-20261111/summary.md) shows why selected-LOD counts and separate steady/sweep timings are needed.

- [x] **Return compact visible-item lists from the render BVH.** `QueryVisibleSlots` returns compact visible slot indices with cached BVH node/bounds/visible counters; camera preparation consumes the list, while stable slot-indexed mask queries remain available to other callers. Camera and shadow metrics are accumulated independently. `.local/tests/RenderSpatialIndexTests.cpp` covers visible/unbounded slots, query statistics, cache reuse, and refit invalidation. **Reference connection:** [AMD GPUOpen's performance guide](https://gpuopen.com/learn/unreal-engine-performance-guide/) describes CPU submission/visibility costs and the tradeoff between merging objects and culling granularity; the current BVH is the engine-side starting point for applying that guidance.

- [x] **Cull placement tiles hierarchically before placement-level GPU culling.** Add conservative bounds and a shallow grid/BVH over placement tiles, reject invisible tiles against camera and shadow frusta, then retain existing per-placement culling for intersecting tiles. Refit or rebuild hierarchy data only when placement membership/transforms change; include foliage wind envelopes in bounds. **Reference connection:** [Unreal's instanced static mesh documentation](https://dev.epicgames.com/documentation/unreal-engine/instanced-static-mesh-component-in-unreal-engine?application_version=5.8) describes a static hierarchy for accelerating instance culling and LOD; this proposal adds a tile-level broad phase ahead of this engine's existing per-instance compute culler.


- [x] Instrument `prepare_prefab_instances_ms` with registry collection, validation, LOD profiles, source-set construction/lookup, build work, and main-thread static-buffer creation. Validation contains its signature/profile/source subphases; build contains main-thread buffer creation. Asynchronous worker execution is not included in buffer creation time.
- [x] Capture frames with the new breakdown. In `.local/performance-logs/prefab-preparation-warm.jsonl`, the final 50 frames after a 240-frame warmup have a 0.96 ms median and 1.23 ms p95 for prefab preparation. Median validation is 0.95 ms, including 0.23 ms for LOD profiles, 0.42 ms for source lookup (including 0.27 ms for source-set construction), and 0.20 ms for source probes; registry collection is 0.007 ms. Build and main-thread buffer creation are zero throughout those final 50 frames. Earlier frames in the same capture still have build work, so this is a warmed result and does not establish the cause of the earlier log's 6.6–6.8 ms median. The automated process was stopped after 55 seconds, after all 120 measured frames had been recorded.
- [x] Cache distinct placement asset names by the existing placement signature and reuse them when constructing LOD definitions and source sets. Add profiler fields for placement-asset collection, LOD profile construction, and source-set construction. The next controlled trace must establish the performance effect.
- [x] Cache resolved LOD profiles and distinct source sets. Invalidate on placement-signature, owner, or component revision changes; refresh at least every 100 ms during preparation to catch direct field edits that bypass revision tracking. Keep source-file/relocation probes on their existing schedule.
- [x] Measure the cache using the same 240-frame warmup and 120-frame Debug meadow capture. Across the final 50 measured frames, prefab preparation median/p95 changed from 0.963/1.226 ms to 0.471/1.065 ms, and LOD profile construction median changed from 0.232 ms to 0.009 ms. Overall frame-time medians were 34.73 ms before and 36.08 ms after; do not infer an overall FPS gain from these runs. Logs: `.local/performance-logs/prefab-preparation-warm.jsonl` and `.local/performance-logs/prefab-cache-warm.jsonl`.
- [x] Check prefab profile edits, untracked direct edits, source relocation/deletion/reappearance, placement edits, and identical serialized restore with the current build. Updated `TerrainSharedPrefabs` for current mesh fields and asynchronous preparation; it passes in Debug. Fixed authored LOD-chain precedence over fallback placement profiles, source-cache refresh bookkeeping, and cleanup when a prefab source disappears.
- [x] Check a terrain cell unload/reload in the `TerrainSharedPrefabs` Debug regression. The test prepares an authored two-level prefab, unloads its cell, reloads it, and verifies the placement and both LOD bands are restored.
- [x] Run an editor play/stop smoke capture. `.local/performance-logs/prefab-play-stop.jsonl` completed successfully and recorded 30 stopped frames after restore.
- [x] Observe the restored editor scene through prefab preparation. In `.local/performance-logs/prefab-play-stop-long.jsonl`, a 900-frame post-stop capture completed cleanly. The first 50-frame stretch with zero prefab build time and a constant 614 candidate draws began at post-stop frame 339 (approximately 42.5 seconds of update intervals after the first stopped frame). The last 100 frames held at 614 candidate draws and 42,608 candidate instances. The capture demonstrates eventual settling; it does not establish an exact visual or object-state match with the pre-play scene, which was still building when playback started.
- [x] Retain existing shared prefab prototypes and prepared buffers across trusted Play restore until restored layers finish preparation; normal scene clears release retention. In the matching 900-frame Debug capture (`.local/performance-logs/prefab-play-stop-retained.jsonl`), 49 prototype references were retained and subsequently released. The first stable 50-frame stretch began at post-stop frame 277 (39.22 seconds), versus frame 339 (42.53 seconds) in the earlier capture; both ended with 614 candidate draws and 42,608 candidate instances. The first 200 post-stop frames had a 54.39 ms median frame time versus 36.46 ms previously, so investigate the frame-time tradeoff before treating this as an unqualified performance improvement.
- [x] Allow prefab preparation to publish additional parts and groups while the shared 3 ms budget remains, while making progress if one part exceeds it and retaining the existing upload allowance. A budget-only capture settled at frame 339 (42.73 seconds), so it did not materially reduce the restore tail in this scene.
- [x] Instrument global toolbar sections and LOD summary construction. Frame records include total/section timings, draw counts, LOD summary timing/calls, and remaining unattributed UI time.
- [x] Capture an initial trace with the Scene View toolbar active. In 24 editing frames, median/p95 were 1.86/2.56 ms for editor UI, 0.22/0.36 ms for the toolbar, and 0.073/0.099 ms for the LOD summary (`.local/performance-logs/toolbar-audit-scene.jsonl`). This short startup capture was stopped after 40 seconds and is not a controlled comparison with the original log; the old scan's cost was not measured before removal.
- [x] Remove the placement scan from the toolbar's LOD summary. Placement-only assets contribute one LOD, so the maximum can be derived from current profiles and overrides; stop scanning scene components after reaching the three-level display limit. The bundled grass prefab has 324,147 placements referencing only two distinct assets.
- [x] Assess whether the remaining profile and scene/component scans warrant a cache. The initial Scene-focused capture measured 0.073 ms median and 0.099 ms p95 for the entire LOD summary, so retain the current direct summary until a representative workload shows a need for caching.
- [x] Decouple shader optimization from application `_DEBUG`. DX12 explicitly requests `D3DCOMPILE_SKIP_OPTIMIZATION` in Debug; DX11 has the same policy.
- [x] Compile optimized shaders with debug symbols by default, retaining application Debug symbols and assertions. Provide an explicit option for unoptimized shader stepping.
- [x] Record effective D3D shader optimization/debug settings in performance provenance. DX11/DX12 compile from source and have no persistent shader bytecode cache requiring a cache-key update.
- [x] Profile unchanged-state work within command recording. In the stationary normal Oak standalone Game capture (Release DX12, RTX A2000 Laptop GPU, 1920x1080; 300 warmup and 180 measured frames), command recording median/p95 was 5.367/7.870 ms, including material 1.262/2.001 ms and batching 0.461/0.723 ms. Material pipeline resolution was 0.057/0.107 ms, geometry binding 0.028/0.052 ms, and other material work 1.173/1.852 ms; batch snapshot, build, and wait medians were 0.254, 0.151, and 0.187 ms. All measured frames reported a batch cache hit with 504 reused keys and a shader pipeline cache hit with zero misses. These stages are nested or may overlap; do not sum them. Evidence: `.local/performance-logs/oak-standalone/oak_meadow-game-dx12-20261112-111543-051/frames.jsonl`. The earlier 19.3/4.1/2.6 ms measurements were from a different workload and are not a matched comparison.
- [x] Reuse unchanged draw/material preparation and batch metadata. Exact unchanged batch inputs reuse grouped membership; Game batching skips editor-only selection lookup; view inverse, direction and view-projection are prepared once per view instead of per draw. A focused 504-input CPU test measured 0.015 ms reuse versus 0.045 ms forced regrouping. A normal Oak Release DX12 capture measured batch build median/p95 0.092/0.176 ms versus 0.151/0.243 ms in the earlier capture, but variable CPU/GPU timings and an intervening noisy run limit the comparison. Further lifetime and multi-view validation remains a separate item. Capture: `.local/performance-logs/oak-standalone/oak_meadow-game-dx12-20261112-113312-447/frames.jsonl`.


## Occlusion culling for all objects

### Route

The backends do not expose the main depth buffer to shaders, and compute
passes bind only buffers (three fixed bindings: input, output, placement
pool). A GPU depth pyramid would need that plumbing in DX11, DX12 and
Vulkan first.

So occlusion starts as a **CPU occlusion buffer**
(`Engine/Core/Rendering/OcclusionBuffer.h`), built for the current frame:

- No one-frame lag, and no trouble with objects coming into view in fast
  turns.
- It works on every backend.
- Occluders are the terrain meshes drawn this frame (their vertices stay on
  the CPU), rasterized at 256 × 144. Other meshes are added later (step 4).

### Done (stage A)

- [x] **Occlusion buffer:**
  - Terrain is rasterized on worker threads with near-plane clipping, row
    bands and centre sampling.
  - A 3×3 erosion keeps partly covered silhouette pixels from hiding
    anything.
  - A max-depth mip pyramid answers box and sphere tests in at most 9
    reads.
  - Bounds crossing the near plane or wholly off screen count as visible.
- [x] **Build:**
  - It runs as a job started right after the camera matrix, alongside the
    cascade and lighting preparation. Shadow views do not use it.
  - It is off for editor views, portal views and warp volumes, like the
    existing terrain tests.
  - `ENGINE_VISIBILITY_OCCLUSION_BUFFER=0` turns it off.
- [x] **Ordinary objects and whole instanced tiles:** their world bounds are
      tested in `PrepareVisibleItems`, next to `HiddenByTerrain` /
      `HiddenBySolid`. Skinned, morphing, offset, blended, preview, portal
      and warped items are skipped.
- [x] **Detail budget:** the cost model drops placements hidden behind
      terrain before its disc coverage grid, so they cost nothing.
- [x] **Benchmark fields:** `occlusion_occluders`, `occlusion_triangles`,
      `occlusion_rasterized`, `occlusion_build_ms`, `occlusion_tested`,
      `occlusion_hidden`.
- [x] **Unit tests:** `.local/tests/OcclusionBufferTests.cpp` (CMake target
      `OcclusionBufferTests`, guarded like the other local tests). It covers
      hidden and visible cases, silhouettes, the near plane, off-screen
      bounds and the empty buffer, plus tiny-occluder skipping, seamed
      strips and the reuse cache. The first version was never committed;
      it was rewritten and the `.gitignore` exception added.

### Measured (landscape demo start view, 1080p, RTX 4090)

| Backend | Build | Triangles in / rasterized | Items tested / hidden |
|---------|-------|---------------------------|-----------------------|
| DX12    | 0.7–1.3 ms | 97k / 24k | about 976 / 23 |
| DX11    | 1.3 ms     |           | 942 / 23 |
| Vulkan  | 1.2 ms     |           | 976 / 23 |

- With the detail scale pinned at 1, frames with occlusion on and off are
  identical apart from wind in the near grass, so nothing visible is
  dropped.
- The render thread pays about 0.9 ms, waiting on the build.
- In this meadow view, terrain hides only about 2% of tested items, and
  GPU geometry time does not change measurably. The landscape's hills are
  low (at most about 120 m), and most hidden placements sit inside tiles
  that are partly visible, so stage B is where the gain should come from.
- With the budget adapting, hidden placements count as free, so the
  budget settles higher (scale 1.41 against 1.25). The grass layer's
  spawn radius grows with it, and that CPU work is outside the budget.
  The demo is currently CPU-bound at about 21 ms a frame, with GPU geometry
  at about 5.5 ms.

### Done (stage B)

- [x] **Fourth compute read binding:**
  - DX11: `t2`.
  - DX12: a fifth root parameter, SRV `t2`.
  - Vulkan: binding 3, with the compute descriptor pool grown to four
    buffers per set. The extra binding is cleared whenever a compute
    pipeline is set.
- [x] **GPU upload:** the occlusion buffer goes up once per view as a float
      array (`OcclusionBuffer::SerializeForGpu`), using a ring of upload
      buffers per prepared frame slot. An empty buffer (level count 0) is
      bound for shadow passes and when occlusion is off.
- [x] **Instance cull:** `InstanceCull.hlsl` (`IsBoxOccluded`, matching
      `IsBoxHidden`) tests every placement in the per-placement path (trees,
      rocks, grass tiles) and every instance in the automatic path, in
      camera passes only. `ENGINE_VISIBILITY_OCCLUSION_GPU=0` turns this off
      and keeps the CPU tests.
- [x] **Debug overlay:** `ENGINE_OCCLUSION_DEBUG=1` with the Detail render
      mode keeps everything occlusion would hide and paints it magenta, so
      any magenta on screen is something visible that would have been
      culled. `=2` marks everything tested, as a check that the overlay
      works.

#### Results (landscape demo start view, detail scale pinned at 1)

- **Placements hidden:** LOD0 foliage placements selected fall from about
  15,500 to about 900 (94% hidden behind terrain). DX12, DX11 and Vulkan
  agree.
- **No visible errors:** the debug overlay shows no magenta pixels in the
  start and west views on all three backends. The control mode paints the
  whole ground and every tree magenta.
- **GPU time:** tree leaves fall from 0.71–0.86 ms to 0.52–0.54 ms, and
  foliage depth from 1.24–1.27 ms to 1.04–1.19 ms.
- **Not comparable yet:** total frame times. The explorer's start position
  currently varies from run to run, so paired runs do not share a camera.
### Done (build cost)

Measured on an RTX A2000 laptop, landscape start view, DX12, 1080p. Run-to-run
variance on this machine is large, so the figures are approximate.

| | Before | After |
|---|---|---|
| Build, camera moving (`ENGINE_VISIBILITY_OCCLUSION_REUSE=0`) | 3.08 ms | 2.02 ms |
| Render-thread wait, camera moving | 2.55 ms | 1.68 ms |
| Build, camera still | 3.08 ms | 0.06 ms, no wait |
| Items tested / hidden | 942 / 23 | 942 / 23 |

- [x] **Start the job earlier:** it is submitted right after the camera
      matrix, overlapping light collection, clustered lighting, shadow
      cascades and environment preparation. Field `occlusion_wait_ms`.
- [x] **Reuse while still:** `OcclusionBufferCache` returns the previous
      buffer when the view-projection, occluder pointers, transforms and
      revisions are bit-identical (field `occlusion_reused`). The key uses
      `Mesh::GetCpuGeometryRevision()`, drawn from one process-wide counter
      and bumped by `SetTerrainGeometry`, `TakeTerrainGeometry` and
      `SetDeformedVertices`.
- [x] **Skip tiny occluders:** projected bounds below a per-occluder minimum
      (2 buffer pixels for terrain), off screen or wholly behind the camera
      are skipped before vertex work (field `occlusion_skipped`). No terrain
      tile is skipped in the start view.
- [x] **Faster build:** vertices transformed and projected once each, in
      chunks of 4096; triangles set up in chunks of 2048, each binned by band
      on its worker (replacing the serial binning pass); raster uses edge
      stepping and keeps the largest 1/w per pixel. Raster fell from 1.02 to
      0.34 ms; setup is the remaining bulk (~1.3–1.6 ms).
- Not met: under 0.5 ms with a moving camera. A serial build takes 6.6 ms
  for setup alone, so the parallel batches matter, but they queue behind the
  frame's other jobs in the shared `JobScheduler` FIFO.

### Done (mesh occluders, off by default)

- [x] **Large opaque meshes as occluders:** instanced placements with an
      opaque material (`MaterialAlphaMode::Opaque`), no wind
      (`foliageWindStrength == 0`), not skinned or morphing. Each uses its
      lowest LOD band's source geometry, shrunk 10% toward its bounds
      centre so a simplified surface never sticks out past the drawn one.
      A bounding-sphere estimate rejects placements behind the camera or
      under 6 buffer pixels before any record is made. Field
      `occlusion_mesh_occluders`.
- [x] **Foliage stays out:** masked and blended materials and anything with
      wind never occlude.
- **Off by default** (`ENGINE_VISIBILITY_OCCLUSION_MESHES=1` enables it).
  In the landscape demo the start view keeps 0 of ~2,000 rock and log
  placements (all too small), and the job costs 0.51 ms instead of 0.14 ms.
  A drive-forward run drew up to 7 rocks or logs as occluders, but showed
  no clear gain in hidden items (median 21 against 25 with them off; the
  runs follow different camera paths) and a higher build median (2.9 against
  2.2 ms). The magenta overlay stays clean with them on. Worth enabling for
  scenes with buildings or other large solid meshes.

- [x] Find out why few trees near the landscape start view get LOD0.
      Not a bug; three factors stack. Authored LOD0 is 60 m; screen coverage
      (camera FOV wider than the 60° reference) × 0.872 gives 52.3 m; the
      detail budget at its 0.6 floor (applied 0.636) gives ≈ 33 m, before
      placement scale (÷ 0.8–1.25). The nearest meadow trees are about
      40–60 m away, so they draw LOD1. Pinned at scale 1
      (`ENGINE_DETAIL_SCALE=1`) the nearest two reach LOD0, as authored.
      The budget sits at its floor because the absolute 6 ms GPU geometry
      budget (`DetailBudgetSettings::geometryBudgetMs`, tuned on an RTX 4090)
      cannot be met on the RTX A2000 laptop: after 3 ms of terrain only 3 ms
      remain for ~8 ms of LOD-dependent work, so the cost budget collapses
      to 1. Fix: auto quality (below).

## Moved from todo.md on 2026-11-20

Completed items, in the order and under the headings they had in todo.md.

### Landscape demo expectations (2026-11-19, from the Chlorophyll Engine video)

- [x] **The landscape had no sunlight in captures.** The sun's cloud-shadow cookie path ("Assets/Textures/...") went to
      the texture loader unresolved, so it loaded only when the working directory was the asset root. From the
      capture tool (run from the repository root) the decode failed, the null (black) texture was bound, and the
      cookie multiplied the sun by zero: every landscape capture since cloud shadows were added (Foliage Phase 3)
      was lit by ambient light only, with auto exposure brightening the result. That is the "dark meadow, bright
      far forest" this section first recorded as the demo's state (meadow 0.011-0.035), and the Foliage Phase 1d
      colour pass on the landscape was judged under it. Whether the editor or the game showed the same depends on
      their working directory; not checked. Fix (`SceneRendererLighting::ResolveViewLighting`): cookie and IES
      paths go through `Mesh::ResolveFilePath`, and a missing or still-loading texture is replaced by a white
      texel, so it can never black out its light. Measured at fixed exposure: a uniform 0.3 cookie now gives
      exactly the image of a 30 % sun.
- [x] **Ground tint was off.** The forest-floor grass field (no tint) cleared the meadow field's ground cover on
      every refresh, so the bare terrain texture showed pale between the blades and "raising the tint did
      nothing" (Foliage Phase 1d open item, now closed). `Terrain::GroundCover` records its owner and a time
      stamp; only the owner changes it and an unrenewed claim expires after 2 s. Meadow preset: tint strength
      0.5 → 0.8, root shade 0.7, cover colour factor 0.8 → 0.45 (the open ground past the blades was 1.6x the
      meadow's luminance; now 1.13x).
- [x] Scene settings (scene and `tools/build_landscape_demo.py`): tone mapping AgX → ACES (AgX could not reach the
      reference sky: saturation 0.44 against 0.56), output exposure -0.4 → -0.25, grading saturation 1.1, displayed
      sky intensity 1 → 2.4 with the sky fill light (`hdriIntensity`) 0.55 → 1, haze `mieScale` 4 → 3, exposure
      floor `minEv` -4.8 → -4. Sun 40 → 57 degrees up, colour 1 / 0.91 / 0.77 → 1 / 0.96 / 0.9.
- [x] Cloud shadows 0.3 → 0.42 under cloud (`generate_sky_sprites.py`, texture regenerated): at 0.3 the shaded
      meadow was 4.5x darker than the sunlit one; the frames show 2.4-4x.
- [x] Far trees: oak impostor materials had twice the near leaves' flat ambient and the same tint, and read 1.5x the
      meadow front-lit. Ambient halved, tint x0.8, and `foliageTranslucency` 0.7 on all 28 impostor and cluster
      materials so they do not go black against the light.
- [x] Near crowns: shaded leaves under a sunlit canopy get scattered light (`FoliageTransmission`, Object.hlsl:
      0.3 x (1 - shadow), more toward the shell). Crown undersides 0.015 → 0.024. It follows the leaf's shadow,
      so a tree in terrain shade keeps a little of it.
- [x] Viewpoint **MeadowSlope** added (`tools/add_landscape_viewpoints.py`: a meadow slope with forest 50-150 m
      ahead, lower ground beyond, no lake in view, sun behind or beside the camera); eye (-240, 12, 1416).
      The tool's sun direction now follows the scene's sun pitch.
- [x] **Grass reach** (2026-11-20, user: "grass vision doesn't extend very far"). Cause: grass radius is multiplied
      by the detail budget's scale, which sits at its 0.6 floor on the landscape (0.53 for the Grass layer), so the
      detailed grass ended at ~20 m instead of 36 and the far ring at ~50 m instead of 90. Now the far ring keeps
      its authored reach (`GrassField::OuterRadiusScale`; its inner edge follows the detailed field's scaled
      radius, which also closes the gap between the two), reaches 200 m on 64 m tiles at 0.9 clumps per m²
      (`MeadowGrass::ApplyFar`), and its clump is wider (`far_grass_tuft`: 14 blades 7.5 cm wide, ~0.9 m across,
      42 triangles). MeadowSlope: the ground from ~60 m to the forest edge measured 0.074-0.091 against a 0.078
      meadow, and reads as grass (`.local/foliage/j3_MeadowSlope.png`, `j3_crop.png`). Cost at MeadowSlope:
      grass GPU 2.0 → 3.8 ms, 16.7k → 127k grass instances, tile count unchanged (268), camera record
      11.1 → 12.3 ms (single runs).
- [x] **Grass shells in the terrain** (user: "should the grass materials for terrain have some sort of 3D effect"):
      `GrassGroundShells` (Object.hlsl) shades the ground under and past the blades as tufts ~0.3 m deep, four
      parallax shells through a procedural tuft height field: tufts hide the ground behind them at low angles,
      show dark gaps from above, and near the horizon the result goes to the tip colour. Replaces the grain and
      sheen of Grass Phase 6a. Terrain GPU unchanged (0.86 ms). Runs on DX11, DX12, Vulkan (test meadow).
      Limit: at eye level over flat ground a pixel row covers metres of depth, so the shells only show their
      depth from a raised view or on a slope facing the camera; standing blades carry the look otherwise.
- [x] **Rolling hills in the lowland** (`build_landscape_demo.py` `lowland_hills`, added in `carve` after erosion):
      swells up to ~150 m on a 1.5 km wave with flat valley floors between, none within 260 m of the start (the
      start meadow and its view of the first lake stay) or on the lakes, fading out 1.9-2.9 km from the start.
      Within 2 km of the start the ground was -38..21 m (5th-95th percentile) and is now -38..116 m; 40 % of it
      slopes 10-25 degrees, 2.8 % is steeper than 30 (no trees there).
- [x] **Escarpment** (`ESCARPMENT`, `escarpment`): a cuesta 700 m south of the start, facing it (centre
      (-150, -660), face toward +z, so it is sunlit and visible from the start meadow). Rock face 66-95 m high,
      steeper than 50 degrees along 730 m, buttresses to 70-80 degrees with gentler gullies, a 12 m talus apron,
      the plateau dipping away over 650 m behind. 15,600 m2 of it passes `GenerateCliffs`' steep-rock test along
      the whole face, so the scanned rock pieces should stand on it; 93 % of the plateau and 96 % of the foot are
      plantable (under 30 degrees).
- [x] The rest of the map is bit-identical: the builder reproduced the stored archive before the change, and hills
      and escarpment are exactly zero outside their footprints. The builder now keeps the sculpt edits of cells
      that come out identical (`unchanged_cells`; all 20 `.terrainedit` files survived) instead of deleting all of
      them. The archive signature changed, so terrain caches rebuild on first load. `world.flow` and
      `world.deposition` are from the erosion and do not know the new hills.
- [x] **Forest prefers higher ground** (`ForestMask(x, z, height, a)`, LandscapeDriver.cpp; replicated in
      `add_landscape_viewpoints.py`): the mask noise is raised by 0.06 from 30 to 80 m above the water level, by
      my estimate from the noise's spread about 44 % forest on valley floors and 60 % above. This applies to the
      whole map, so the foothills carry roughly a third more trees, near and far. Cost not measured.
- [x] **Closed forest edge** (`GenerateForest`, second pass with its own random stream; the first pass's trees
      keep their places where the mask did not change): medium and small oaks and saplings (young spruce and fir
      in the conifer band) across the mask's transition, up to one per ~30 m2, smaller toward the meadow, leaning
      out up to 9 degrees. Loaded cells only, not in far blocks. No new assets.
- [x] **Shrub belt** (`GenerateProps`, undergrowth, own random stream): the stand's tall bush (30 % the low one)
      at 1-1.9x scale along the meadow side of every edge, up to one per ~12 m2, ragged by the 16 m clump noise.
      **Removed after the capture (below)**: it did not show from the meadow.
- [x] **No soil line under the edge** (`PaintSample`): forest soil is painted only from mask 0.3 up (full at
      0.85), so the outer edge trees and the belt stand in meadow grass. `PaintRevision` bumped.
- [x] Viewpoint **Escarpment** (`find_escarpment`): eye (-350, -10.7, -370) on open ground 350 m in front of the
      face, looking at its middle. **MeadowSlope** moved to (-368, 120.3, -1000), a 20 degree slope on the hill
      behind the escarpment with a 149 m drop to the valley (it was (-240, 12, 1416)); the numbers in the
      expectations table are from the old pose.
- [x] **Captured and judged** (2026-11-20 evening, RTX 4090, DX12, 1920x1080; `.local/foliage/k1_*.png` as
      pushed, `k5_*.png` after the fixes below; poses other than the saved viewpoints via `--camera-position`).
      The start pose was not captured.
      - Hills: read well from MeadowSlope (`k1_MeadowSlope.png`): forested swells, meadow clearings, the lake
        and the alpine range behind. Kept as built.
      - Forest edge as pushed: **failed**. From 75 m (`k1_edge80.png`) a row of even 10 m oaks with a trunk
        line and a strip of soil under it, and no shrub belt at all. The belt bushes exist but are 1.5-2 m of
        the pack's broad lime leaves, and every bush stops being drawn ~25 m from the camera (also at
        `ENGINE_DETAIL_SCALE=1`); why single-LOD props end there is not found (see To investigate).
      - Escarpment as pushed: **failed**. The sheer parts of the face were bare terrain with a striped rock
        texture (`k1_escair.png`): `GenerateCliffs` refused ground steeper than 72 degrees, which is most of
        what is seen of the face. Rows of dark columns stood through the forest at its foot and on the rim:
        the generated `Crag_spire_01` filler (25 m tall). The saved viewpoint looked at trees: the foot of
        the face is forest.
- [x] **Forest edge closed with sunk oaks** (`GenerateForest`, second pass rewritten; 10 attempts per tree
      attempt, own random stream). Across the mask's transition, meadow side first: small oaks sunk 1.6 m x
      scale, to the base of their crowns, as 2-4.5 m leafy mounds on the grass (up to one per ~8 m2, ragged by
      a 14 m noise); medium oaks sunk 3 m x scale as 5-8 m mounds behind them; then standing small and medium
      trees as before. Young spruces do all three in the conifer band. The pack-bush belt is gone. Result
      (`k5_ForestWall.png`, `k5_ForestEdge.png`, `k2_edge35.png`): a closed wall from the grass up at 22, 35
      and 75 m; soil-coloured pixels along its base 1.5 % → 0.2 %. Not done: height and species variety along
      the wall; sky share through the silhouette (my first measure counted the sky between crown tips and is
      not usable); the sunk trees' impostors past 130 m; colliders (the sunk trees still collide as trees);
      the conifer band's edge (not captured).
- [x] **Escarpment face covered** (`GenerateCliffs`): sheer ground counts (normal.y down to 0.08, was 0.3), up to
      64 pieces a cell from 220 attempts (40 from 140), scanned faces only (the generated spire, overhang and
      ledge are no longer placed). `k5_Escarpment.png`: a continuous band of scanned rock with forest above and
      below, short stretches of bare striped terrain left between pieces. Applies to every cliff on the map,
      so the alpine slopes carry more pieces too; cost there not measured. Scree rocks (`GenerateProps`) now
      lie tipped 45-85 degrees, at most ~2 m, 0.4 odds (0.55), and generated boulders are not used in scree;
      **not looked at up close**.
- [x] Viewpoints (`add_landscape_viewpoints.py`): **ForestWall** added (open meadow, forest edge 75 m ahead,
      sun behind; eye (320, -27.9, -216)), the pose for the forest edge row. **Escarpment** is now chosen by
      line of sight to 32 points on the face over ground and canopy: from the ground no spot sees enough of
      them, so the eye hovers 50 m up at (-110, 24.4, -410) and sees 19. **MeadowSlope**
      requires no forest within 30 m toward the sun; now (-368, 111.0, -1032).
- [x] Moving-camera runs after all of this (single runs, ForestFloor start, 400 frames): walking 29.5 ms
      median, p95 53, 22 frames over 50 ms; again 29.0, p95 43, 11 over 50 ms; walking and turning 37.0,
      p95 64, 50 over 50 ms. Against 28.5 (11 over 50 ms) and 35.2 (34) before the pass: 0.5-2 ms slower at
      most, which single runs cannot separate from noise. Standing: Escarpment 20.5, ForestWall 20.3,
      MeadowSlope 23.0, ForestEdge 23.0 ms (23.1 before the edge rewrite). The hitches at cell crossings are
      unchanged and are still item 2 of "Next".
- [x] **Full-resolution look pass** (2026-11-20 night; `.local/foliage/hires.sh <tag> <viewpoint>` captures at
      2560x1440 and `tiles.py` cuts 2x2 unscaled 1280x720 tiles, `crop.py ... <zoom>` for details; `h1_*` before,
      `h4_Escarpment_*` after the soil fix). Until now every look judgement was made on half-size 960x540
      copies. A 3840x2160 request is clamped by the window to 2564x2160 and fails; 1440p is the largest that
      works on this display. 1440p costs 0-1.5 ms over 1080p at the standing poses (21.3-23.6 ms): the frame is
      CPU-bound. Look captures only; timings stay at 1080p. What the tiles showed:
      - Forest floor in sun was rust red (sRGB 108,69,41; `h1_Escarpment_r1c0.png`, gaps in the canopy and a
        red line under the rim forest): the Ground048 photo itself, textured layers ignore `baseColor`.
        **Fixed**: `baseColorTextureTint` (0.85, 1.15, 1.3) on `PBR/forest_soil.material` (and in
        `generate_terrain_pbr_content.py`); now 98,76,53. The material is shared with the other PBR terrain
        scenes, which were not re-captured.
      - Forest edge: holds at 20 m (`h1_wallnear_*.png`) and at the ForestEdge pose. From 75 m the base of
        the wall shows a row of dark blue-grey posts and slabs (`h1_wall_pickets.png`, `h1_wall_slabs.png`):
        trunks, stumps and logs inside the forest, lit only by sky ambient that the canopy does not occlude,
        with no understory in front (bushes end at ~25 m). Not fixed.
      - Escarpment: two scan sets of very different colour sit side by side (dark mossy brown crag, pale pink
        boulders); the rim forest past the mesh LODs is a flat mid-green hedge on bare soil. Not fixed.
      - Meadow: mid-distance grass is one broad-bladed clump repeated in visible rows with soil between; the
        pack's low-poly bushes in the foreground clash with the scanned trees; flowers are loose white cards.
        Not fixed (Grass phases).
      - Far view from MeadowSlope: forest on the mountains is near-black navy blobs (sRGB 45,65,73) against
        pale untextured rock (191,179,160), where drawn forest at mid distance is 72,87,68. Raising the far
        canopy colour 1.5x moved it 2 levels, so the blobs are the far tree stand-ins, not the terrain tint;
        reverted. Pale blurred ellipses (171,161,137) lie in a row along a meadow crest: probably rock paint
        from the archive's ridge code seen through the macro colour; taking trails out of the macro colour
        did not change them (reverted); the LowClouds prefabs are elsewhere. A snow summit behind ForestWall
        shows as a flat white facet (`h1_wall_white.png`). None fixed.

### Static objects (2026-11-20)

- [x] **Bake at play start**: static prefab instances that are plain geometry (Mesh, Material,
      PrimitiveObjectCollider, Static RigidBody; no blended material; no prefab overrides; rotation + uniform
      scale; two or more of the prefab; not named by another component's field) are removed from the scene and
      become placements on one runtime-only `TerrainObjectLayer` ("Static objects (baked)"), the path forests use.
      Editor edit mode is untouched; play mode restores the scene on stop. `ENGINE_STATIC_BAKE=0` keeps the objects.
- [x] **Warnings** (stderr and debugger in the game, Console in the editor, and under the checkbox): non-static
      parent, Dynamic/Kinematic rigid body, a component that moves its owner (controller, spline follower,
      animation, IK, cloth; by type name), non-uniform, mirrored or sheared scale, prefab overrides, referenced by
      name; and while playing, a static object whose transform changes (`MotionWatch`, 32 objects checked a frame).
      Static lights, cameras, terrain and emitters stay scene objects without a warning.
- [x] Terrain scenes: every top-level object except the explorer is static in landscape_demo, grass_test,
      foliage_test and woodland_assets_preview (`tools/scene_static.py`; the three scene builders call it).
- [x] Checked on woodland_assets_preview (DX11, 960x540, laptop): 16 conifers of 4 prefabs baked, no warnings;
      a copy with a 1 2 1 scale and a Dynamic body on the ground printed both warnings and baked the other 15.
- [x] **Baked trees follow the prefab's LOD distances** (checked 2026-11-20, DX12, young spruce alone, authored
      23.75 / 85.5 m, 640x360 where the view scale is ~0.77): submitted triangles 50.8k at 18 m (LOD0), 13.2k at
      30 and 38 m (LOD1), 316 at 66 and 96 m (impostor). The layer receives the distances from the instance's
      mesh (printed: fir 37.5/135, pine 50/180, tall spruce 60/216, young spruce 23.75/85.5) and, like the
      forests, keeps LOD0, LOD1 and the last level (the impostor) of a longer chain.
- [x] **foliage_test with the grove as static objects** (2026-11-20, laptop, DX12 1280x720, `ENGINE_DETAIL_SCALE=1`,
      `ENGINE_AUTO_EXPOSURE=0`; `build_foliage_test_scene.py --standalone` copied to
      `.local/static-test/foliage_standalone.scene`, images in `.local/static-test/foliage/`): 77 of the 84 grove
      objects of 19 prefabs baked, no warnings; the other 7 are single instances of their prefab. Baked and
      unbaked (`ENGINE_STATIC_BAKE=0`) look alike at ForestFloor, Backlit and Meadow (3-5 % of pixels differ by
      more than 0.16, the LOD differences above). Frame interval, 5 frames each, rough: scene's own grove layer
      18-22 ms, baked 28-57 ms, unbaked 85-115 ms. The baked layer has no fixed shadow level and a 1000 m
      shadow distance, which is most of its gap to the scene's layer (GPU shadow 2.2-3.1 ms against 0.8-1.0 ms).
- [x] **On the landscape the bake does nothing** (2026-11-20 night, RTX 4090): its 12 static objects are the
      viewpoints, sun, atmosphere and the Landscape root; none is a bakeable prefab instance, and the forest
      is placements already. What costs there is the ~9.7k runtime mesh objects under the static Landscape
      root (terrain cells, far tiles, far-object blocks, layer groups, grass tiles).
- [x] **Static objects skip the transform test for their cached render item**
      (`SceneRendererFramePreparation::PrepareFrameItems`). When streaming adds or removes any object, the
      renderer has no prepared transform hierarchy for that frame and tested every item serially
      (`prepare_items` 5.5 ms against 2.6). That was 84 % of walking frames. An object that is static in its
      hierarchy is now tested on the workers in those frames too, without reading its transform. If it moves
      anyway its item is stale until the next frame without a structure change (and `MotionWatch` reports
      it). `ENGINE_STATIC_ITEM_TRUST=0` turns it off. Walking, two interleaved pairs: `prepare_items`
      5.4 → 3.65 ms, median 30.4 → 28.9 ms.
- [x] **Grass tile objects are pooled** (`GrassField::m_freeTiles`, `Mesh::ResetRuntimeLods`): the cause of
      those structure changes was grass, which added and removed ~6 tile objects on 3 of 4 walking frames.
      A retired tile is emptied and reused for the next tile in range. `ENGINE_GRASS_TILE_POOL=0` removes
      objects as before. Structure-change frames 336 → ~155 of 400 (the rest is cell, far-block and prefab
      layer streaming in bursts around cell crossings).
- [x] **Both together, interleaved A/B, 400 frames, 1080p DX12** (off = both switches 0; median / frames over
      50 ms): walking 31.3/23, 31.7/19, 29.9/15 → 28.6/14, 25.9/9, 26.5/11 (mean 31.0 → 27.0 ms);
      walking and turning 40.5/63, 38.1/39 → 35.8/39, 36.0/38, 34.4/34, 35.4/32. Stages while walking:
      scene prepare 9.0 → 5.9 ms, simulation update 5.0 → 3.4, post-physics commit 1.4 → 0.02. Standing
      ForestEdge 23.1 → 24.2 ms, one run each (no change expected; within run noise, not confirmed).
      End frames differ between any two runs by ~16 % of pixels with or without the changes (the walk's end
      point depends on frame times), so the look check is by eye: `s2_walk1.png`, ground cover present.
- [x] **Cell-crossing hitches: cause found and fixed** (2026-11-20 night). Timing the commit after physics
      showed removals of just 3-4 objects taking 45-95 ms: each unloaded cell's "Landscape props"
      `TerrainObjectLayer` took 6-12 ms to destroy. Cells that stream in together ask for the same prefabs
      before any is cached; each layer built its own prototype (a small scene with a physics world), used
      whichever finished first and kept the others unconsumed until it was destroyed. Now one build per
      prefab is shared by every layer waiting for it (`TerrainPrefabInstances.cpp`, `buildsInFlight`;
      `ENGINE_PREFAB_PROTOTYPE_SHARE=0` for the old behaviour): props layer destruction 10 → 1.3 ms.
      Also `Scene::FlushPendingObjectRemovals` looked each pending object up by walking the whole hierarchy
      (33 ms for one commit of 584 removals among 10.6k objects); it now asks the id table.
      Interleaved A/B on the sharing switch, 400 frames (p99 / max / frames over 80 ms / longest commit):
      walking 120/139/6/77, 120/144/6/81, 123/144/6/78 → 74/83/1/34, 74/79/0/36, 79/82/2/33;
      walking and turning 123/144/10/79, 124/139/10/76 → 79/84/3/36 (one run). Medians unchanged
      (26-28 ms walking). Left: a ~33 ms commit once per crossing (an unloaded cell's grass field removes
      its 300-600 tile objects one by one), and ~10 frames of `scene_prepare` +10 ms after a crossing.
- [x] **Grass pool keeps a recycled tile's meshes until the frame fence.** `ResetRuntimeLods` first destroyed
      a retired tile's meshes and placement buffers at once, where a removed tile object is kept until the
      frame fence. They now leave on a carrier object released behind the fence
      (`Mesh::ResetRuntimeLods(receiver)`, `Scene::RetireRenderObject`).
- [x] Two wrong turns on the way, both reverted: keeping prototypes alive for the scene's lifetime (no
      effect: the unused copies were the cost, not the shared one), and retiring render leaves inside a
      removed subtree behind the frame fence (47 of 53 objects retired, same time: it was the layer).

### To investigate

- [x] **Diagonal stripes on plant materials** (user screenshot): reproduced on the landscape ForestFloor with
      `ENGINE_TAA=0` (`.local/foliage/stripe_notaa.png`). Cause: screen-space ambient occlusion. The AO pass
      rotates its two slices per pixel with interleaved gradient noise, and `ScreenAmbientOcclusion`
      (Object.hlsl) read it back with a bilinear 2x2 lookup; on wind-moved leaves the AO history is
      rejected, so the noise's diagonal structure showed as dark bands in occluded areas (TAA only smeared
      them into blotches). Fix: a depth-matched 3x3 tent lookup (9 taps instead of 4), the footprint that
      noise is uniform over. `.local/foliage/ao_taa0_crop.png`: gone with TAA off too. Not the shadow map
      (ground plants cast no shadows) and not the canopy taps. Cost of the 5 extra taps not isolated.
- [x] Mossy rock scans read as large orange sandstone: tinted [0.42, 0.5, 0.4] and shrunk
      (`.local/foliage/ao_taa1.png`). The scan still shows little actual moss.
- [x] Reference video "[Chlorophyll Engine] 10M Trees - 15km² Test" (user: "can we achieve this?"): four frames
      saved, press coverage read, the frames and the demo measured. Result: the "Landscape demo expectations"
      section above. The look needs no technique this renderer lacks; the tree count and frame rate claimed for
      that engine are developer-reported and were not compared.

### Foliage and world realism / Foliage Phase 0: Baseline and look targets

- [x] **Test scene** `tools/build_foliage_test_scene.py` → `Scenes/terrain/foliage_test.scene`: the grass
      test cell plus a grove (oaks, a conifer edge, saplings, pack bushes; 84 placements in an instanced
      `TerrainObjectLayer`) and saved viewpoint cameras **ForestFloor**, **Backlit** (looking up through
      the crowns into the sun) and **Meadow**. Loads in seconds. Capture with
      `.local/foliage/capture.sh <tag>` (DX12, 1920x1080, 300 warm-up + 120 frames; `topng.py` makes
      half-size PNGs).
- [x] Baseline (`88efe11`, RTX 4090, DX12, median frame / GPU leaves / bark / grass, ms):
      ForestFloor 6.7 / 0.76 / 0.92 / 2.5, Backlit 7.5 / 0.53 / 0.69 / 1.7, Meadow 4.3 / 0.33 / 0.32 / 1.9.
      Captures `.local/foliage/base_*.png`: crowns evenly lit flat green with no glow or dark interior
      even straight into the sun; the floor under the grove is as bright as the meadow; the generated
      meadow bushes read as large flat polygons up close.
- [x] **Found: standalone prefab trees hit a very slow CPU path.** The same 84 trees as scene prefab
      objects (`--standalone`) take 55-70 ms a frame, almost all in `shadow_submission_ms` (64 ms CPU
      while `gpu_shadow_ms` is 0.5). Instanced layer placements: 0.3 ms. Matters for hand-placed hero
      trees; see Phase 2.
- [x] Vista baseline (`--viewpoint Vista`, settled, 600 + 300 frames): **42.8 ms median**, p95 50.3.
      **CPU-bound, not GPU-bound:** GPU stages sum to ~13.4 ms (opaque 9.9: foliage depth 4.75, leaves
      2.0, other 2.7; clouds 0.8, shadow 0.95, terrain 0.7, grass 0.6), while update 5.1 + prepare 10.8 +
      record 25.4 (camera 18.1, shadow 6.3) run back to back and add up to the frame. `detail_scale`
      sits at its 0.6 floor. So the 60 fps floor on the vista is mostly a CPU draw-recording problem;
      Phase 2 needs a CPU item, not just GPU overdraw work.
- [x] Reference shots saved whole in `.local/reference-images/` (second set of eight; the first six in
      `first-set/`). No per-pose crops were made.

### Foliage and world realism / Foliage Phase 1: Leaves lit as volumes (biggest visual win, ~free)

- [x] **Crown normals + canopy occlusion** (cook step `tools/cook_foliage_lighting.py`): finds every
      alpha-masked, non-impostor leaf mesh through the woodland prefabs (30 chains, 76 meshes: oaks,
      conifers, pack bushes). Per chain, LOD0's leaf vertices are fitted with Gaussian lobes (k-means, one per
      ~4 m of crown, up to 8) whose sum is the crown field; every LOD gets the same crown. Each vertex:
      normal bent 75 % toward the field's outward normal (card normal flipped to the outer side first,
      tangent re-orthogonalised, handedness kept); `uv1.x` = canopy occlusion
      (`(1 - 0.8 * depth^0.85) * (0.4 + 0.6 * up)`, floor ~0.16); `uv1.y` = 4 + depth into the crown. The 4.0
      offset marks the mesh as cooked (leaf uv1 was 0 or a copy of uv). Cooking overwrites normals, so it skips
      cooked meshes; restore from git before re-cooking. `generate_conifers.py` writes
      `foliageTranslucency` and calls `cook_prefabs` after writing its meshes.
- [x] **Leaf lighting** (`Material::foliageTranslucency` 0-2, carried in `subsurfaceTransmission.y`; no
      object buffer growth). `Object.hlsl`: `GetFoliageLeaf`, `FoliageWrapExtra`, `FoliageTransmission`,
      used by `ObjectSurfacePS` (oaks, conifers) and `PSFoliageMain` (pack bushes; it now takes `uv1`).
      With it on: cooked normals are not flipped on back faces; ambient/environment × canopy occlusion;
      direct × `lerp(0.45, 1, occlusion)`; wrapped diffuse past the terminator; transmission = back term
      0.35 + forward scatter `pow(dot(-v, l), 4)` × 0.9, filtered yellow-green, × `lerp(0.08, 1, shadow)` ×
      occlusion², so the shell glows and the interior stays dark. Set on `Cluster_Mat 2` (1.0),
      `Conifer_Needles` (0.8), `phong3 1` bushes (1.0). Oak leaf `ambientColor` halved (it is a flat extra
      ambient on top of the SH sky light and lit crowns evenly from every side).
- [x] Cooked leaf normals that face away from the viewer (crown far side) are bent just into the view
      hemisphere (`n.v >= 0.1`) in `ObjectSurfacePS`, so GGX cannot divide by n.v ~ 0.
- [x] Check (foliage_test, `.local/foliage/{base,p1e}_*.png`): Backlit crowns have dark interiors and lit,
      glowing shells toward the sun (before: evenly lit flat green with white glints); looking up from
      ForestFloor the canopy has depth instead of uniform bright green; Meadow crowns read as rounded.
      Backlit looks the same on DX11 and Vulkan (`.local/foliage/p1_Backlit_{dx11,vulkan}.png`).
      Leaf GPU change +0.02-0.08 ms (ForestFloor 0.72 → 0.77, Backlit 0.56 → 0.64, Meadow 0.31 → 0.33);
      other stage differences are run-to-run noise. Tuning runs: p1a = translucency only (uncooked: whole
      crown glows evenly), p1b = cooked, p1c = + gloss, p1e = + darker undersides / halved ambient.
- [x] Vista (`.local/foliage/p1g_Vista`): far forest unchanged (impostors/clusters dominate, no visible
      near/far mismatch at this pose); GPU leaves 2.01 → 1.59 ms, GPU stage sum 13.4 → ~11 ms, i.e. no
      regression. Frame time can't be compared today: four runs with no relevant change measured
      42.8 / 30.7 / 27.7 / 21.9 ms, all stages moving together (CPU load on the machine).
      Final test-pose numbers (p1): ForestFloor 7.5 ms / leaves 0.76, Backlit 7.7 / 0.59, Meadow 4.6 / 0.33.

### Foliage and world realism / Foliage Phase 1b: Blocky bushes and the landscape demo (2026-11-19, user report: "bushes look blocky")

- [x] **Cause: grass LOD measured from height 0.** GrassField tile objects sat at y = 0 with ground heights
      inside the placements, so LOD selection and fades measured distance from a point tens (test cell) to
      hundreds (landscape) of metres above or below the plants: nearly every grass plant and bush drew LOD2
      at the viewer's feet. Fixed with `TileState::originHeight` (ground at the tile centre is the tile object's y; both build
      paths, procedural `AdvanceBuild` and authored `BeginBuild`, subtract it from placements and bounds;
      world positions unchanged). Grass now draws the detail it was tuned for: grass GPU on foliage_test
      2.0 → 3.3 ms (ForestFloor), 1.3 → 2.3 ms (Meadow); the detail budget balances it.
      Found by swapping LOD meshes (LOD2 := LOD0 fixed the near bush), then moving the layer's LOD
      distances to 30/35 m (no change), then a capture near the cell origin.
- [x] **Bush leaves** (`generate_meadow_grass.py`): ovate leaf outlines per LOD (10/6/2 tris) instead of
      diamonds, random leaf roll (no tiers), value jitter, gentler coarse LODs, no core at LOD0. Shared by
      meadow_bush(_tall), meadow_shrub and bramble.
- [x] **Landscape demo viewpoints** `tools/add_landscape_viewpoints.py` (also run by
      `build_landscape_demo.py`): **ForestEdge**, **ForestFloor**, **Backlit**, found near the start by
      replicating LandscapeDriver's `ForestMask`/`ForestBand` and reading ground from the height archive.
      The demo's start pose flies 25 m up, where grass and bushes (26 m radius) are out of view.
- [x] Checked on the landscape (`.local/foliage/land_*.png`, DX12): ForestEdge shows dense full-detail
      meadow grass and leafy bushes; Backlit crowns glow toward the sun; ForestFloor is bare forest soil
      with rocks and pale, over-exposed trunks (no undergrowth: Phase 4, and Phase 3 sky visibility).
      Timings (median frame / CPU record / GPU sum / foliage depth, ms): ForestEdge 27.1 / 16.2 / 14.7 / 5.1,
      ForestFloor 24.9 / 14.8 / 13.3 / 5.1, Backlit 26.3 / 14.5 / 14.2 / 4.2. **37-40 fps at ground level**:
      both CPU recording and GPU (alpha-tested foliage depth) are near 15 ms, so Phase 2 needs both.
- [x] **Dark diagonal stripes on trees** (user report): the per-placement LOD dissolve and the impostor view
      hand-over threshold each pixel with `ScreenDitherThreshold`, which was interleaved gradient noise. Its
      diagonal line structure showed through as dark bottom-left-to-top-right stripes on crowns sitting in a
      dissolve (the forest behind between them; ~1/3 of impostor angles are a hand-over). Now an 8x8 Bayer
      matrix (still complementary per pixel; no line structure). `.local/foliage/dither_zoom.png` vs
      `bayer_zoom.png`.
- [x] Shadow acne guards found on the way: cooked leaves offset their shadow lookup along the real card
      normal (`FoliageShadowNormal`, screen-space facet turned toward the light) instead of the crown-bent
      shading normal; tree-wind vertices pass their rest position to the pixel shader (only the rasterized
      position moves), so shading matches the static shadow casters.
- [x] **TAA on by default with an animated dissolve dither**: while TAA jitters a view, SceneRendering stamps
      the jitter index (1..8) into `ObjectGPUData::skinParams.z` (was unused) and `ScreenDitherThreshold`
      rotates each pixel's Bayer threshold by the golden ratio per frame, so the history resolves LOD
      cross-fades and impostor hand-overs to a smooth blend (`.local/foliage/taa2_zoom.png`; shifting the
      matrix instead left faint diagonal hatching). `AntiAliasingSettings::taa` default false → true and the
      editor's Balanced preset; Performance stays off, Quality stays MSAA 4x (static Bayer there, as on
      Vulkan, which has no TAA yet). Projects with saved Effects settings keep theirs.

### Foliage and world realism / Foliage Phase 1c: Tree variety (2026-11-19, user report: "a lot of the same trees")

- [x] Two species per size band per cell, mixed tree by tree (all eight can appear in one cell); scale
      0.75-1.3x; a lean via the placement normal (oaks up to ~7 degrees, squared so most stand straight;
      conifers 2.5). Derived from the existing per-tree draws, so positions are unchanged; far blocks copy the
      normal, so near and far trees agree.
- [x] Cost (single runs): draws +6-9 % (vista 298 → 316), triangles +1-3 %, no measurable frame-time change
      (vista 21.9 → 21.7 ms). Captures `.local/foliage/p3_*.png`.
- [x] Far clusters: 4 → 7 stands (`generate_forest_clusters.py`: Oak_03/Oak_04 from the oak variants the
      near forest leaves out, Conifer_03), all re-baked from the cooked meshes (crown normals in the normal
      atlases); `TerrainManager.farClusters` lists them (scene + builder). Bake ~8 min.

### Foliage and world realism / Foliage Phase 1d: Less cartoon (2026-11-19, user report: "style is very cartoonish")

- [x] **Plant palette** near measured albedo: grass blades were ~2x too bright and far too saturated
      (`BLADE_MID` 0.13/0.28/0.055 → 0.065/0.125/0.032, tips, sward, ferns, shrub/bramble/herb sun colours, accent
      stems and leaves likewise); dry blade share 10 % → 22 %. All grass/accents meshes regenerated.
- [x] `GrassLeafiness` is relative to brightness now (the absolute green difference would have halved for
      the darker palette and changed pelt shading).
- [x] Blade mottling in `PSGrassMain`: two octaves of world-space value noise (7 cm, 1.8 cm), ±14 % value with
      a slight hue shift, faded out by 25 m.
- [x] Terrain meadow to match: `grass.material` base colour 0.2/0.26/0.11 → 0.11/0.145/0.065, LandscapeDriver
      layer preview colour likewise.
- [x] Landscape tone mapping ACES → **AgX** (scene + builder).
- [x] foliage_test Meadow: ground between blades read paler than the grass, and tint strength 0.5 → 0.8 changed
      nothing. Cause found 2026-11-20: the forest-floor field cleared the meadow's ground cover every refresh, so
      there was no tint at all (look pass 1 under "Landscape demo expectations").

### Foliage and world realism / Foliage Phase 7: World detail (2026-11-19, user request: paths, streams, small things, cliffs/overhangs)

- [x] **Trails** (`LandscapeDriver.cpp` `TrailMask`): tracks follow the 0.5 contour of a broad noise
      (distance = value / gradient), so they wind across the whole map and join in loops; ~6 m wide (terrain
      vertices are 4 m apart). Painted as gravel on gentle land inside the forest band; trees, undergrowth
      and props stay off; grass and the forest floor follow the paint. `PaintRevision` bumped (terrain
      caches rebuild). `.local/foliage/w_Trail.png`.
- [x] **Cliff bands**: `GenerateCliffs` grows each steep-rock seed along the contour (up to 8 blocks a cell,
      was 2 lone blocks), every block facing downhill. New shapes from `generate_boulders.py`
      (`Rock.overhang` shear): `Cliff_overhang_01` (top leans ~3 m over its foot) and `Outcrop_ledge_01`.
- [x] **Scree**: rocks and boulders gather where a steep slope lies 9-18 m uphill.
- [x] **Forest debris**: `forest_branch` (fallen branch with twigs) and `ground_ivy` (creeping ivy patch) in
      the forest-floor drifts.
- [x] Steep rock rendered near black: the canopy-shade estimate's shadow taps landed inside the hillside.
      Canopy shade now fades out on terrain steeper than ~35 degrees.
- [x] Crags: `Crag_face_01/02` (28-38 m wide), `Crag_spire_01` (30 m tall), LODs 20480/5120/320 tris; up to 40
      pieces a cell (was 8), strides sized per piece so neighbours overlap, leaned a third with the slope.
      `.local/foliage/w_Cliffs3.png`: a rocky outcrop instead of scattered boulders.
- [x] **Scanned rock faces** (downloads approved): `tools/fetch_polyhaven_props.py` fetches CC0 Poly Haven
      models into `.local/polyhaven` (FBX + 2k diffuse/normal/roughness JPGs), cooks LOD chains with PropCook
      and writes materials and prefabs under `Models|Prefabs/Woodland/Scans` (62 MB): `RockFace_01/02`,
      `Mountainside_01`, `Cliff_Scan_01/02` (8-20k / 2.5-6k / 0.5-1.7k tris). `GenerateCliffs` now places
      these as the main pieces (scaled 2.2-4.5x to 20-45 m), generated spire/overhang/ledge as filler, only
      where the slope keeps rising behind them. Materials are two-sided with a cool tint (the scans are warm
      sandstone, the terrain rock grey). `.local/foliage/w_Cliffs4.png`: reads as fractured, bedded rock.
      Pitfalls: PropCook is `EXCLUDE_FROM_ALL` (a stale binary wrote one prefab per LOD; rebuild it by
      name); the scans are open shells, so a camera behind one sees its back.
- [x] **Scanned forest props** (Poly Haven, CC0, via `fetch_polyhaven_props.py`; sets cook one prop per
      mesh node, leaf scans merge their Alpha map into the colour): `Stump_01/02`, `MossRock_01-06`,
      `DryBranch_01-03`, `FernScan_01-04` (masked, `foliageTranslucency`). Placed by `GenerateProps`: stumps
      with the logs, dry branches on the forest floor, mossy rocks instead of plain rocks inside forest,
      ferns in drifts in the undergrowth layer; one stump, one branch, two rocks and two ferns per cell.
      `Models/Woodland/Scans` is 107 MB. `.local/foliage/s_ForestFloor.png`, `s_Trail.png`.

### Foliage and world realism / Foliage Phase 2: Pay for it (performance)

- [x] **Indexed woodland meshes** (`tools/index_woodland_meshes.py`): the oak/bush pack meshes were plain
      triangle lists; only 35 % of their vertices were unique (148 meshes, 1.06M → 377k vertices). Rewritten
      indexed (byte-identical vertices merged, first-use order). foliage_test GPU: bark 1.39 → 0.79 ms,
      foliage depth 1.40 → 1.12 ms (ForestFloor); identical image.
- [x] `PrefabLod::IsPrefab` built a `std::filesystem::path` (with a wide-string conversion) per call, once per
      placement of each object layer it was checked on; now an allocation-free suffix test (same `#lod`
      rules). `PreparePrefabInstances` only rescans placements for prefabs when they change.
      `prepare_prefabs_ms` 3.0 → 2.2.
- [x] Occlusion signature hashed ~90 bytes per visible item one byte at a time (FNV); now 8 bytes per step.
- [x] Camera draw records (`PreparedDraw`, ~2 KB) are built in place in the reserved vector instead of
      zero-filling a local and copying it (`_NLG_Return2` under `push_back` was ~5 % of the thread).
- [x] CPU, round 2 (from a `--cpu-sample` profile at ForestEdge): prefab LOD profiles refreshed every
      100 ms per forest/props layer (~10 % of the scene thread in string work) → every 1 s, matching the
      file-stamp checks underneath; `TransformHierarchy` membership by a process-unique stamp on each
      Transform instead of an `unordered_map` lookup per render candidate per frame (~3 %). Machine-wide
      CPU swings (±30 % between identical runs today) hide these in single-run frame times; confirm with the
      profiler's shares or interleaved A/B.
- [x] CPU, round 3 (profiles `.local/foliage/prof3` → `prof6`, landscape ForestFloor, profiler attached:
      frame 34.0 → 27.1 ms, prepare 9.2 → 6.4, camera record 15.5 → 12.0; without the profiler 25.5 ms):
  - Prefab preparation ran to its full 3 ms deadline every frame even with nothing to build. The
    deadline is now 0.75 ms once every object layer reports its prefabs complete, 3 ms while any is
    still building (`m_prefabPreparationSettled`). `prepare_prefabs_ms` 3.0 → 0.8.
  - The detail-budget cost model copied the placements of every visible instanced tile on the scene
    thread at each launch (every 100 ms; `memmove` ~5 % of the thread, and a spike on those frames).
    Placements are now snapshotted once per tile revision and shared with the job
    (`DetailCostCollector::snapshots`, dropped ~3 s after a tile leaves view).
  - A view's draw records (~2 KB each, a few thousand per view) lived in a vector allocated and freed
    per `Render` call: several MB of fresh pages per view per frame. Storage is now leased from a
    per-thread pool (`PreparedDrawLease`); the camera path also fills the object record in place.
    Camera material loop 4.8 → 3.3 ms.
  - `TransformHierarchy::Prepare` (called twice a frame over 10.4k transforms) settles unchanged frames
    in one pass instead of building the parent list and scanning again. Small: 6.2 → ~5 % of the
    thread; the cost is touching every Transform, which needs dirty tracking to remove.
  - Cached frame items are copy-constructed in place; texture footprints refresh every 8 frames (was 4):
    footprint stage 2.8 → 2.0 ms.
  - The "is this static item unchanged" test runs on the workers for members of the prepared transform
    hierarchy (merged into the material validation pass; `TransformHierarchy::IsPreparedMember`);
    the serial loop only copies the item. `prepare_items_ms` 4.1 → 3.0-3.7.
  - The per-frame render packet is recycled (`NativePacketRecycler`): its ~3 MB item list was a fresh
    allocation every frame. `prepare_items_ms` → 2.6, `scene_prepare_ms` 6.2 → 4.7.
  - Checked on landscape ForestFloor, foliage_test ForestFloor and the animation showcase (animation
    still advances), DX12 only. One of fourteen landscape runs ended during loading with zero frames
    and no error text (the window-closed path); it did not recur in the seven runs after it.
- [x] **Moving camera, first fix.** Still-camera numbers hid the real problem: turning in place had a
      63 ms p95 and walking spiked to 150-230 ms. Cause: `Scene::MoveObject` ended with a whole-scene
      `NotifyStructureChanged()`, so every far terrain tile, far-object block and streamed cell that was
      parented under the terrain manager reconciled the component registry and cleared the static
      render item cache (all 9,124 items rebuilt, ~40 ms a frame for 3-4 frames, about once a second
      while turning). Appending a child now notifies only that object, as `SetParentLocal` does.
      Landscape, 400 frames each (median / p95 / frames over 50 ms): turning 24.3 / 63 / 41 →
      24.7 / 38 / 3; walking 35.0 / 84 / 49 → 31.3 / 51 / 20; walking and turning 39.5 / 90 / 91 →
      35.2 / 59 / 34. World intact after the walk (`.local/foliage/mv2_walkturn.png`).
- [x] **GPU occlusion pyramid from real depth** (`Shaders/DepthPyramid/DepthPyramid.hlsl`,
      `SceneRendererLighting::BuildDepthPyramid` / `DepthPyramidForCulling`). After the AO pass copies the
      main view's depth, one compute dispatch reduces it to the CPU occlusion buffer's GPU layout
      (`OcclusionBuffer::SerializeForGpu`: 256-wide, 4 levels, farthest view depth per cell, 3e38 for sky, the
      frame's view-projection in the header). Next frame, InstanceCull's existing per-placement
      `IsBoxOccluded` reads it instead of the CPU pyramid (terrain + mesh occluders only) when the camera is
      the same, the viewport unchanged, it is one frame old, and the camera moved < 1.5 m and turned < ~8
      degrees; otherwise the CPU pyramid as before. No new binding slots; Vulkan SPIR-V prebuilt in CMake.
      `ENGINE_GPU_OCCLUSION=0` disables it. Needs the AO depth copy (AO on). Landscape ForestEdge A/B (DX12):
      GPU 15.75 → **11.45 ms**, foliage depth 5.13 → 3.38, leaves 2.26 → 1.18, GPU-selected LOD0 placements
      59,623 → 360, LOD2 9,362 → 2,130; image diff at the noise floor (0.3 % px > 40, wind). Runs on DX11
      and Vulkan. Single-phase (last frame's depth): a newly disoccluded object can lag a frame; a second
      phase against this frame's depth would close that.

### Foliage and world realism / Foliage Phase 3: Shadows and sky visibility (dappled light)

- [x] **Canopy sky visibility** without a new texture (a global texture slot is still missing on the three
      backends): `CanopySkyVisibility` (Object.hlsl) averages five sun-shadow taps on a 3 m disk, 2.5 m above
      the surface (`FogShaftSunVisibility`, one bilinear tap each), and scales ambient/environment light to
      35-100 % with it; fades out over the last 30 % of the shadow range. Applied to terrain (both paths),
      objects (not cooked leaves, which have crown occlusion), pack bushes and grass. Cost on foliage_test
      ForestFloor ~0.5 ms GPU (grass 1.31 → 1.61). The estimate sits a little downwind of the crowns.
- [x] **Exposure floor**: auto exposure brightened forest interiors back to the meadow's level, undoing the
      darkening. `autoExposure minEv` -8 → -4.8 on the landscape (builder sets it; test scenes copy it): sunny
      meadows meter above it (Meadow pose unchanged), forest interiors stay ~1 stop darker
      (`.local/foliage/ev4.5_ForestFloor.png` is the -4.5 look). A soft partial adaptation (only part of the
      way below a reference EV) would be smoother than a hard floor; not needed yet.
- [x] **Cloud shadows** as a scrolling sun cookie: `Light::cookieScrollX/Y` (m/s) fill the cookie axes'
      spare `.w` lanes; `DirectionalCookie` / `SunCookie` (Object.hlsl) apply the directional cookie in every
      sun path (objects, fast near terrain, distant terrain per pixel and per vertex, pack bushes, grass).
      `generate_sky_sprites.py` `cloud_shadow` writes `Textures/Atmosphere/Cloud_Shadow.png` (tiling, linear:
      1 clear, 0.3 under cloud, ~38 % cover). Landscape sun: 2.4 km tiles drifting 6/2 m/s. Statistically
      like the volumetric clouds, not positionally matched to them. The test scenes strip the cookie (fixed
      poses for A/B). `.local/foliage/p7_Vista.png`.
      **2026-11-20: from the capture tool the cookie never loaded** (unresolved asset path), and the null
      texture switched the sun off; fixed, and 0.3 → 0.42 under cloud (look pass 1 under "Landscape demo
      expectations"). `p7_Vista.png` therefore did not show cloud shadows.

### Foliage and world realism / Foliage Phase 4: Forest floor and undergrowth (RDR2 / GTA VI shots)

- [x] Generators (`generate_meadow_grass.py`, forest floor section): `forest_litter` (fallen-leaf patch with
      twigs, static), `forest_herb` (broadleaf rosette ~0.45 m), `forest_seedling` (oak seedling), `forest_fern`
      (~1 m, darker than bracken; `meadow_fern` is now `fern()` with defaults, byte-identical output),
      `dry_grass_clump`. LOD0 triangles: litter 468, herb 126, seedling 104, fern 1120, dry grass 1080.
- [x] **Forest floor layer**: `MeadowGrass::ApplyForestFloor` for a "Procedural forest floor" object layer
      (litter 2.2/m², herbs 1/m², seedling/dry-grass/plantain drifts, forest fern + herb thickets over 45 %,
      sparse bushes; no ground tint). `LandscapeDriver` points it at the ForestSoil paint; the layer is in the
      demo's `objectLayerNames` (scene + builder). foliage_test paints forest soil under the grove (soft 8 m
      edge) and carries the same field. `.local/foliage/p4b_ForestFloor.png`: ferns, herbs and litter where
      the floor was bare soil.
- [x] Canopy-driven mix, by paint: the meadow field grows on grass paint and the forest floor field on the
      forest-soil paint the trees stand on, so edges blend with the paint weights.
- [x] Forest-floor terrain layer: the landscape already paints `forest_soil` under forests (LandscapeDriver
      `PaintSample`); the litter field covers it.
- [x] CC0 scanned plants for hero undergrowth: Poly Haven ferns, stumps, mossy rocks and dry branches are in
      (Foliage Phase 7, scanned forest props). Scanned shrubs are the open part, listed there.

### Foliage and world realism / Foliage Phase 5: Trees

- [x] **Tree wind** without new vertex data (`GrassWind.hlsli` `ApplyTreeWind`, `Mesh::foliageWindTree`,
      `grassAnimation.y` = 2): a cantilever trunk bend (strength x 0.01 x h², mean lean + broad gust + ~0.26 Hz
      per-tree sway), ~0.7 Hz branch sway growing with distance from the trunk axis, ~4 Hz flutter on
      cooked leaves weighted by the canopy shell (uv1). Same wind field as the grass, coarsened so crowns
      move as one. Fades out 60-100 m; shadow casters static (as grass). On for all woodland prefabs (oaks,
      conifers 0.1; pack bushes 0.15) and in `generate_conifers.py` / `woodland_assets.write_prefab`
      (`tree_wind`). `ENGINE_TREE_WIND=0` disables it. foliage_test: crowns move between frames, ~+0.5 ms
      GPU. Note: wind meshes are left out of the CPU mesh occluders (opaque coarsest bands only; trees
      hardly qualified).

### Foliage and world realism / Foliage Phase 6: Air and camera

- [x] **AgX** tone mapping (`ToneMappingOperator::AgX` = scene 3 → PostProcess.hlsl operator 6; polynomial
      fit of Troy Sobotka's AgX; editor Output Tone Mapping combo). Lifted shadows, softer highlights and less
      saturated sky/greens than ACES (`.local/foliage/agx_*.png` vs `aces_*.png`). ACES stays the default;
      a per-scene choice.

### Grass / Grass Phase 1: Per-plant distance fade (pop-in)

- [x] Vertex shader (Object.hlsl `VSMain`, after the world position): per placement,
      `s = 1 - smoothstep(start, end, distance(root, camera) + advance)`. `advance` is a per-placement hash
      (`GrassWindHash` of the root, 6 cm cells) × 0.35 × band. It only brings the fade forward, so every
      plant is gone by `end` and the edge never forms a ring.
- [x] The plant shrinks toward its root along its own up axis (correct on slopes): height × `s`, width ×
      `lerp(0.5, 1, s)`, collapsed to the root at `s = 0`. Wind bend height is scaled by `s` too.
- [x] New `ObjectGPUData::placementFade` (x start, y end), filled from new transient
      `Mesh::placementFadeStart/End` for both color and shadow draws, so shadows/depth match.
      Object buffer stride 1792 → 1808 (static_asserts and both shader structs updated).
- [x] GrassField: new `distanceFadeFraction` (default 0.3 of the radius). When > 0 the tile `visibility`
      range dither is off (the `appearance` fade for newly built tiles stays); 0 falls back to the old
      `distanceFadeWidth` tile fade. Distances are converted to world units with the terrain's scale.
- [x] Check: grass_test.scene capture shows no tile squares or ring at the edge.

### Grass / Grass Phase 2: Terrain matches the grass (coverage + distance)

- [x] `GrassLeafiness` moved out of the grass-only `#ifdef`; new `GrassPatchVariation` (the broad 11 m
      dry/lush pattern only) is shared with the terrain. The per-clump jitter stays grass-only because its
      25 cm cells would show as a checkerboard on the ground. Kept in Object.hlsl rather than a new include:
      GrassWind.hlsli is already included there unconditionally.
- [x] New transient `Terrain::GroundCover` (colour, strength, root shade, fade start/end, paint layer mask)
      → `ObjectGPUData::groundCoverColor/groundCoverRange` in `PrepareTerrainMaterial`.
      Object buffer stride 1808 → 1840.
- [x] Terrain shader (after layer blending, before the macro-colour fade), weighted by the cover layers'
      share of the paint weight:
  - inside the grass range: root colour (`color × rootShade`) so gaps read as undergrowth;
  - across and past the grass fade: the mean blade colour;
  - both with `GrassPatchVariation`; the layer texture survives as brightness detail (60% near, 30% far).
- [x] GrassField publishes the cover (`PublishGroundCover`, from `Start`, `Refresh` and
      `DescribeSpatialRegion` so cells outside the spatial region are tinted too). Colour = mean vertex
      colour of the short + tall grass meshes (LOD0), measured once per mesh pair from the shared CPU
      source cache. Only procedural fields with a `proceduralLayerMask` publish. New properties
      `groundTintStrength` (0.75 at first, 0.5 since the tint tuning) and `groundRootShade` (0.55), category
      "Grass | Ground".
- [x] Tint tuned on the landscape: the first pass made the meadow a flat saturated lime; now cover colour
      × 0.8 with 25 % desaturation, strength 0.5 and more texture detail kept (80 % near / 50 % far).

### Grass / Grass Phase 3: Density falloff with width compensation (coverage at mid range)

- [x] Shader-side thinning (Object.hlsl VSMain, `ObjectGPUData::placementThin`, `Mesh::placementThin*`):
      from a fifth of the radius to the fade start the share of plants kept falls to
      `GrassField::distanceThinning` (default 0.45; 1 = off). Each plant has a fixed random rank and
      shrinks away as the share passes it (no popping); survivors widen sideways by `1 / sqrt(share)`.
      Object buffer stride 1840 → 1856. Grass GPU time on grass_test.scene (DX12, raised pose):
      3.52 → 1.97 ms with the same plant count (collapsed plants skip rasterisation).
- [x] Thinning also in InstanceCull.hlsl: the thin range rides in the tile record's spare
      `reserved` uvec4 (`InstanceTileRecord`, filled from `placementThin` in SceneInstanceBatching);
      compute drops placements the vertex shader would shrink to nothing (same hash and share formula),
      so they cost no vertex work either.

### Grass / Grass Phase 4: Lighting as one surface

- [x] Pelt shading in `PSGrassMain` (Object.hlsl): normals lean toward the sky so a field lights as one soft
      surface, wrapped diffuse, root occlusion along the blade (uv.y), transmission when the sun is behind
      the blades, and a Kajiya-Kay sheen along each blade.

### Grass / Grass Phase 6: Grass texture past the radius (flat distance)

- [x] **6a:** terrain shader (ground-cover block, weighted by `beyond`): two octaves of fine
      brightness grain (1.7 and 6.3 /m, each faded as it nears the pixel footprint so it cannot
      flicker) and a grazing-angle sheen, so open ground past the blades reads as short grass.
- [x] **6b → far grass ring** (geometry instead of cards; no alpha textures needed):
      `Procedural far grass` layer per terrain cell (`MeadowGrass::ApplyFar`, LandscapeDriver,
      landscape_demo layer list, test scene). A second GrassField with `innerRadius`: 16 m tiles of
      `far_grass_tuft` (12 wide two-segment blades, 36 tris, same mesh for every LOD) at 1.2/m² out to
      90 m. Tiles wholly inside `innerRadius` are skipped; it fades in by running the thinning backwards
      (`placementThin` start > end, minimum 0, `w = 1` keeps width). Its fade-in must finish before the
      detailed grass starts fading (inner = 0.5 R, width 0.2 R), or a band of bare ground shows between
      the two. It does not publish the ground tint. Placement caps scale with tile area.
      First test (fades not yet overlapped): far-band terrain 86 % → 59 %, eye-level overall 22 % →
      17 %; grass GPU 2.77 → 3.27 ms, 5.9k → 13k plants, 100 → 106 tiles.

### Grass / Grass Phase 6½: Antialiasing (grass edges stair-step and shimmer)

- [x] **FXAA** inside the compose pass (`PostProcess.hlsl` `ApplyFxaa`): console-style edge blend plus
      a sub-pixel pass, on HDR before bloom/exposure/tone mapping, with edges found on exposed,
      compressed luma. No new targets or passes, so DX11, DX12 and Vulkan all get it; settings travel in
      compose `params0.w` (negative = FXAA), which only video accumulation used before.
- [x] **Grass minimum width** (`Object.hlsl` VSMain, grass shader only): blades narrower than N pixels
      widen across their UV u axis; centre-line vertices (midribs, tips) stay. `placementFade.zw` carry
      N and the world size of a pixel per unit depth (`2 / (proj[1][1] · viewport height)`).
- [x] **Project settings** `ProjectSettings::renderingEffects` (`Model/RenderingEffectsSettings.h`),
      saved as `<RenderingEffects .../>` in the .proj, edited in Preferences > **Effects** (presets
      Performance / Balanced / Quality; dependent controls grey out). Applied through the process-wide
      `Rendering::RenderingEffects` policy (editor load, Preferences changes, game start). Also project
      switches/scales over scene look: bloom (+scale), auto exposure, exposure bias (EV), colour grading
      (+strength toward neutral), ambient occlusion.
- [x] Checked on grass_test.scene (DX11): stair steps and single-pixel sparkles gone; distant blades read
      as solid strokes. Image slightly softer at the default 0.75 subpixel.

### Grass / Coverage test (render mode)

- [x] `SceneRenderMode::Coverage` (6): terrain red, everything drawn on it green (reuses the Performance
      heat colours via `morphParams.z`). Editor: Scene view render-mode list > Coverage. Captures:
      `GameBackendCapture --render-mode 6`. Measure with a red/green pixel count (terrain share of the
      ground pixels, split into near/mid/far screen bands).

### Grass / Grass Phase 8: Larger, thicker foliage

- [x] `tools/generate_meadow_grass.py`: `meadow_bush` (~1.2 m rounded broadleaf), `meadow_bush_tall`
      (~1.7 m upright), `tall_grass_clump` (~1.1 m dense grass with seed stalks). Bushes have a solid
      **crown core** (closed lumpy ellipsoid per lobe, dark foliage colour) under the leaf cards, so they
      block the view at every LOD; `meadow_shrub` gets the same core.
- [x] `GrassField` **Bushes** category: `bushMeshes`, `bushesPerSquareMeter`, `bushClusterSize`,
      `bushCoverage`; clusters from the generalised thicket noise plus a few singles; grass thins under
      clusters. Meadow preset / LandscapeDriver / test scene: bushes 0.06/m², clusters 16 m, coverage
      0.25; `tall_grass_clump` added to the shrub-thicket species.
- [x] Thicker base cover: `meadow_tussock` sward 84 → 150 blades, 2.0 → 2.6 cm wide, 0.22 → 0.26 m;
      LandscapeDriver/test scene: tussocks 2.6 → 3.5/m², bushes 0.12/m², bush coverage 0.35.
- [x] Bushes rebuilt as **grown plants** (`grow_bush`): 6–7 main stems → branches → twigs (four
      levels, tapered round tubes, curving outward from the trunk axis and up, each growing a share of
      the way to an ellipsoid envelope so twigs reach it), clusters of 8 leaves (6.5–8 cm on short
      stalks) at twig tips and along twigs, and a small dark core fitted to the foliage (35 % of its
      spread at LOD0, 80 % at LOD2) as the shadowed interior. One layout for every LOD; coarse LODs
      drop fine wood and keep fewer, larger leaves. meadow_bush 8780/4292/1898 tris,
      meadow_bush_tall 7544/3730/1794. (A first version centred the core on the envelope and aimed
      child branches outward from the centre, so low branches pointed down: the crown stayed empty
      and the core read as a smooth egg.)
- [x] Shrub/bush leaves (`generate_meadow_grass.py`): `leaf_card` is now an ovate leaf per detail
      level (10 / 6 / 2 triangles; raised midrib, back-curl) instead of a 4-vertex diamond; `grow_bush`
      leaves get a random roll (they lay in parallel tiers), ±25 % value jitter and occasional yellowing;
      coarse LODs keep more, smaller leaves (`LEAF_KEEP` 1/.6/.35, `LEAF_GROW` 1/1.25/1.6, was 1/.46/.2 and
      1/1.45/2.15: 26 cm diamonds at LOD2); no crown core at LOD0 (it read as an egg), smaller at LOD1/2.
      meadow_bush 8780/4292/1898 → 32844/11954/2718 tris, meadow_bush_tall 7544/3730/1794 → 28152/10562/2438.

### Grass / Grass Phase 7: Mesh/coverage tuning (optional)

- [x] 2026-11-19: blade meshes reworked (`tools/generate_meadow_grass.py`): leaf outline (narrow sheath,
      widest ~20% up, parallel margins, long tapered tip) instead of a root-to-tip triangle; V-folded
      cross-section with a sunken, paler midrib (3 columns at LOD0/1); 5/4/3 segments (was 4/3/2);
      ±12% brightness per blade; seed stalks are 3-sided tubes at LOD0/1. Triangles: meadow_tussock
      798/340/88 → 1824/709/126, field_tussock 580/290/101 → 1390/671/149.

### Landscape look (large-scale vista with layered haze) / 0. Baseline + test shot

- [x] Fixed-pose capture: `GameBackendCapture --camera-position x,y,z --camera-target x,y,z` (recorded in the session header)
- [x] GPU `Sky` timing stage (`gpu_sky_ms` in captures and the editor frame profiler)
- [x] `ENGINE_ATMOSPHERE_FOG=0|1` env override for fog A/B captures
- [x] Rough vista pose (peak over a valley, see command below)
- [x] Vista pose is a scene viewpoint: inactive Camera object "Vista" in landscape_demo.scene (emitted by
  `tools/build_landscape_demo.py`, `VISTA_EYE`/`VISTA_TARGET`). Scene View toolbar "Viewpoints" menu jumps the
  editor camera to any scene camera and sets the selected camera from the current view
  (`Camera::GetWorldPose/SetWorldPose`); captures take `--viewpoint Vista` (recorded as `viewpoint` in the header)
- [x] Vertical dashed line in the sky: the equirect panorama's u wrap (atan2 jump at world -X) gave implicit
  derivatives a full-texture step, so 2x2 quads straddling it sampled the smallest mip. Skybox.hlsl now uses
  `SampleGrad` with wrap-aware u derivatives (checked on DX12, DX11, Vulkan)
- [x] Black sky after the 2026-11-17 architecture merge: the D3D `SkyboxCB` kept `float4x4 invVP` ahead of the new
  ray-basis fields while C++ `SkyboxConstants` dropped it, so every sky/fog/sun constant read 64 bytes off. Removed
- [x] DX11 vista differed from DX12/Vulkan (weaker sun glow, sparser far forest): not a rendering difference.
  DX11 captured the composed back buffer, DX12/Vulkan the HDR scene color before post-processing with a plain
  2.2 gamma, so their captures had no bloom, tone mapping or grading. DX12 now copies the back buffer after
  compose, Vulkan the swap-chain image after composition (swap chain gets `TRANSFER_SRC` usage; BGRA swizzled).
  Second cause: benchmark runs passed the host's real input to the game, so a capture window that ended up in
  the foreground let the user's mouse turn the camera during warmup (runs of one viewpoint diverged; Vulkan was
  off by ~40 degrees once). Benchmarks now get an unfocused, key-less snapshot; only synthetic input drives them.
  Vista now: DX12 == DX11 pixel for pixel, Vulkan differs on 0.22% of pixels, all three at the exact viewpoint.
  Captures taken before 2026-11-18 on DX12/Vulkan show pre-post-processing images
- [x] Vista pose re-picked for the 24 km map: eye (2304, 463, -3072) on a 410 m foothill at the valley's south-east
  rim, target (283, 254, -865): across the valley and a lake at the backlit north-west ranges, dark canopy in the
  foreground. Chosen from candidates found by searching the far grid for rim peaks with a deep drop toward the
  sun's azimuth and high ranges behind (A2 at (2688, 972, -4352) is a wider, flatter alternative). Set in
  `VISTA_EYE`/`VISTA_TARGET` and the scene's "Vista" camera. Numbers above this line used the old pose
- [x] Depth precision: forward Z with D32F and near 0.06 gave ~25 m depth steps at 5 km and ~100 m at 10 km. Every
  depth buffer (camera, portal views, shadow atlases) is now reverse-Z; `Graphics/DepthConvention.h` holds the
  convention (clear 0, `CompareNearer` = GREATER as the builder default, `CompareNearerOrEqual`). Projections are
  built with near/far swapped in double (`Camera::GetRenderProjectionMatrix`, `BuildStableShadowCascade`);
  `Camera::GetProjectionMatrix` stays forward-Z for editor picking/overlay math. Shader sites: shadow compares
  (`receiver + bias >= stored`), skybox/grid far-plane triangles and grid plane depth, fluid front depth, parallax
  `SV_DepthLessEqual`, portal aperture near clamp (`z = min(z, w)`) and depth-reset far plane (`z = 0`),
  sky ray unprojection, `ProjectedTextureFootprint` near test. The software occlusion buffer and GPU cull work in
  clip w, and frustum tests are symmetric in z, so neither changed. Verified: DX12 vista vs HEAD 0.10% of pixels
  differ (noise floor 0.01%, all in distant canopy), Vulkan matches, DX11 unchanged apart from its known sky
  difference; portal_traversal_demo, room_larger_inside (DX12/DX11/Vulkan), particle_fluid_river and
  surface_detail_materials match HEAD up to animation/streaming timing; `.local/tests` pass. Vista frame time
  unchanged (A/B on the same machine: HEAD 26.3/27.0 ms, reverse-Z 25.9/26.5 ms). Not checked: editor grid
  (editor-only pass, math changed in `Grid.hlsl`)
- [x] Vista "regressed" from ~10 ms (2026-11-16) to ~26.5 ms at `ec249b3d`. Two causes, neither a code regression:
  - Unfocused captures were throttled. Since background launches (`ec249b3d`), the capture window never has focus
    and Windows 11 power throttling (EcoQoS) ran every CPU stage 2-3x slower on this hybrid CPU. The old 5.2 km
    world at the old pose, current code: 25.2 ms throttled, 10.4 ms with `Engine::Core::DisablePowerThrottling()`
    (`Startup/LaunchActivation`), i.e. the 11-16 number. GameBackendCapture always opts out; Game/Editor opt out
    when launched in the background. Captures from before 11-17 18:00 ran focused; later ones before this fix were
    throttled, so neither set compares with new numbers
  - The 24 km world is heavier at the same pose: 16k far-object cells (radius 64 x 128 m) instead of ~1.6k
  - [x] `TerrainManager::UpdateFarObjects` rescanned the whole far window each frame (map lookups per cell, a sort
    of every uncomputed cell) and rebuilt a block after every 8-cell batch: 4.4 ms/frame plus constant prefab
    re-preparation. Now incremental: the nearest-block-first queue is rebuilt only when the center cell changes,
    only loaded/latched cells are checked for near coverage, and a block rebuilds once its data is complete
    (coverage/radius changes still apply at once). 2-6 far jobs instead of 2. 4.4 -> 0.02 ms; drive-forward
    A/B: median 29.0 -> 21.4 ms, far time over the drive 7.7 s -> 0.46 s
  - [x] Far blocks re-hashed every placement once per second per block (`GetPlacementSignature`) to catch
    in-place edits they never get: `TerrainObjectLayer::placementsChangeOnlyWhenMarked`. Prefabs 3.0 -> 1.4 ms
  - [x] Texture footprints: shadow cascades are orthographic, so each instance chunk's world box with its
    largest scale bounds all its shadow demand in one projection; chunks 4x their diagonal away also use the box
    for the camera. Per-instance work is now camera-only for near chunks. 5.2 -> 2.5-2.9 ms, image identical
  - Result (DX12, 1920x1080, settled after 5000 warmup frames): 24.8 -> 16.8-20.7 ms. CPU path now update 1.7 +
    prepare 5.0 + record 10.5 ms; the GPU varies 11-20 ms between runs (other apps share the GPU)
- [x] Benchmarks measured while far objects still streamed (600 warmup frames was ~half of the 16k cells).
  `--wait-for-residency` now also waits for `TerrainManager::GetFarObjectStatus().settled` (window computed, no
  jobs or dirty blocks, every block's instances prepared, latched per block since its last rebuild); frame logs
  carry `far_object_cells`, `far_object_cells_queued`, `far_object_blocks`, `far_object_blocks_preparing`,
  `far_objects_settled`. The timeout message prints which condition is missing. Vista settles after ~109 s.
  - [x] Found on the way: the prefab source stat cache (`PreparePrefabInstances`) dropped each entry when it went
    1 s stale, so a layer visited less than once a second (round-robin under the 3 ms budget, ~300 layers) found its
    sources pending on every visit; a far block whose prefabs no other layer kept fresh never built. Stale entries
    now keep serving until the refresh lands
  - [x] Fully settled vista measured 40.1 ms median / 47.5 p95 (DX12, one run), far above the 16.8-20.7 ms
    recorded with half the far cells streamed. Stage breakdown (Foliage Phase 0, 42.8 ms): CPU-bound, update 5.1 +
    prepare 10.8 + record 25.4 back to back, GPU ~13.4. After the foliage CPU rounds and the GPU occlusion
    pyramid: ~25.7 ms (2026-11-19)
- [x] Turning stuttered: `PersistentInstanceBatches` keys include bound texture and mesh-buffer pointers, which
  texture streaming replaces constantly while the view turns, so every new key created four D3D12 buffers (16% of
  the simulation thread, upload p95 8.7 ms). Batches unused for 8 frames now park their buffers per frame slot and
  new keys reuse them. Look (adaptive detail): median 15.2 -> 13.9 ms, p95 24.8 -> 19.2, max 38 -> 26
- [x] Software occlusion buffer (rebuilt every frame while turning) now starts in the camera's shadow pass, which
  runs first and computes the same view; the color pass adopts it when camera, frame serial, viewport and
  view-projection match bit for bit (`EarlyCameraOcclusion`, joined by every color pass, frame preparation and
  `FenceBackgroundWork`; `ENGINE_EARLY_CAMERA_OCCLUSION=0` disables). Interleaved A/B while turning: occlusion
  wait 0.78 -> 0, command record 7.0 -> 6.5 ms, frame 13.1 -> 12.6 ms
- [x] `Material::PrepareTextures` residency check moved into the parallel validation pre-pass
  (`NeedsTexturePreparation`, read-only); serial texture preparation 0.37 -> 0 ms, items 2.33 -> 2.11 ms
- [x] Shader prewarm skips materials whose shader, asset path and terrain use are unchanged since their last
  prewarm under the same epoch (`ShaderSourceCache::Generation()` + `m_materialShaderBytecodeEpoch`); the check
  runs in the parallel pre-pass. Prewarm 0.29 -> 0 ms, items 2.0 -> 1.5 ms, prepare 2.9 -> 2.46 ms
- [x] Tried: texture pointers out of the instance batch key (per-frame occurrence ordinal instead). No
  measurable change (upload 0.40 vs 0.38 ms while turning): the remaining upload is visible tile membership
  changing with the view, not texture churn. Reverted
- [x] Detail budget oscillated: the predictive mode grew its cost budget whenever the GPU was under budget, but the
  GPU measures the *applied* scale, which glides far behind the target, so the budget wound up (0.49M -> 1.37M,
  target at the 8x maximum) and geometry overshot to 10-14 ms. Now the budget comes straight from a calibration
  (smoothed GPU ms / smoothed predicted cost of the applied scale), cuts in 0.2 s and grows over the response time;
  the controller follows a lower target in 0.1 s; and when *measured* GPU time is >1.1x the budget, detail is shed
  up to 5x faster than the anti-popping glide. `.local/tests/DetailBudgetTests.cpp` runs the closed loop in
  simulated time (static, fast view changes, and a model whose predictions run opposite to real cost).
  Captures (adaptive): static 12.0/14.8/19.1 -> 10.6/12.5/13.9 ms (median/p95/p99), geometry max 10.2 -> 7.0;
  fast turn 13.9/19.2/23.0 -> 12.6/16.5/19.1, geometry max 14.2 -> 7.4

### Landscape look (large-scale vista with layered haze) / 1. Aerial perspective + height fog

- [x] Per-pixel analytic height fog in the forward shaders via one shared include (`Shaders/AtmosphereFog.hlsli`); no per-material fog code
- [x] Extinction/inscatter blend: `color * T + inscatter * (1 - T)` (Beer–Lambert), exponential height falloff so haze pools in valleys
- [x] Henyey-Greenstein sun-glow term for in-scatter toward the sun, using the scene's main directional light
- [x] Sky horizon haze from the same parameters (skybox shader)
- [x] Scene settings + editor UI ("Atmosphere" in scene Properties), serialized with the scene
- [x] `GameBackendCapture` with `--backend dx12` failed "renderer did not return the requested scene capture" when
  the window came out at 1520x873 instead of 1920x1080. Two bugs: (1) startup placement centred the benchmark
  window on the splash's monitor and shrank it to that monitor's work area (smaller or DPI-scaled monitor);
  benchmarks now skip `StartupSplash::PlaceWindow`. (2) DX12 mapped `rowPitch * height` of the readback, but the
  copy's last row is not pitch-padded, so widths not a multiple of 32 px (64 px for the RGBA8 back buffer copied now) overran the buffer and `Map` failed; it
  maps the footprint size now. `IGameRenderer::ReadSceneCaptureRGBA` returns the size actually captured (DX12 and
  Vulkan read the window size; Vulkan also recreates its readback buffer after a resize), and the benchmark writes
  the image at that size and says when it differs from the request. Checked DX12 at 1520x873 and 1920x1080, and an
  oversized 3840x2400 request (Windows caps the window at 2564 px wide here) on DX12/Vulkan/DX11. The capture tool's
  default scene (`oak_meadow.scene`, removed) is now `terrain/landscape_demo.scene`
- [x] Fog particles (CPU, GPU, fluid surface) and MMD outlines. Particles read a per-pass `particleFog` buffer
  (t7 / Vulkan binding 8: camera position + fog0..3, since particle constants already fill the 128-byte push
  constant); alpha particles blend to the in-scatter, additive ones fade by transmittance
  (`AtmosphereFogTransmittance`); the fluid surface fogs at its density-weighted particle center. Checked on DX12,
  DX11, Vulkan with a dense-fog copy of particle_showcase; fluid river runs on DX12/Vulkan. MMD outline fog
  compiles but was not seen in a capture
- [x] Fog the skybox seen through portals: `ViewLighting::SetSkyboxFog` now sets the sky fog terms for the main sky
  and portal skies alike (portal skies use the connected view's eye height). With fog forced on in
  portal_traversal_demo the sky inside the aperture matches the main sky exactly (213, 219, 226)
- [x] Tune defaults against the step 0 test shot: on the new vista the defaults (density 2e-4 at height 0, falloff
  0.004 = 250 m scale height, sky distance 30 km) give the layered look: haze pools on the far valley floor and
  around the ridge bases while the peaks and the foreground stay crisp. Thinner, deeper fog (8e-5..1.2e-4 with
  830..500 m scale heights) washed the whole range to one flat tone. The landscape scene never enabled fog; it
  does now, with those values written out (the generator carries `<settings>` over)

### Landscape look (large-scale vista with layered haze) / 2. Terrain scale + erosion (Tools/build_landscape_demo.py)

- [x] Grow world to 16–32 km with 800–2000 m relief (now 24.2 km, -56..1888 m; archive v3 stores its own scale, 1/16 m)
- [x] Coarser spacing outside the play area (16 m samples, upsampled on read); 4 m within 3 km of the start
- [x] Verify far-terrain cache/LOD in-engine at 24 km: confirmed faceted. Far tiles take vertices only at 128 m cell
  corners (the archive is read just for seam cells), so distant peaks were a few flat-shaded triangles. The macro
  color map now carries a 16 m world-space normal map (same grid and mapping, texture slot 2,
  `terrainMacroParams.w`), and distant tiles are lit per pixel with it whenever a macro or long-range shadow map
  is bound: the ranges shade with eroded ridges, gullies and spurs. Same on DX12/DX11/Vulkan
- [x] Hydraulic erosion pass (particle-based) before writing `world.heights`
- [x] Export flow and deposition maps alongside heights (`world.flow`, `world.deposition`; format in the script docstring)

### Landscape look (large-scale vista with layered haze) / 2b. Terrain editor tools (done; needs an artist pass in the editor)

- [x] Realtime brushes: Erode (hydraulic), Weather (thermal), Noise, Terrace
- [x] Select tool: rectangle / ellipse / lasso / polygon with feather, draped highlight, Apply commits one undo step
- [x] Region operations: hydraulic + thermal erosion, smooth, noise, terrace, flatten (Ctrl+click picks height), raise/lower, paint layer

### Landscape look (large-scale vista with layered haze) / 3. Distant terrain color + forest coverage

- [x] Macro color map at 16 m (1513^2, the erosion grid) instead of the far palette's one color per 128 m cell
  corner. `LandscapeDriver` composes it on a worker from the near paint rules (`PaintSample`), slope, and the
  generator's `world.flow`/`world.deposition` (`TerrainHydrologyMap.h`, validated against the archive signature);
  `TerrainManager` uploads it as an sRGB mip chain and binds it in texture slot 5 (`ObjectGPUData::terrainMacroMap`,
  `terrainMacroParams`). Distant tiles take their albedo from it; loaded cells blend to it between
  `macroColorNear`/`macroColorFar` (256/640 m) so both sides of the loaded-cell border match. Alpha holds forest
  cover. Editor: TerrainManager "Terrain appearance" (Macro color map, start/full distance, tooltips) and
  LandscapeDriver "Macro color" group (map paths, lushness, flow threshold, scree, scree depth, gully rock,
  brightness, with tooltips via the new `Component::RegisterFieldTooltip`). Draw preparation polls the composer's
  settings every 100 ms, so edits rebuild it live in the editor and in play (0.8 s per build). Verified on DX12,
  DX11 and Vulkan: slopes gain gully and scree detail, mid-distance ground fades into the far colors
- [x] Use flow for lush valley floors / streams, deposition for scree, steep + high flow for bare rock (in the macro
  map, i.e. at distance; the near paint does not read the erosion maps yet)
- [x] Forest-density map baked with the macro map (macro alpha = canopy cover; the color takes the canopy tint), and
  distant tiles shade forest as a bumpy canopy: `ApplyDistantCanopy` (Object.hlsl) tilts the 16 m macro normal with
  world-space crown/clump/stand noise (14/42/130 m) and darkens its hollows, each octave fading out once a pixel spans
  half its period. TerrainManager "Distant canopy relief" (0..1, default 0.6, tooltip), packed as
  `terrainMacroParams.w = 1 + relief`. Subtle in practice: the canopy albedo is very dark, and within
  `farObjectRadius` (8 km) the far tree impostors cover it; with impostors off it changes 2.7% of the vista's
  pixels, all on the far forest. No specular on distant tiles, so no canopy roughness

### Landscape look (large-scale vista with layered haze) / 4. Lighting + post

- [x] GTAO (half-res, temporally filtered) for contact darkening under trees and in valleys:
  `Shaders/AmbientOcclusion/AmbientOcclusion.hlsl` (compute) after the main view's draws
  (`SceneRendererLighting::ComputeAmbientOcclusion`). New context call `CopySceneDepth` copies the scene depth into
  a buffer (DX12 `CopyTextureRegion` of the depth plane; Vulkan `vkCmdCopyImageToBuffer` while the scene pass is
  paused; DX11 a small compute copy from a now-typeless depth; recorded in command packets, gated by
  `SupportsSceneDepthCopy`). GTAO per half-res pixel: view-space position/normal from depth, 2 slices x 2 sides x
  6 steps (rotated per pixel and frame), cosine-weighted arc integration, then blended with last frame's AO
  reprojected through the camera stored in the output's header (depth-checked). Applied next frame through a new
  structured slot 47 (DX12 root SRV t47, Vulkan binding 48) to ambient + environment (diffuse and specular) only,
  never sunlight: object surfaces, foliage cards, grass, fast terrain and per-pixel distant tiles
  (`ScreenAmbientOcclusion` in Object.hlsl: reprojects, 4-tap depth-matched filter, 1 off screen / disoccluded).
  Scene `ambientOcclusion` (`enabled`, `radius` 1.5 m, `intensity`, `temporalBlend` 0.15); editor block. Landscape
  demo: on. No measurable frame cost on DX12 or Vulkan (vista and near view, 4090). Notes:
  - fxc compiled `continue`/`break` inside the unrolled GTAO loops into a GPU hang (TDR); the loops are now
    branch-free. One later DX12 run still froze GPU timings once and has not reproduced in 12 runs since; the
    debug layer is clean. `ENGINE_GPU_DEBUG=1` now prints DX12 debug-layer errors/warnings to stderr
  - Editor viewports (D3D11View / D3D12View / VulkanView) don't register their depth yet, so AO is game-view only
  - Dense foliage shows some half-res blotchiness; a depth-aware spatial filter pass would smooth it. 2026-11-19:
    the lookup in Object.hlsl is now a depth-matched 3x3 tent (was 4 taps), which removed the diagonal noise
    stripes on plants; a filter pass in the AO shader itself is still open
  - Vulkan's AO GPU timer includes the barrier wait for the opaque pass to drain (reads ~1.3 ms; frame time
    shows no cost)
- [x] Auto exposure (luminance histogram, eye adaptation) with editor min/max EV and speed: three small passes
  in each backend's post-process before compose (`Shaders/PostProcess/PostProcess.hlsl`
  `ExposureLuminancePS` / `ExposureHistogramPS` / `ExposureAdaptPS`, shared plumbing in `Graphics/PostProcess.*`).
  64x32 log-luminance map (centre-weighted metering) -> 64-bin histogram over -14..+10 EV -> 1x1 adapt pass that
  trims the histogram to [lowPercent, highPercent], clamps to [minEv, maxEv] and eases from the previous frame
  (two ping-pong RGBA32F targets, separate up/down speeds, snaps after resize or re-enable). Compose maps the
  adapted average to mid grey; `outputExposure` stays as compensation. GPU-only, no readback. Lit views only.
  Scene `autoExposure` (`enabled`, `minEv`, `maxEv`, `speedUp`, `speedDown`, `lowPercent`, `highPercent`);
  editor block under Output Tone Mapping. Landscape demo: on, compensation -0.4 (vista meters at -2.7 EV, about
  where the fixed exposure was). DX11/DX12/Vulkan meter identically. Note: fxc mis-compiled a nested,
  partially unrolled histogram loop (DX counted ~1/10 of the map); it's one flat `[loop]` now. Not separately
  GPU-timed (64 + 1 pixels reading a 16 KB map)
- [x] Long-range shadow: terrain sun-visibility map ray-marched over the far height grid on a worker thread when the sun moves (`TerrainManagerSunShadow.cpp`); near terrain multiplies it into the cascade shadow, distant tiles are lit per pixel with it. TerrainManager `longRangeShadows`, `longRangeShadowSoftness`
- [x] Bloom: half-res 13-tap downsample / tent upsample mip chain in each backend's compose pass (`Shaders/PostProcess/PostProcess.hlsl`, shared by DX11/DX12/Vulkan). Scene `bloomEnabled`, `bloomIntensity`
- [x] Color grading (warm highlights, cool shadows): analytic split-tone + contrast + saturation after tone mapping. Scene `colorGrading*`, `grading*`
- [x] Sun disc / glare: limb-darkened disc + Henyey-Greenstein glow in `Skybox.hlsl` toward the brightest directional light; bloom provides the glare. Scene `sunDisc*`, `sunGlowIntensity`

### Landscape look (large-scale vista with layered haze) / 5. Sky computed from the sun

- [x] Hillaire-2020-style sky: `Rendering/Atmosphere/PhysicalAtmosphere.*` (Rayleigh/Mie/ozone media, Bruneton
  transmittance LUT 256x64, Hillaire multiple-scattering LUT 32x32, planet geometry in double) and
  `PhysicalSky.*`, which renders the sky on a worker into a 512x256 RGBA32F world-space panorama (with mips)
  whenever the sun moves >0.2°, its illuminance >1%, the camera altitude >100 m or the parameters change.
  The panorama replaces the sky-view LUT: it feeds the existing skybox draw and EnvironmentMap unchanged, so
  sky, reflections and ambient all come from one image. CPU-side because the generic graphics context has no
  offscreen color targets; the GPU only samples. Scene `physicalSky` (`enabled` is the HDRI toggle; HDRI is
  used until the first panorama is ready), `groundHeight`, `rayleighScale`, `mieScale` (haze), `mieScaleHeight`,
  `mieAnisotropy`, `ozoneScale`, `groundAlbedo`, `skyIntensity`; editor "Physical Sky" block. The skybox's
  analytic sun glow is off over the physical panorama (the aureole is in it); the sun disc stays.
  Landscape demo: enabled, `mieScale` 4. Tests: `.local/tests/PhysicalAtmosphereTests.cpp`
- [x] Derive sun light color and ambient SH from the same atmosphere model: the strongest directional light is
  multiplied by the transmittance toward the sun at the camera altitude (`attenuateSun`), and the environment
  map / SH are prefiltered from the physical panorama on the same worker
  (`SceneRendererLighting::ApplyPhysicalAtmosphere`)
  - Found while doing this: the split-sum BRDF LUT had lost its `/ sampleCount` in the 11-09 refactor
    (`RenderUtilities.h` `BrdfIntegration::Integrate`), making specular IBL ~256x too bright. It went unseen
    because PNG HDRI skyboxes drop their CPU pixels after upload, so `EnvironmentMap::Acquire` returns null and
    those scenes get no environment lighting at all (still true for the HDRI path; separate issue). Fixed, with
    a LUT range check in PhysicalAtmosphereTests

### Landscape look (large-scale vista with layered haze) / 6. Generated landscape assets (Python, same conventions as Tools/generate_meadow_accents.py)

- [x] Conifer generator (`Tools/generate_conifers.py`): trunk + layered branch whorls with needle cards; ~4 species/size variants; LOD chain; mid/far quality
  Spruce_tall_01 (24 m), Spruce_young_01, Pine_scots_01, Fir_subalpine_01 under `Prefabs/Woodland/Conifers`. LOD0 sprays
  (3-14k tris) / LOD1 fewer larger sprays with card coverage matched to LOD0 (side coverage within ~5-20%) / LOD2
  whorl stars / view-baked impostor (6 views + top, 38 tris, `impostorBillboard`, same card layout as the oak pack).
  Shared procedural needle atlas (spruce, fir, pine, clump cells) and bark; species colour is muted linear vertex colour
  (inner/outer/tip, per-branch jitter). Instanced chain resolves to LOD0 -> LOD1 -> impostor. Shared helpers:
  `tools/woodland_assets.py` (native .mesh writer, numpy rasterizer/impostor baker, PNG I/O, prefab/material writers)
- [x] Boulder / cliff generator: noise-displaced, faceted meshes, ground pivot, LODs, triplanar rock material
  `tools/generate_boulders.py`: Boulder_01-03, Boulder_flat_01, Cliff_block_01/02 under `Prefabs/Woodland/Boulders`.
  Superellipsoid cut by random planes (smooth-min) + FBM, cliffs with vertical cuts and stepped strata; one radius
  function sampled on icospheres per LOD; normals split at 38 degree creases. New `Material::triplanarScale`
  (world-space 3-projection base/ORM/occlusion/normal with UDN blend, projections < 4% skipped; packed in
  `textureUvSets1.z`, a terrain-only lane) with the Rock030 set; meshes keep box UVs at the same scale
- [x] Far-forest clusters / impostor cards baked from conifer + oak meshes; hook into TerrainManagerFarObjects
  `tools/generate_forest_clusters.py` bakes 4 stands (2 oak from the pack's LOD2 meshes + material textures, 2 conifer)
  of 14 trees on 36 m. `TerrainManager::farClusters` (asset, match text, baked tree count) + `farClusterDistance` /
  `farClusterSize` / `farClusterMinimumTrees`: far blocks wholly beyond the distance bucket their trees on the grid and
  replace squares where one family is >= 70% with one cluster (centroid, scale sqrt(count / baked)). Landscape demo:
  2.2 km, 40 m, 5 (scene + `tools/build_landscape_demo.py`)
- [x] Check alpha-to-coverage / coverage-preserving alpha mips so distant canopies do not thin out or shimmer
  Real-time rendering is single-sample (MSAA only in DX11 video export), so A2C does not apply. Measured: PNG foliage
  gets runtime box-filtered mips, which at the 0.32 cutoff grow oak canopy/impostor coverage 14-60% by mip 4-7 (crowns
  bloat and pulse) and drop sapling impostors to 25-48%. `tools/cook_foliage_textures.py` cooks every Mask material's
  PNG base colour with `TextureCook --alpha-cutoff` (push-pull fill + per-mip coverage rescale, BC7) and repoints the
  19 materials; generated foliage is cooked the same way. `--check` prints the per-mip table
- [x] Cloud and valley-mist sprite textures (noise-based) + soft particle/card prefabs
  `tools/generate_sky_sprites.py`: Cloud_Puff_A/B, Mist_Wisp_A/B, Mist_Sheet (`Textures/Atmosphere`), prefabs
  ValleyMist / LowClouds (ParticleEmitter) and MistCard (Blend mesh) under `Prefabs/Woodland/Atmosphere`. Soft
  particles: `ParticleEmitter::softness` fades against the main view's depth copy (reused from AO, or copied when AO is
  off; `SceneRendererLighting::SceneDepthForPixelShaders`, particle t8), plus `nearFadeDistance`, `fadeInTime`,
  `sizeVariation`, `randomRotation`. Portal views draw hard. Verified on DX12/DX11/Vulkan
- [x] Prefabs for all of the above under `Assets/Prefabs/Woodland/`, placeable by TerrainObjectLayer
  Conifers/, Boulders/, Clusters/, Atmosphere/. Landscape demo uses them through its layers: `LandscapeDriver`
  grows conifers by altitude (`ConiferShare`: oaks below 350 m, mixed foothills, conifers from 710 m to the tree
  line; picked from existing draws so the lowland oak layout is unchanged) and puts the generated boulders in rock
  fields; far impostors and clusters follow automatically. `Scenes/terrain/woodland_assets_preview.scene` shows every
  asset at its LODs; captured on DX12, DX11 and Vulkan
  Demo update: cliff blocks join the forest layer on steep rock below the snow (<= 2 per cell, upright, sunk by the
  slope; 900 m reach). They are authored `cullAfterLastLod`, which `GetObjectLodProfile` now passes through and
  `FarPlacements` skips, so near-only props never become far objects (~30% of cells are steep). The scene gets an
  "Atmosphere" group (`build_landscape_demo.atmosphere_items`): LakeMist (new wide prefab) over all five lakes,
  ValleyMist + three mist cards on the first lake by the start, LowClouds on four summits in the vista
- [x] Editor: run generators from an editor tool and refresh the asset browser
  Tools > Asset generators lists `tools/generate_*.py` / `cook_*.py` (docstring as tooltip), plus "Run all woodland
  generators" (ordered queue). `AssetGeneratorRunner` runs one at a time without a console window, streams output to
  the Console, then reloads rewritten textures (`Texture::Invalidate`) and refreshes open scenes
  (`OnAssetContentsChanged`); the asset browser lists new files through its directory watch. Builds; not yet exercised
  in a running editor
- [x] Wider, less bright tree and foliage colours: `Material::foliageColorVariation` (masked foliage; world-space noise
  at ~9 m crown and ~70 m stand scale trades yellow-green against blue-green and +-~20% value; `textureUvSets1.w`) on
  oak leaves/impostors (0.7), conifers and clusters (0.55/0.7), identical across LODs. Oak leaf + impostor tint
  [0.86, 0.84, 0.77] (darker, less saturated); terrain meadow layer `grass.material` [0.24, 0.32, 0.12] ->
  [0.20, 0.26, 0.11]

### Status and Next sections as they stood before the 2026-11-20 cleanup

The measurements behind the plan's order (shadow view breakdown, item counts, the one-species run, the
distance gate that did nothing, radius 3) are in the "Next" text below.

#### Status (2026-11-19)

Landscape demo, DX12, 1920x1080, RTX 4090, RelWithDebInfo, uncapped; moving runs are 400 frames.

| Run | Median | p95 | Frames over 50 ms |
|---|---|---|---|
| Standing, forest floor (three runs) | 24.0-25.0 ms | 27-29 ms | 0 |
| Standing, forest edge / trail / vista (before the last two CPU changes) | 26.9 / 28.5 / 25.7 ms | 31 / 33 / 29 ms | 0 |
| Turning in place (`--look-yaw 6`, forest edge) | 24.7 ms | 38 ms | 3 |
| Walking (`--drive-forward 380`, forest floor) | 31.3 ms (28.5 on 2026-11-20) | 51 ms, p99 115 (42, p99 103) | 20 (11) |
| Walking and turning (`--drive-forward 380 --look-yaw 3`) | 35.2 ms | 59 ms (p99 115) | 34 |

- Standing still the frame is CPU-bound: update ~1.8 + prepare ~4.7 + draw recording ~16 ms (camera view ~11.5, shadow
  view ~3.9) run one after another on the scene thread while the render thread is idle ~89 % of the frame. GPU is
  9-11 ms. Trimming hot spots gave ~2 ms a round; 16.7 ms needs the recording split across threads.
- Moving adds hitches: walking spikes to 80-120 ms each time a terrain cell boundary is crossed, and raises the steady
  cost (update 1.8 → 4.6 ms, prepare 4.7 → 8.9 ms).
- Checked on DX12 only since the colour pass: trails, scans, TAA dither, the 3x3 AO lookup, CPU round 3 and the
  moving-camera fix. Look pass 1 was checked on the test meadow on DX11, DX12 and Vulkan.
- **Landscape captures taken between the cloud-shadow work (Foliage Phase 3) and 2026-11-20 had no direct
  sunlight** (cookie texture not found from the capture tool's working directory). Frame times are unaffected;
  every look judgement made from those landscape captures is suspect, including the Phase 1d colour pass.
- Committed through `28210b49` (2026-11-19 22:34). Uncommitted: CPU round 3, the moving-camera fix, the 3x3 AO lookup
  in Object.hlsl, the merge of the plan files and look pass 1.

#### Next, in order

Reference check at full resolution (2026-11-20 night, `.local/reference-images` against `h1_*` tiles), largest
gaps first: (a) meadow, ref 04: fine thin blades as continuous cover to the horizon; ours is one broad-bladed
clump in rows with soil between, ending at 26 m. (b) forest interior, ref 07: haze between trunks so they fade
pale with depth; ours shows shaded trunks as dark blue-grey posts with no haze. (c) vista, ref 08: layered
ridge haze and far forest that still reads as conifer silhouettes; ours is navy blobs on pale untextured
mountains and a flat green hedge at mid distance. These are items 1 (haze, far forest), 4 (grass) and 6 below.

1. Done 2026-11-20: viewpoint and the settings half of the look (look pass 1 under "Landscape demo expectations").
   Left over from it: exposure at forest edges, far-forest texture and haze.
2. **CPU pipeline to 16.7 ms while moving** (order changed 2026-11-20 night: this before the hitches, and both
   before the grass work, which adds draws). A walking frame is now ~27 ms of CPU, mostly serial: camera view
   record ~11, shadow view record ~4.7, scene prepare ~6, update ~3.4; GPU 9-11 ms fits already. Steps, in
   order (Foliage Phase 2, "Next CPU steps" and "CPU draw recording on the vista"):
   a. shadow view recorded on a worker while the camera view records (est. 4-5 ms);
   b. a dirty list for frame items, one retain per distinct mesh buffer, shadow scene preparation skipped when
      nothing changed (est. 3-5 ms together);
   c. camera view record: split across threads, or far forest and terrain tiles on GPU-driven draws so the CPU
      does not walk ~2,650 visible items for ~350 draws.
   Estimate, not measured: a and b bring walking to 18-20 ms; under 16.7 needs part of c. Measure with the
   walking and turning runs, interleaved, not only standing poses.
   **Measured before starting the split (2026-11-20, late): the item count is the larger lever.**
   - The shadow view's 4.7 ms has nothing to cache away: startup 0.3, culling 0.5, sorting 0.2, material loop
     1.0, texture footprints 0.3, instance tiles 0.5, record upload 0.9, the rest in small pieces. Recording
     it on a worker saves at most those 4.7 ms (less, since it would compete with the camera view for the
     same workers): walking ~25 → ~21-22 ms at best.
   - Shared between the two `Render` calls and needing a per-view split first: the mapped light buffer
     (both calls rewrite it), cluster buffers, `SelectRenderPassBuffers`, the instance pool and persistent
     batches, the material record cache, texture footprint requests, the camera occlusion job the shadow
     call starts for the camera call, scratch vectors, ~75 writes to the timing structs, and scene calls
     that assert the owner thread.
   - Of 7,834 render items on a walk, ~7,300 are per-cell prefab batches ("Shared prefab part": one object
     per prefab part per terrain cell): forest 2,920, props ~2,500, far objects ~1,800. Terrain cells 121,
     grass tiles 224. The GPU already culls their placements one by one; the CPU still prepares, culls,
     sorts, resolves materials and builds tiles per batch object, in both views.
   - Frame time against item count, same walk with `loadedRadius` 3 instead of 5 (9,755 → 6,285 mesh
     objects; far objects take over the outer ring): 25.2 → 18.1 and 19.7 ms. Update 3.3 → 2.1, scene
     prepare 5.5 → 3.6, shadow record 4.5 → 3.7, camera record 10.6 → 8.2; GPU unchanged (opaque 8.2 → 8.6).
     About 1.7 ms per 1,000 items. Extrapolated, not measured: one batch per prefab part per 2x2 cells
     instead of per cell (~7,300 → ~1,800 items) would take ~9 ms off, to about 16 ms.
   So the order: (c') fewer prefab batch objects first (chosen 2026-11-20); the per-view split after it, if
   still needed.
   **(c') measured further the same night; nothing merged yet.**
   - One tree species instead of all (temporary switch, same tree count: 275k → 278k instances submitted):
     mesh objects 9,361 → 5,119, the walk **22.8-25.1 → 15.5 ms**. Update 3.3 → 2.2, scene prepare 5.4 →
     3.3, shadow record 4.0 → 2.2, camera record 10.3 → 7.1. GPU fell too (opaque 9.2 → 6.4, shadow 3.0 →
     0.8), partly because the one species is a cheaper mesh, so the GPU part is not a clean comparison.
     About 2 ms of CPU per 1,000 batch objects: the count of batch objects that exist is the cost.
   - Switching off batch objects whose LOD band cannot draw at their distance (a gate on `Object::enabled`,
     conservative margins): visible items 2,804 → 1,576 with draws and instances unchanged, and **no
     change in frame time** (24.4 against 24.2 ms, three pairs). Removed. So disabled objects cost as much
     as enabled ones, and the camera view already skipped those bands cheaply; only fewer objects helps.
   - `loadedRadius` 3 instead of 5: every tree past ~310 m is an impostor already and far-object blocks
     draw those, so the outer two rings of full cells may buy nothing. Standing: MeadowSlope 23.5 → 17.2
     ms, Escarpment 22.8 → 21.4; walking 25.2 → 18.1 and 19.7 (above). Look: the one tile pair compared by
     eye (`q5_`/`q3_Escarpment_r0c0.png`) is the same; 3-7 % of pixels differ over the whole frames, not
     examined (wind and cloud shadow differ between runs too). **Not adopted yet**: Vista and ForestWall were not captured at radius 3 (failed runs), and props,
     terrain detail and colliders in rings 4-5 are not checked.
   Next for (c'): finish the radius-3 look check; then one batch per prefab part per 2x2 cells for what
   stays loaded, and larger far-object blocks (1,800 of the batch objects are far blocks).
   Looked at on 2026-11-20 night, not started: the shadow and camera views are two calls of one ~3,000-line
   `SceneRenderer::Render` that share per-frame members (instance view counters, object record capacity,
   visibility and timing state), so (a) needs that state split per view first. A walking profile after
   tonight's fixes shows no single large piece in recording: material draw loop ~2.6 ms, startup ~1.9,
   texture footprints ~1.5, occlusion build ~1.5, instance tiles ~1.3; the shadow view's 4.7 ms is mostly
   its own scene preparation (3.7).
3. **Moving-camera hitches** at cell crossings: mostly done 2026-11-20 night ("Static objects"): steady
   walking 31.0 → 27.0 ms, worst walking frame ~140 → ~80 ms, frames over 80 ms 6 → 0-2 a run. Left: a
   ~33 ms commit per crossing (a cell's grass field removing its tiles one by one) and ~10 frames of scene
   prepare +10 ms after it.
4. **Forest and meadow content**: the forest edge is closed from the ground up with sunk small and medium
   oaks (2026-11-20, captured; "Terrain and forest edge pass"). Left: more broadleaf silhouettes and height
   variety along the edge (scanned shrubs, Foliage Phase 7), grass reach and the fade band (Grass Phases 1,
   3, 6).
5. **Terrain**: rolling hills and the escarpment are in and captured; the face is covered by scanned rock.
   Left: a clearing so the escarpment is seen from the ground, the striped rock texture on sheer terrain,
   the cliff follow-ups of Foliage Phase 7.
6. **Variants from the earlier reference sets**: still water with reeds and lily pads, haze between trunks and
   layered ridge haze, streams (Landscape look 7; Foliage Phases 6 and 7).

### "Next" section as it stood before the 2026-11-21 review

Replaced by a plan led by structural changes (vegetation and ground cover on the GPU, a retained render
scene). Open items were carried over; the completed ones below are the record of the 2026-11-20/21 work.

#### Next, in order

Reference check at full resolution (2026-11-20, `.local/reference-images` against `h1_*` tiles), largest gaps
first: (a) meadow, ref 04: fine thin blades as continuous cover to the horizon; ours is one broad-bladed clump in
rows with soil between, ending at 26 m. (b) forest interior, ref 07: haze between trunks so they fade pale with
depth; ours shows shaded trunks as dark blue-grey posts. (c) vista, ref 08: layered ridge haze and far forest
that still reads as conifer silhouettes; ours is navy blobs on pale untextured mountains and a flat green hedge
at mid distance.

1. **Fewer prefab batch objects** (the measured lever for frame time). Done: 3 loaded rings instead of 5
   (`loadedRadius`, scene and `build_landscape_demo.py`), with cliff pieces carried by the far blocks out to
   their 900 m reach (`TerrainManagerFarObjects.cpp`, `GenerateCliffs` no longer skips far cells).
   - [x] **One batch set per 2x2 cells** (`TerrainManager::nearObjectBlockCells`, 2 in the scene and the
         builder; `UpdateNearObjects` in `TerrainManagerFarObjects.cpp`). A loaded cell's prefab layers keep
         their placements and colliders but make no batches (`TerrainObjectLayer::suppressPrefabRendering`);
         one runtime layer per block and layer name holds the merged placements, with the member layers'
         settings, and builds the batches. A far block gives a cell up only when its near blocks have
         finished with it in. Editor streaming stays per cell. `ENGINE_NEAR_OBJECT_BLOCKS=1` turns it off.
         Interleaved, 1080p: walking 18.2 and 18.7 → 16.8 and 16.7 ms, p99 57-66 → 43-46, frames over 50
         ms 5-8 → 2; standing Escarpment 17.8 and 17.3 → 16.2 and 15.9 ms. Mesh objects 6,311 → 5,314.
         Escarpment picture identical (0.01 % of pixels differ). Not checked: the editor, prefabs with
         colliders, removing a placement at runtime, 3x3 or 4x4 blocks.
   - [ ] Far blocks of 16x16 cells instead of 8x8 would quarter the ~1,800 far batch objects, but every run
         with `farObjectBlockCells` 16 failed (12 of 12 attempts, device removed or a crash). Not usable as is.
   - [ ] Radius 3 look, still open: the ground at the foot of a distant cliff is the blurrier far-terrain
         colour (`qf5.png` against `qf3c.png`, 560 m from the escarpment); one tree on a rim 450 m away has
         a different silhouette as a far placement than as a near one (`qc5.png`, `qc3.png`); props, terrain
         detail and colliders in rings 4-5 are gone by design and were not looked at from the air.
2. **CPU pipeline**, if 1 does not reach 16.7 ms: the shadow view recorded on a worker (at most ~4.7 ms, and
   needs the per-view state in `SceneRenderer::Render` split first: light and cluster buffers, pass buffers,
   instance pool and batches, material cache, footprints, timing structs); a dirty list for frame items; one
   retain per distinct mesh buffer (Foliage Phase 2, "Next CPU steps").
   - Measured 2026-11-21 (interleaved pairs, FieldWalk and forest walks): recording the shadow view takes
     3.1-3.9 ms of CPU and does not follow the shadow ranges (trees 90 → 60 m, props 80 → 53 m,
     undergrowth 45 → 30 m changed it by less than the run-to-run spread, GPU shadow 3.0 → 2.8-3.1 ms in
     the forest). It follows the number of batch objects, like the camera view, so the worker is the lever,
     not shorter ranges. `ENGINE_LANDSCAPE_SHADOW_SCALE=<factor>` stays for A/B runs.
3. **Pop-in** (reported 2026-11-21: "lots of easily seen pop-in ... especially foliage-grass types",
   "ferns appearing just meters ahead of the camera"). To see it: `--moving-captures 15` on a
   `--drive-forward` run writes `capture_a.ppm` and `capture_b.ppm`, 15 frames apart while moving.
   - [x] Ground cover built one tile per update per field (the setting says 8): ~50 tiles were always
         waiting while moving (128 before 2026-11-20), so a quarter of the grass was missing and arrived
         late. Now several per update within the work budget (`GrassField::Refresh`): waiting tiles median
         50 → 0, active tiles 203 → 252, frame time unchanged.
   - [x] Ground cover ended ~16 m out: its radius followed the detail budget, which is pinned at its 0.6
         floor here (GPU geometry 12 ms against a 6 ms budget, CPU record 10.5 against 4). The landscape
         now keeps the authored radius (`GrassField::minimumRadiusScale` 1) and the reach is 50 m
         (`grassRadius`, +10 m fade), with 16 m tiles so the tile count falls (627 at 4 m and 26 m → 163).
   - [x] **16 m tiles thinned the grass underfoot** (a tile took its detail level from the distance to its
         own origin). Ground cover now picks the level per plant (`Mesh::perPlacementLod`, set by
         `GrassField::perPlantDetail`; geometry-fade mode 6 in Object.hlsl; cull bits 2|64 in
         InstanceCull.hlsl test the plant's root distance, the same value the pixel shader fades by). The
         0.35 distance push is gone. ForestEdge standing: grass GPU 4.6-5.2 → 4.2 ms, soil share 4.7 → 4.9 %;
         walks unchanged (CPU-bound). `ENGINE_LANDSCAPE_GRASS_PER_PLANT=0` goes back to per tile.
         Dead end on the way: the trees' bounds-padded band test drew twice the full-detail plants (6.5 ms).
   - [x] **Reach by plant size**: ferns, shrubs and bushes reach 80 m, fine grass and accents still end at
         60 m, inside one field (`Mesh::InstanceRange::reachScale`, `GrassField::fineReachFraction`; carried
         per draw in `DrawCBData::padding` and cull `selection.w` bits 16-25;
         `ENGINE_LANDSCAPE_SHRUB_REACH=<m>`, 0 = off). Cost while walking, interleaved pairs: forest
         16.6 → 16.8-17.0 ms, meadow 18.3-19.3 → 19.8-20.2 ms (tiles 165 → 245). 100 m: forest 17.2-17.4.
   - [x] `GrassField::LoadSources` rebuilt its species lists every frame for every field in range
         (1.2-1.6 ms of simulation time); now only when the field, its layer or the device changes.
         Simulation update 2.5 → 1.15 ms on the forest walk; the frame median did not move (16.4-16.6).
   - [x] The 80 m reach cost ~1 ms on the meadow walk: every tile was recorded in every species range at
         every level. A tile is now left out of a batch when that level's distances or that range's reach
         miss its bounds (`coverSkip` in `SceneRendering.cpp`, the same tests the GPU cull makes per plant;
         `ENGINE_COVER_TILE_SKIP=0` turns it off). Placement candidates -30 %. Interleaved walks: forest
         16.9-17.3 → 16.6-16.8 ms, meadow 19.5-19.6 → 18.2-18.7 ms.
   - [x] Shrub and fern counts were capped at 128 per tile whatever its size, so 16 m tiles carried half
         the forest-floor ferns. The cap now scales with tile area (bushes too). Costs ~0.4 ms on both
         walks: forest 17.1-17.2 ms, meadow 18.6-19.1 ms (not interleaved; same build session).
   - [ ] The detail budget never leaves its floor, so tree and prop detail distances are at 0.64 of
         authored (LOD0 to ~38 m, impostors from ~140 m) and switch close to the camera. Pinned at 1.0
         the forest walk costs 20.1 ms against 16.8. The budgets (6 ms GPU, 4 ms CPU) predate the current
         frame split and need setting against a 16.7 ms frame.
   - [ ] Not looked at yet: prop and tree level switches as such, cell hand-off between far and near blocks
         while moving, the meadow's far grass ring against the new reach.
4. **Load time** (reported 2026-11-20: "initial load times heavy"): ~29 s until the far forest had settled,
   ~15 s now.
   - [x] Half the cores work on the far backlog while it is large (was 4 jobs; `TerrainManagerFarObjects.cpp`,
         `ENGINE_FAR_OBJECT_JOBS=<n>` fixes the count): ready after 28.1-29.0 s → 14.5-15.0 s, two pairs;
         walking unchanged. Larger batches per job (32 cells instead of 8) gave 14.8 → 14.3 s, inside the
         spread; not kept.
   - [ ] Save far placements to the cache directory so later launches skip the generation.
   - [ ] Measure when the near world is playable, separately from the far fill.
5. **Remaining stutter at cell crossings**: a ~25-35 ms commit per crossing (a cell's grass field removing
   its tiles one by one) and ~10 frames of scene prepare +10 ms after it.
6. **Forest and meadow content**: more broadleaf silhouettes and height variety along the edge (scanned shrubs,
   Foliage Phase 7), grass reach and the fade band (Grass Phases 1, 3, 6). Adds draws: measure with the walks.
   - [ ] **Fields of tall grass** (asked 2026-11-21). References:
         `.local/reference-images/chlorophyll-video` (`02_meadow_flowers_ridge_haze_0m12`,
         `04_meadow_lone_tree_cliff_escarpment`, `01_forest_edge_meadow_slope_0m04`; full-size PNGs in
         `png/`). What they show against ours: knee-to-waist-high grass as one continuous mass with no
         soil between plants; fine thin blades that arch and lean with the wind, not broad-bladed
         clumps; seed heads and stems standing above the mass and breaking the skyline at the slope's
         crest; small white and yellow flowers scattered through it; darker in the depth of the mass and
         lighter at the tips; the cover carries on to the far meadow as a smooth lighter green.
         - [ ] Tall-grass species: thin-blade tussock and seed-stem meshes with three levels (the meadow
               preset's short/tall slots, `MeadowGrass.cpp`), height ~0.6-1.1 m with variation per plant.
         - [ ] Fields, not an even pelt: a field mask (noise in terrain space, as the shrub thickets use)
               choosing tall grass or the short sward, so fields have edges and paths can cut them.
         - [ ] Density that closes over the ground at 1080p from eye height, within the grass budget:
               measure GPU grass time standing at MeadowSlope and the meadow walk (18.6-19.1 ms now).
         - [ ] Root-to-tip shading and wind lean so the mass reads as depth (Grass Phases 3 and 6).
         - [ ] The far grass ring matches the tall field's colour and silhouette at the 60 m hand-off.
         - [ ] Compare unscaled 1440p tiles (`hires.sh`) against the references at MeadowSlope and
               ForestEdge.
7. **Terrain**: a clearing so the escarpment is seen from the ground, the striped rock texture on sheer
   terrain, the cliff follow-ups of Foliage Phase 7.
8. **Variants from the earlier reference sets**: still water with reeds and lily pads, haze between trunks and
   layered ridge haze, streams (Landscape look 7; Foliage Phases 6 and 7).

## Moved from todo.md on 2026-11-22

### Next / Look and content / Cliffs as terrain stamps (as it stood on 2026-11-22; replaced by sculpted cells, see todo.md "Cliffs as sculpted terrain")

- [ ] **Cliffs as terrain stamps** (asked 2026-11-21: "cliffs don't seamlessly connect with terrain and aren't
      fully connected all the way around"; the user's earlier idea of cliff brushes and adjustable terrain
      detail). Today `GenerateCliffs` (`LandscapeDriver.cpp`) stands photo-scanned rock faces, which are open
      shells, upright on steep cells at runtime, sunk a little and facing downhill. The ground is not changed,
      so it falls away under and beside the pieces (gaps, hollow edges) and keeps its own smooth slope behind
      them (a rock lying on a hill).
      Constraints found: terrain detail levels only coarsen the stored 4 m grid (every point near the camera,
      every second from `lod2Radius` 2, every fourth from `lod4Radius` 3, then far tiles), so nothing near the
      camera is finer than 4 m; the whole map at 1 m would be ~16x the height data (~2.3 GB) and triangles. The
      engine compiles vertex, pixel and compute shaders only (`IShader.h`, no tessellation stages on any
      backend), so finer terrain means mesh patches built on the CPU, which colliders need anyway. Stamps change
      heights that feed colliders, far tiles and cell caches, so they are applied when the map is built, not at
      runtime. Reusable: `TerrainRegionEdit` (cross-cell edits that cannot open a seam), `paintResolution`
      (paint finer than the vertices), `TerrainFarLodEdges.h` (level edges keep the shared boundary vertices).
      - [x] **Height-map stamps tried 2026-11-21, reverted.** Built: `tools/bake_terrain_stamp.py` (scan →
            0.5 m height patches), `tools/landscape_cliffs.py` (the runtime placement rules ported to the
            builder, 9,104 pieces in 219 cells, and the stamping), `world.cliffs` read by `LandscapeDriver`
            (`LoadCliffList`; inert without the file). Raising 4 m samples to under each scan left single
            samples tens of metres above their neighbours: pyramid spikes in front of the faces (captures
            `.local/terrain-stamps/ab_*.png`). The map is restored and `STAMP_CLIFFS` is off in
            `build_landscape_demo.py`. User feedback the same day: no cliff objects at all, terrain with
            the scans' shape and materials; and cliffs overlap vertically (overhangs), so a height map cannot
            hold them: "needing more than a height map but a 3d point map/mesh".
      - Revised design: **terrain patches displaced in 3D** (a vector displacement map per stamp). Each
        stamp is a regular grid of 3D points shrink-wrapped onto the scan, projected along the face's
        outward direction instead of straight down, so overhangs fit. Under a stamp the terrain builds a
        finer patch whose vertices move by those 3D offsets; its border vertices are the normal grid's, so
        it is seamless by construction and keeps the grid's detail levels, paint and collider path. Limit:
        one continuous sheet, so no caves or arches, and deep overhangs stretch triangles. Full 3D terrain
        (a density volume meshed with marching cubes or dual contouring) has no such limit but is a second
        terrain system; only if the sheet falls short.
      - [x] Stage 1: bake (`tools/bake_terrain_stamp.py`, `Assets/TerrainData/Stamps/*.stamp`): per scan a
            0.25 m grid of 3D points along the face's mean normal, with normals, the scan's UVs and a weight
            that rises over 1 m from the outline. Surface lost behind the outer layer 0-5 % of points, folds
            0-1.7 % of neighbour pairs (Cliff_Scan_01's stacked boulders are the worst: curtains between
            blocks). Checks: `.local/terrain-stamps/*_check.png`.
      - [x] Stage 2: placement at build time (`tools/landscape_cliffs.py`, the runtime rules ported; 12,975
            pieces in 300 cells, `world.cliffs`; heights unchanged). Matches runtime placement (0.0-0.1 %
            of pixels differ). No bulge in the heights: the patch's weight-0 border lies on the ground.
      - [ ] Stage 3: patches as part of the cell's terrain mesh (2026-11-21, works, too costly yet).
            `TerrainStamp` (loader), `Terrain::stamps` + `AppendStampGeometry` at every mesh install (rebuild,
            baked levels, prepared hand-off, cache load; the cache holds the grid only),
            `TerrainManager::SetTerrainStamps` (set before build or cache load), `LandscapeDriver` supplies
            them and keeps scan pieces only in far blocks. Points ~1 m apart on the placed shape at full
            detail. `OrientTerrainSurfaceUp` now keeps down-facing triangles whose winding matches their
            normals (overhangs). Terrain collision covers the grid only (`Mesh::collisionIndexCount`;
            colliding with patches cost 24 ms of physics a frame).
            Result: rock closes onto the ground, no gaps, spikes or hollow edges (`st_*.png`). Cost, 2560x1440,
            scans → stamps: pieceA 15.5 → 15.4 ms, pieceC 18.4 → 22.4, Escarpment 21.7 → 26.7, Cliffs 15.7 →
            40.2, old Cliffs 12.6 → 44.0. Open:
            - [x] Dense cliff areas overran the terrain memory budget (264-byte vertices, ~24,000 patch
                  vertices per cliff cell): cells unloaded and reloaded every frame (20-30 of 49 loaded).
                  Fixed by simplifying the patches at bake time (`tools/stamp_simplify.py`: quadric half-edge
                  collapses, so kept points stay original stamp points; the outline only slides along
                  itself; levels at 0.2 / 0.4 / 0.8 m stamp-space error for the cell's full, half and
                  quarter detail; stamp file version 2). Largest stamp 2,347 → 463 / 369 / 331 points. All
                  49 cells load; 2560x1440, scans / unsimplified / simplified: Cliffs 15.7 / 40.2 / 20.4 ms,
                  old Cliffs 12.6 / 44.0 / 17.1, Escarpment 21.7 / 26.7 / 21.7, pieceC 18.4 / 22.4 / 18.8,
                  pieceA 15.5 / 15.4 / 17.6. Camera recording fell with the vertex count (Cliffs 15.9 → 8.6
                  ms). The rock is more angular; fine relief is for the normal map (stage 4).
                  Not used: the compact 48-byte terrain vertex with a paint map for the weights
                  (`TerrainManager::compactCellVertices`, `Terrain::compactMesh`, off): cells install and
                  draw but produce no pixels; cause not found (checked vertex shader, buffers, bounds,
                  occlusion, pipeline layout, clips). Worth finishing for all terrain (5.5x less vertex
                  memory).
            - [ ] Camera recording is still above the scans' in every stamp view (Cliffs 4.9 → 8.6 ms,
                  Escarpment 7.7 → 11.5); not explained.
            - [ ] GPU terrain 1 → 3-5 ms. Measured 2026-11-21: one side projection and one layer on steep
                  faces saves only ~0.4 ms (pieceC 4.7 → 4.3, Cliffs 3.6 → 3.3), so sampling is not the main
                  cost; the rest is pixels: rock covers more screen than the scans' cheap material did, and
                  overlapping pieces (a third along each band) plus the grid behind them are shaded over and
                  over with no depth pre-pass. The fix is depth before shading for terrain ("Next" 7).
            - [x] Vertical texture streaks on stamped rock (user report 2026-11-21: "lot of vertical lines in the
                  stamped cliffs"): the side projection was chosen by the smoothed normal and its raw y, so
                  downward faces and facets leaning away from their normals smeared the texture. Object.hlsl
                  now follows the facet where it departs more than ~20 degrees from the normal and uses |y|;
                  height-field terrain keeps the smooth selection. Cost unchanged (terrain 3.6 → 3.3 ms at
                  Cliffs).
            - [x] Bright vertical slivers: fold faces where a stamp stepped from an outer layer of rock to an
                  inner one (the single-sheet limit). The bake now eases steps (`ease_steps` in
                  `tools/bake_terrain_stamp.py`): inside the shape, points behind a jump are pulled out to a
                  slope of at most 1:1 across the grid; in the 1 m band along the outline, points in front of a
                  jump are pulled back instead (pulling them out stood thin points above the crest as needles,
                  since the stamp direction leans up). Neighbour jumps over 0.5 m: 0.5-6.5 % → 0-3 %. The
                  smoother surface also simplifies further (Mountainside finest level 375 → 234 points).
                  Trade-off: recesses between stacked boulders (Cliff_Scan_01) become ramps.
            - [x] Patch borders blend into the terrain behind the stamp (a march along the stamp's inward
                  direction, up to 40 m) instead of the ground straight below, which under a tall piece's top
                  edge is far down.
            - [x] The terrain conforms to its cliffs (user, 2026-11-21: "seeing it load in normally then cliff
                  placed on top rather than smoothly transitioning from ground to cliff"). Most stamps are
                  near-vertical walls, so there is no mass under them to fill; their top edge stood above the
                  hill behind as a fin. `conform_heights` (`tools/landscape_cliffs.py`, run by the builder after
                  placement) extrudes each wall's top edge back into the hill: the ground rises to the top less
                  1.5 m, holds 6 m, then falls back at 45 degrees; a low apron (up to 3 m, 12 % of the wall)
                  fades over 8 m at the foot; nothing is raised above a rock surface over it or lowered; ground
                  under rock is painted rock, aprons scree. 58,663 samples raised (mean 4.4 m, max 40.6 m),
                  sculpt edits untouched. 2560x1440: pieceA 15.3 ms, Cliffs 15.4, farA 19.4, farB 21.3, 450 m
                  out 17.2. Not checked yet: a moving capture across the far/near hand-off (the drive went into
                  a canopy). Far cells still draw scan pieces on the far tiles (which now carry the shoulder),
                  so a shape change at ~400 m remains until far cells get stamps (stage 5).
            - Status 2026-11-21 after easing (2560x1440, scans → stamps): Cliffs 15.7 → 15.0 ms, old Cliffs 12.6 →
              13.8, pieceC 18.4 → 16.5, pieceA 15.5 → 16.6; from 120 m (new poses farA, farB in the scratch
              pose list): farA 19.1 → 18.5, farB 26.9 → 20.8. From 120 m the stamps read as dark rounded rock
              set into the hillside, without the scans' layered colour and relief: stage 4.
            - [ ] Far cells still draw scan pieces; patches there need the cell's far levels.
      - [x] Stage 4: the scans' own surfaces on the stamps (2026-11-21). The scans' textures are UV atlases
            (many islands), so patch triangles cannot sample them through per-point UVs; instead
            `tools/stamp_textures.py` (run by the bake; needs Pillow) renders each scan straight on along
            its stamp direction into 1024 px images over the stamp grid: colour, roughness, and the scan's
            normal (mesh normal bent by its normal map) in the stamp's own axes, plus a `.material`
            (`Assets/TerrainData/Stamps/*`, ~4 MB). The terrain packs them as extra layer-array slices after
            the six paint layers (`TerrainLayerArrays::MaxSlices` 16, `Terrain::stampMaterials`,
            `TerrainManager::SetTerrainStampMaterials`, one shared set for all cells); patch vertices carry
            their stamp texcoord, slice and weight (uvChannels4) and the stamp axes (tangent,
            uvChannels0.zw/1.z); Object.hlsl blends colour, normal, roughness and occlusion over the terrain
            by the weight, so the outline still meets the paint. Bug found on the way (user: "cliffs had grass
            material"): on the cell-cache load path the patches were built before the cell's layers were
            read, so they pointed at the paint layers' slices. 2560x1440: pieceA 14.5 ms (scans 15.5),
            Cliffs 15.2, farA 18.4, farB 20.8, 450 m 17.2.
            - [x] The stamps read 2-3x darker than the scans when lit from the front (rock-to-meadow
                  luminance 0.22 against 0.58 at farB; back-lit they matched). Not the long-range height-grid
                  shadow (skipped for stamp surfaces now anyway: the grid does not know them), not the normals
                  (checked by drawing them), not occlusion. Two causes fixed: the bake multiplied the scan
                  material's tint into sRGB values (the engine applies it in linear light, so red came out at
                  0.52 instead of 0.74), and stamp surfaces went through the terrain's cheap lighting pass
                  (flat ambient, Lambert sun); they now take the full lighting the scans had. Front-lit 0.22 →
                  0.37 (scans 0.58), back-lit 1.75 → 1.91 (scans 1.64). The rest of the gap is not explained.
      - [x] Stage 5: no scan pieces anywhere (2026-11-21). Far blocks (and cells outside the play area,
            which keep runtime placement) place stamp proxies instead: `write_proxy` in
            `tools/bake_terrain_stamp.py` writes each stamp's three simplified levels as an ordinary mesh
            (`Assets/Models/Woodland/Stamps`, prefabs in `Assets/Prefabs/Woodland/Stamps`) with the stamp's
            baked material; vertex normal = stamp direction and tangent = its u axis, so the normal image
            (in stamp axes) shades it. Same placements and transforms as the terrain stamps, so a cell's
            cliff keeps its shape and surface when it comes into range (`CliffPrefab` in
            `LandscapeDriver.cpp`). The photo scans are bake sources only. Checked from 650 and 800 m up:
            proxies stand where the scans stood; frame times within noise. Not checked: a moving run across
            the hand-off at ~450 m.
            - [ ] Moving check 2026-11-21 (`--capture-every 15` on a drive from 720 m, new capture option):
                  the hand-off at ~450 m still jumps. Before it the escarpment face is the far tiles' pale
                  macro colour with a few scattered proxies; after it a continuous dark rock band (stamps on
                  rock-painted terrain). Two causes: the far blocks hold only some of the cliff pieces (as they
                  did with the scans), and the macro colour map paints the face pale rock, not the stamps'
                  colour.
                  - Findings 2026-11-21: the far blocks did hold every piece. With far terrain on, streaming
                    stops at `loadedRadius` (`TerrainManagerEditing.cpp`: 49 cells, all stride 1); everything
                    past 384 m is far tiles, one quad per 128 m cell. That turned each escarpment into a smooth
                    slope standing in front of its proxies, and it is why cliffs look flat at a distance.
                    (Disabling a far tile's object does not stop it drawing, so a "hide far tiles" test
                    proved nothing.)
                  - [x] Macro maps carry the stamps: stamp materials have `terrainMacroColor` (linear mean
                        albedo, `tools/stamp_textures.py`); `ComputeMacroColors` paints each listed piece's
                        stamp colour and wall facing into the 16 m colour and normal maps (no more pale
                        sun-facing slope).
                  - [x] Far-grid patches: every cliff cell's full-resolution (conformed) heights go into
                        `world.fargrid` (`write_archive`), so far tiles keep the escarpment's height and shape
                        (`.local/terrain-stamps/far_patch2.png`). Up to 300 x 2,048 triangles; cost not timed
                        (single static runs too noisy).
                  - [x] No proxies in listed cells (`GenerateCliffs`); the edge fade to the macro colour skips
                        stamp pixels (Object.hlsl).
                  - [x] Far stamps (`TerrainManager::farStampRadius`, default 8 cells; `ENGINE_FAR_STAMPS=<n>`
                        overrides for A/B, 0 = off; `TerrainManagerFarStamps.cpp`): each stamped cell the far
                        terrain draws gets its stamps as one runtime mesh (`Terrain::BuildStampSurface`,
                        coarsest stamp level, built on a worker from the procedural builder), shaded as terrain
                        with the loaded cells' layer arrays (`TerrainBatchAppearance`), hidden while the cell is
                        loaded. Static check either side of the 384 m border at 1440p: the same rock on both
                        sides (`.local/terrain-stamps/handoff_static.png`); at 450 m the escarpment is rock, not a
                        smooth brown wall (`far_stamps1.png`). Cost at the 450 m view, 1920x1080, interleaved
                        A/B x2: frame 16.7 → 17.5 ms (terrain GPU 1.0 → 1.65 ms, shadows +0.1, other opaque
                        +0.2); memory +150-250 MB. A coarser stamp level does not help (tolerance 0.8 → 3 m: 488
                        → 463 triangles; the outline dominates).
                  - [ ] Far stamp cost: they take the full lighting path (as near stamps do) and draw over
                        the far tile; candidates are cheaper lighting past ~600 m, or cutting the far patch
                        under the stamps.
                  - [ ] A moving check of the hand-off once drives are okayed again.
            - [ ] Sculpted cells, replacing stamps (user 2026-11-21: "it looks like you placed the old cliffs
                  there instead of morphing the terrain"). Seen from above the stamps were overlapping shells
                  (36 pieces within 40 m at the test escarpment), cutting through each other with gaps, their
                  textures smeared wherever the rock turned away from each piece's bake direction
                  (`.local/terrain-stamps/top_cliff.png`). Prototype on cell 0,-6 (`tools/cliff_sculpt.py`):
                  a 0.5 m grid on 6 m-smoothed ground, each point marched along its normal to the outermost
                  surface of ground and scans (KD-tree over points sampled on the scans), simplified to three
                  levels with the 4 m border locked (`stamp_simplify.simplify_levels(locked=)`), rock images
                  baked from the scans. Engine: `TerrainSculpt` (`.sculpt`), carried as a stamp instance; the
                  cell's surface replaces its grid triangles (`Terrain::AppendStampGeometry`), shaded through
                  the stamp path with world-space normals. First result: one connected surface, no shards or
                  gaps, but texture streaks on walls from ground-plan texcoords; texcoords spread by surface
                  distance only turned them into swirls (each texel took its nearest scan point, which jumps
                  about on a surface that is not quite the scans'). Option 2 (user's choice): no per-cell bake;
                  a seamless rock tile cut from the Mountainside_01 stamp's face-on images where it is flattest
                  (`tools/cliff_rock_texture.py`, 13.8 m repeat; the scans' UV islands are at most ~420 px),
                  projected from three sides in Object.hlsl (whiteout normal blend), tinted per vertex by the
                  scans' blurred colour; grid 0.25 m. `.sculpt` v2: tint instead of texcoord, tiling in the
                  header.
                  - [x] Whole escarpment: cells -1..1,-6 (`python tools/cliff_sculpt.py -1,-6 0,-6 1,-6 --only X,Z`
                        runs them in parallel, ~10 min each). Pieces sampled with a per-piece seed and every
                        point locked on borders shared by sculpted cells: borders identical at all three
                        levels (513 points, height difference 0). Top and side views against the stamps:
                        `.local/terrain-stamps/esc_top_vs.png`, `esc_side_vs.png`; borders `seams.png`.
                  - [x] Cost: the CPU occlusion buffer rasterized the sculpts' full meshes (+190k triangles,
                        frame +1.2 ms); `Mesh::occluderIndices` now gives it the coarsest level. Interleaved
                        A/B (`ENGINE_SCULPTS=0` keeps the stamps), escarpment side view 1920x1080: frame 18.1 /
                        17.1 ms against 18.5 / 19.5 ms with stamps; terrain GPU 2.1 against 3.0-3.3 ms.
                  - [ ] Shared borders still turn the march directions vertical (the 8 m `EDGE` band), which
                        streaks the rock along them; only borders with grid cells need it.
                  - [ ] From the side the sculpt is one even wall: the outermost surface hides the gaps and
                        shadows between pieces that gave the stamps their blocky look.
                  - [ ] Fine folds (marched rays crossing) show as small bright slivers.
                  - [ ] Moving drives (`cl_drive_v2`) froze three times around frame 410 with process memory
                        climbing past 18 GB (GPU hang suspected); static captures are fine. No drives until the
                        user okays them.
      - Order: after "Next" 1-2, as before; stage 1 can start any time.

### To investigate

- [x] Detail budget sits at its 0.6 floor on every landscape view. Answered 2026-11-21: it measures geometry
      at 14-15 ms against a 6 ms budget and CPU recording at ~10.6 against 4, and its whole range (scale 0.6-1)
      changes predicted cost by under 10 %, so nothing it controls can reach the budget. "Next" 6 replaces it
      as the level policy.

### Foliage and world realism / Foliage Phase 7 (superseded 2026-11-22 by "Cliffs as sculpted terrain")

- [ ] Cliff follow-ups: a proper wide viewpoint (the capture pose ended up behind a piece; the existing
      "Cliffs" viewpoint is too close under the slope); 2k textures stretched 4x are soft up close (use 4k
      or add a triplanar detail layer); colour match to terrain rock is a tint only; bare steep slopes
      where rock paint is under the threshold; pieces per cell (40) and their LOD0 cost not profiled.
- [ ] **Not yet a continuous cliff line** (2026-11-21: the plan for this is "Cliffs as terrain stamps" under
      "Next"; user: "repetitive flat/steep terrain, nothing like the rocky edge of a
      mountain"): the generated pieces read as separate rounded lumps (one radial shape each), the strata
      pattern looks like honeycomb, and slopes whose rock paint is below the threshold stay bare. Generated
      shapes have hit their limit, and the scanned rock faces (above) read as real rock but still as an
      outcrop. Next: ledges/terraces carved into the height archive (`build_landscape_demo.py`) so the
      scans sit on a real break in the slope, and rock on the bare steep slopes.

### Grass / Grass Phase 6½: Antialiasing

- [x] **MSAA** (`AntiAliasingSettings::msaaSamples` 1/2/4/8, Preferences > Effects > MSAA; default off).
      Renderers lower an unsupported count to the nearest supported one.
  - **DX11:** game renderer follows the setting live (`ApplyLiveSampleCount`, video export keeps its
    own count); editor views (`D3D11View`) follow it too (they were fixed at 4x before, so they now
    default to 1x + FXAA). Multisampled depth is now readable: typeless + `Texture2DMS` SRV, and the
    depth copy has a `Texture2DMS` variant, so AO and the depth-aware sky/haze keep working.
  - **DX12:** `D3D12PipelineState` keeps its description and builds a variant per sample count on
    first use (`ForSamples`); `D3D12GraphicsContext::SetPipeline` picks the bound scene target's count
    (`DX12PostProcess::BoundSceneSamples`, set by BindScene/RebindScene, 1 after Compose; always 1 in
    depth-only passes). `DX12PostProcess` renders into a multisampled target and resolves at compose.
    Depth is multisampled; `CopySceneDepth` copies sample 0 with its own compute root signature/PSO
    (`CopyTextureRegion` cannot read MSAA depth), then restores the shared texture descriptor heap.
    Game renderer flushes and recreates on a change; editor views (`D3D12View`) retire old targets
    for 4 renders instead (frames in flight).
  - [ ] **Vulkan:** not yet (renders without MSAA).
  - Checked on grass_test.scene: DX11 and DX12 4x give clean blade edges; AO runs under MSAA on both;
    no DX12 device errors. With auto exposure on, DX12 MSAA captures at 120 warm-up frames looked
    darker: a start-up adaptation transient (converges to the same level by 600 frames; identical
    with `ENGINE_AUTO_EXPOSURE=0`).
  - [ ] Not yet exercised: live MSAA switching in the editor's DX12/DX11 views (captures use the game
    renderer).
  - Diagnostics for captures: `ENGINE_MSAA`, `ENGINE_FXAA`, `ENGINE_GRASS_MIN_WIDTH`,
    `ENGINE_AUTO_EXPOSURE` override the project settings in the game/capture tool.
- [x] **TAA** (`AntiAliasingSettings::taa`, `taaFeedback` 0.75–0.97, `taaSharpen` 0–1; Preferences >
      Effects; on by default since Foliage Phase 1b, with the Balanced preset (Performance: off, Quality:
      MSAA 4x); mutually exclusive with MSAA in the UI, MSAA wins if both are set).
  - **Jitter:** SceneRendering offsets the main view's projection by a Halton(2,3) 8-sample sub-pixel
    sequence, per camera (`SceneRenderer::m_temporalViews`), only when the backend reports
    `IGraphicsContextFactory::SupportsTemporalAntiAliasing()` (DX11, DX12), single-sample, perspective.
  - **Reprojection:** one matrix, previous unjittered view-projection × inverse(current), in double,
    carried in `PostProcessSettings` (so it also travels in the game's render packets). History is
    invalid after a resize or on a camera's first frame.
  - **Resolve** (`PostProcess.hlsl` `TemporalResolvePS`, before bloom/exposure): reprojects the
    nearest depth in the 3×3 neighbourhood, clips history to the neighbourhood's YCoCg spread (no
    per-object motion vectors, so this is what stops animated grass ghosting), blends on
    luminance-compressed colour, and lowers the history weight with motion. Result goes to one of two
    history targets and is copied into the scene colour, so bloom/exposure/compose are unchanged.
  - **Sharpening:** compose applies a 4-tap unsharp mask after TAA (`temporal.z`).
  - `PostProcessConstants` grew 64 → 80 bytes (`temporal` float4); all backends size it with
    `sizeof` (Vulkan push constants stay well under the 128-byte minimum).
  - DX11: history in `DX11PostProcess`; game renderer and editor views (`D3D11View`, depth now
    typeless + SRV) hand it their depth. DX12: two history targets/RTVs and two descriptor tables
    (scene, history, depth) in `DX12PostProcess`; depth goes DEPTH_WRITE → PIXEL_SHADER_RESOURCE
    around the pass.
  - Checked on grass_test.scene: static camera converges to clean edges on DX11 and DX12 (DX11
    high-frequency energy 14.4 → 8.3); a turning camera (`--look-yaw 6`) shows no ghost copies or
    trails, sky and clouds crisp, grass softer while moving (sharpening helps). Cost on the iGPU:
    ~+1 ms DX11, ~+0.05 ms DX12 at 960×540. No DX12 device errors.
  - [ ] Vulkan: not supported yet (no jitter is applied there).
  - Override for captures: `ENGINE_TAA=0|1`.

### Landscape look / 0. Baseline + test shot (CPU items)

  - [x] `SceneTextureFootprints::Request` projected every instance into the camera and all shadow cascades on one
    thread (3.8 ms). Now split into 512-instance chunks on RenderJobs: 1.0 ms. New capture fields
    `render_material_draw_loop_ms`, `render_material_footprint_ms`, `render_material_record_upload_ms`.
  - [x] Frame item preparation did two `dynamic_cast` component scans per object per frame (~3.8k objects); terrain
    components now come from `FrameComponentLookup`: `prepare_items_ms` 4.3 -> 3.3.
  - [x] Sampling profiler without admin rights: `GameBackendCapture --cpu-sample` samples every thread during the
    measured frames and writes `<out>/cpu-profile.txt` (per-thread inclusive/exclusive + caller chains;
    `.local/tests/CpuSampler.h`). Adds sampling overhead: use it to find hotspots, not for timings.
  - [x] Biggest finding: no shader cache. Object.hlsl entry points take ~21–23 s each in fxc (Water.hlsl PSMain 25 s),
    so every launch spent 60–100 s compiling and the compiles were still running on the job workers during
    measurement, starving the frame's parallel work. Added a persistent DX11/DX12 bytecode cache
    (`Renderers/D3DShaderCache`, key = preprocessed source incl. all includes + entry/target/flags/compiler version;
    `%TEMP%/EngineShaderCache`, `ENGINE_SHADER_CACHE_DIR`, `ENGINE_SHADER_CACHE=0`). Warm startup 65.6 s -> 1.8 s.
  - [x] Particle rendering scanned every object with `GetComponent<ParticleEmitter>` four times per frame (~15% of
    the simulation thread) in a scene without emitters; now uses the component registry.
  - [x] Remaining per-draw `GetComponent<Terrain/TerrainBatchAppearance>` in Render/PrepareTerrainMaterial use the
    frame item's `terrainSurface` flag.
  - [x] Standalone game publishes scene snapshots without per-object contents (only the editor reads them):
    simulation update 3.6 -> 2.3 ms.
  - [x] `Material::Validate` (0.75 ms, cache misses over ~40 fields of each large per-object Material) now runs as a
    parallel pre-pass over the frame lookups before the serial item loop: 0.15 ms. Prefab LOD materials still
    validate inline. Shader prewarm dedupe no longer builds a `tuple<string,string,bool>` per item (transparent set).
  - [x] Texture footprints re-project each 512-instance chunk every 4 frames (staggered, per view; last result is
    re-submitted in between) via `Rendering/TextureFootprintCache` owned by the Scene: 2.55 -> 1.5 ms, same
    residency. Remaining: the parallel join (~60%), serial gather, `RequestGraphicsFootprint` mutex per texture slot
  - [x] `TerrainLayerArrays::Acquire` built a string key (paths + formatted tints) per terrain draw per frame; now
    an identity fast path (texture pointers + tints, weak refs guard address reuse). `PrepareTerrainMaterial`
    looked up the parent TerrainManager twice. Material draw loop 1.3 -> 0.9 ms
  - [x] GrassField visibility loop: per-frame `std::set` of desired tiles replaced by a refresh serial, tile Material
    cached (was a component scan per tile): grass 0.94 -> 0.42 ms with ~1000 active tiles. Captures now carry
    `grass_*_ms` phases
  - [x] Hotspots listed here on 2026-11-16 (item preparation, TerrainObjectLayer::PreparePrefabInstances,
    Material::PrepareTextures, BuildOcclusionSignature, TransformHierarchy::Prepare, TerrainManager::
    UpdateFarObjects, heap allocation, `_NLG_Return2`) have each been cut: texture preparation and far objects
    by the items in this section, the rest in the foliage CPU rounds (Foliage Phase 2). The current list is
    "Next CPU steps" there

### Landscape look / 1. Aerial perspective + height fog

- [x] Shadowed in-scatter (light shafts): `ObjectFogSunVisibility` (Object.hlsl) marches the cascade shadow map
  along the view ray in quadratically spaced segments (finer near the camera); each segment's analytic fog opacity
  is weighted by one bilinear shadow tap, dithered per pixel with interleaved gradient noise (static: there is no
  temporal pass yet). Only the sun-glow share of the in-scatter is shadowed (`ApplyAtmosphereFogShadowed`); fog past
  the last cascade split is sunlit. The range uses the view-wide splits, so lighting bands do not cut shafts at
  object edges. Settings: Light Shafts / Samples (1-32, default 8) / Strength, packed into `fog0.w` (no new
  `ObjectGPUData` field). Opt-in: 8 samples cost ~2 ms GPU on the Vulkan vista (terrain 1.03 -> 1.97, leaves
  4.26 -> 5.19 ms). `ENGINE_LIGHT_SHAFTS=0|N` overrides for A/B captures. `AtmosphereFogSettingsTests` covers
  round trip, clamps and the `fog0.w` encoding. Checked: FXC ps_5_0 + DXC SPIR-V compile; on the vista the change
  only darkens (forest within shadow range), no seams

### Landscape look / 5. Sky computed from the sun

- [x] Replace step 1's analytic fog with the aerial-perspective volume behind the same shader include:
  `AtmosphereFog.hlsli` has a physical mode (fog0.w >= 100, packed by `ViewLighting::PackPhysicalAerialPerspective`)
  used by objects, particles and outlines with no call-site changes; light shafts keep working
  (`aerialPerspective` toggle, height fog otherwise). Deviation: it's analytic, not a froxel volume:
  per-channel Rayleigh + Mie extinction with their own exponential height profiles integrated along the
  ray, sun single scattering (Rayleigh + HG phases, sun illuminance at the camera) and Hillaire's
  multiple-scattering term, combined as color*T + S*(1-T)/tau. Exact for the lower atmosphere at
  terrain distances. A froxel volume needs 3D textures and a global texture slot, which the generic graphics
  context doesn't have yet
- [x] Volumetric clouds: `Shaders/Clouds/Clouds.hlsl`, a fullscreen pass blended (premultiplied) over the sky right
  after it, so terrain and objects drawn later occlude it. Flat layer (base 2600 m, 1400 m thick, above the
  1.9 km peaks), ray-marched per pixel (48 steps, jittered): 3D value-noise FBM shaped by a 2D weather/coverage
  noise, cumulus profile (flat bases, threshold rising with height for domed tops), Worley billows and value-noise
  edge erosion; 5-step light march toward the sun (Beer-Lambert with a multiple-scattering floor), two-lobe
  Henyey-Greenstein phase, sky light brighter toward the tops; fades out toward max distance into the hazed sky.
  Wind drifts the field. `SceneSettings::volumetricClouds` (coverage, density, base, thickness, shape scale, edge
  detail, wind speed/direction, sky light colour/intensity, sun intensity, steps, max distance), serialized, in
  scene Properties > Atmosphere with tooltips, read every frame (live in the editor). Constants: 128 bytes in an
  extra slot of the sky constant ring (`SceneRenderer::kCloudConstantSlot`), Vulkan SPIR-V built by CMake.
  Enabled in landscape_demo. Sky pass (sky + clouds) 1.0-1.3 ms GPU on the vista, same on DX12/DX11/Vulkan
  - [x] Cloud shadows on terrain and objects: a scrolling cloud-shadow cookie on the sun (Foliage Phase 3),
    statistically like the clouds
  - [x] Half-resolution march: `Clouds.hlsl` `CSMain` ray-marches one thread per half-res pixel into a structured
    buffer (`SceneRenderer::m_cloudSamples`, retired on resize) in a compute pass before the sky
    (`SceneRendererLighting::MarchVolumetricClouds`); `PSMain` upsamples it bilinearly (graphics slot 6, the lights
    are rebound after). The grid size and step count are bit-packed into `shape.w` to stay within 128 bytes of
    constants. New GPU stage `Clouds` (`gpu_clouds_ms`). Vista cost 1.33 ms -> 0.61 ms on DX12 (march 0.58 +
    sky/composite 0.03), DX11 0.46, Vulkan 0.82; image nearly identical (edges marginally softer). Vulkan's
    `StorageBarrier` now also covers fragment-shader reads (it only covered compute/vertex/indirect)

## Moved from todo.md on 2026-11-24

- [x] **Real-speed capture and flag button** (2026-11-23/24). `--capture-async` saves every frame without
      draining the GPU (DX12 readback ring, half resolution). `--flag-button` (or `ENGINE_FLICKER_BUTTON=1` in
      normal play) shows "Flag flicker (F8)"; F8 or a click writes `{"type":"flag","frame":N}` to frames.jsonl.
      `.local/transitions/flag_pops.py` finds the pops before each flag, `flag_review.py` compares with a
      reference run, `pairdiff.py` diffs two runs on the same route frame by frame.
- [x] **TAA edge strip on fast turns** (2026-11-24): the strip a turn reveals had no history and showed the
      raw jittered, dithered frame for ~10 frames. History age now rides in the history alpha (2 * age +
      alpha, decoded in the compose); new pixels average their first frames evenly and the first one is
      softened toward its 3x3 neighbourhood (`taaFastSettle`, Preferences "TAA Fast Settle",
      `ENGINE_TAA_FAST_SETTLE=0`). Strip/interior sharpness on fast turns 1.11 -> 1.03 (TAA off 1.01).
      `ENGINE_TAA_HISTORY_DEBUG=1` tints no-history pixels red. Ambient occlusion has the same restart: not done.
- [x] **Bushes vanished for ~20 frames and popped back while walking** (meadow, real speed; user flags 2026-11-24
      at frames 486, 2314-2333 and 3800-3820; the first real-speed capture's 1971-1986 was the same). Cause: a
      ground cover tile still fading in (GrassField tile appearance, `fadeDuration` 0.35 s) kept
      `geometryFade.w` = 2 + visibility ("tile level"), while settled per-plant tiles get 6 ("per placement",
      `perPlacementFade` in SceneRendering.cpp only switched fully visible tiles). The GPU cull takes the
      per-placement mode for a whole batch from its first tile (`representative`), so whenever a far tile
      fading in at the 80 m range edge led a batch, every tile in it was culled by tile-level bands for the
      fade, while the pixel shader still dissolved settled tiles per plant: plants whose own band differed
      from their tile's were drawn in no band, invisible until the far tile finished, then back at once.
      Timing-dependent (which tile leads the batch, wall-clock fade), only while moving (no tile builds when
      spinning in place). Fix: fading per-plant tiles stay per placement, their visibility carried as
      w = 6 + 0.4 (1 - visibility) (`GroundCoverVisibility` in Object.hlsl). Validated on a default
      real-speed meadow walk with every frame captured: the bush pops at 485, 1986 and 2306 that appeared in
      nearly every run are gone (largest step left 2.8 k pixels, the canopy passing at 3946-3954); the user
      watched a run with no bush flicker.
      How it was found: `ENGINE_TRACE_LAYER` stage counters and a checksum of the cull inputs were identical
      between runs with and without the vanish; uploads matched the CPU; occlusion, texture streaming, shader
      misses, frames in flight (`ENGINE_GPU_SYNC_EACH_FRAME=1`), the LOD cross-fade, pool identity steal and
      tile pooling were ruled out; the visibility monitor (`ENGINE_VISIBILITY_MONITOR`, CPU reference cull vs
      the GPU visible list) showed zero missing in a run that did vanish, so the loss was after the cull;
      `ENGINE_GRASS_APPEAR_SECONDS=0` removed it, which led to the batch-wide mode. The flag button tied the
      user's sightings to frames; the spin test scene and spinning in the landscape showed it needs movement.

## Moved from todo.md on 2026-11-24 (evening)

### Landscape demo expectations (2026-11-19, from the Chlorophyll Engine video)

- [x] 2026-11-22 (agents/look): `autoExposure groundWeight` (0-1): the top third of the frame meters a fifth
      as much, so the meadow meters alike whatever stands behind it; landscape: groundWeight 1,
      outputExposure -0.25 -> -1. 2560x1440, meadow near / mid: ForestEdge 0.216 / 0.249 -> 0.060 / 0.100,
      FieldWalk 0.101 / 0.104 -> 0.083 / 0.074, forest wall 0.065 -> ~0.04 (reference crowns 0.03-0.05).

- [x] outputExposure -0.75 (2026-11-22): near / mid meadow ForestEdge 0.078 / 0.129, FieldWalk 0.095 / 0.087,
      MeadowSlope mid 0.065 (cloud shadow). Forest interior darker than before (ForestFloor whole frame 0.015):
      accepted by the user for now.

- [x] **Understory foliage glowed in canopy shade** (user 2026-11-22: yellow-orange blades by a rock at
      ForestFloor): the woodland pack's tall bush cards got FoliageTransmission's crown-scatter term (0.3 of the
      sun when shaded, meant for a crown's own underside). `Material::foliageCrownScatter` (default 1); the pack
      bush materials set 0. Blade area 99th percentile 0.204 -> 0.079 (`.local/foliage/ff_blades_fix3.png` in
      agents/look); other foliage materials unchanged.

### Status (2026-11-20, night)

- [x] Ground walks follow the trails (2026-11-23, user: "follow nearby paths drawn in the terrain for a more
      typical path players would take"). The W drive flies a straight line at ~23 m/s, through forest and
      across open ground. `GameBackendCapture --route <SplinePath object>` walks the camera along a scene path
      instead: eye on the route, gaze 12 m ahead on it, a fixed `--route-speed`/60 m per frame (default 6 m/s,
      a run), so every run renders the same poses (Engine/Game/Benchmark/README.md).
      `tools/add_landscape_viewpoints.py` traces the trail contour into two 400 m routes near the start:
      `ForestTrailWalk` (from the Trail viewpoint: forest, edge, meadow, forest; 42 % in forest) and
      `MeadowTrailWalk` (open meadow trail, sun behind). Both stay on the track the whole way
      (`.local/routes/check_*`, a snapshot every 46 m).
      DX12, 1920x1080, 1000 frames at 6 m/s (~100 m), two runs each: forest trail 14.4 / 14.7 ms median,
      p99 29-30, 3-4 frames over 50 ms; meadow trail 12.9 / 13.1 ms, p99 26-28, none over 50 ms. Whole route at
      23 m/s (1060 frames): forest 16.3 ms, p99 66, 21 over 50 ms; meadow 15.8 ms, p99 70, 16 over 50 ms. Same
      day, old FieldWalk W drive (380 frames): 12.2 ms, p99 43, none over 50 ms.
      Found: ~90 m into the forest trail a tree's low crown hangs over the track at eye height and the camera
      walks through its leaves (the crown-clearance item under Foliage).
      Use these for walking costs from now on: `--route ForestTrailWalk --frames 1000` and
      `--route MeadowTrailWalk --frames 1000` (add `--route-speed 23` for streaming stress).

- [x] **Baseline + temporal upscaling on the trail routes** (2026-11-23 evening, after the stamp/cliff removal and a
      cache rebuild; `.local/routes/perf_runner.py base1123`: interleaved native / `ENGINE_RENDER_SCALE=0.67`, two
      repetitions, 12 GB watchdog (16 GB for the uncounted cache-rebuild run, which peaked 5.1 GB after a
      12.1 GB transient killed the first try), retries; all 16 runs passed first time, peaks 3.0-4.3 GB).
      Medians, rep 1 / rep 2:

- [x] Bisected (2026-11-22, forest walk, interleaved pairs; `ENGINE_TERRAIN_STAMPS=0`,
      `ENGINE_FAR_PATCHES=0` added for it): the far grid's full-resolution cliff-cell patches. Stamps cost
      nothing measurable. Patches on / off: frame 20.8-21.0 / 17.2-17.3 ms, camera record 10.0 / 7.5,
      occlusion build 2.7 / 0.9 (occluder triangles 357k / 73k), 7-9 / 2 frames over 50 ms. Fix: far tiles
      give the CPU occlusion buffer their coarse grid only (`Mesh::occluderIndices` and the new
      `Mesh::occluderVertexCount`, set in `TerrainManagerFarTerrainUpdate.cpp`): frame 17.3-17.4 with
      patches against 17.2-17.5 without.

- [x] Patches still cost two ~150 ms hitches per forest walk (~65 ms without, at the same cell crossings)
      and ~0.4 ms GPU. Replaced 2026-11-22: no far-grid patches at all (`world.fargrid`, `write_archive`).
      Far stamp cells are now the cell's own terrain at stride 4 (16 m grid, edges at full resolution as
      at every terrain level, coarsest stamp or sculpt level), built through the cell's `Rebuild()`; far
      tiles leave them out (`TerrainManager::FarStampShowing`), keep cell detail in tiles that hold one,
      and stitch their edges from archive heights, as for loaded cells. Forest walk 17.3-17.4 ms, 2-4
      frames over 50 ms, worst 62-64 (patches: 5-6 and ~150 ms); terrain and shadow GPU as without
      patches. The 450 m cliff view is unchanged (`.local/terrain-stamps/farcells_450.png`).

### Next, in order

2. [x] **Levels chosen per view and per machine** (2026-11-24): `FairVisibilityLod()` (ScreenCoverage.h, on
   by default, `ENGINE_FAIR_LOD=0` off): LOD distances no longer scale by zoom/resolution
   (`ViewLodCoverageScale` = 1), the quality preset's geometry factor, or the detail budget
   (`DetailBudget::LayerLodScale` = 1, which also fixes grass reach and far-terrain levels); vegetation
   draw range ignores the vegetation quality factor; screen-pixel level choices
   (`SceneRendererLodSelection::ChooseScreen`) use a reference camera (60 deg, 1080 lines) at the world
   distance. Per-placement size scaling stays (world-space). Left: the screen-LOD hysteresis remembers
   the approach direction (a 10 % band), shadow ranges still follow the quality preset.

3. [x] **Far levels thinner than LOD0** (2026-11-24): `tools/match_lod_coverage.py` scales each LOD1/LOD2
   leaf card about its centre until the level covers what LOD0 covers (four side views and the top);
   lowering the alpha cutoff instead took it to a seventh and turned cards into rectangles. Impostors
   are baked from LOD0 with a cutoff matched to LOD0's coverage (`bake_octahedral_impostors.py`).

- [x] **Undergrowth vanishing for frames: shader reloads from unrelated file writes** (2026-11-23). Still camera
      at Trail: ferns, bushes and undergrowth vanished for 1-5 frames and came back, 2-5 times per 30 frames
      (104 k pixels), whenever frames were slow. Not occlusion (both paths off: same), not GPU blades, not the
      detail budget (0.6 throughout); CPU draws unchanged. It matched one counter exactly: 5 material shader
      pipeline cache misses on every vanishing frame. Cause: `ShaderSourceCache` watched the nearest existing
      ancestor of each shader search path, and material shaders try `Assets/Shaders/...` relative to the working
      directory, so it watched the project root, recursively; any directory entry created there (capture
      frames, terrain/object caches, logs) counted as a shader change, and a flood of writes overflowed the
      watch (read as lost notifications). Both forced a new revision on every source, every custom-shader
      material (ferns, scanned bushes, grass blades) missed its pipeline, the old one was retired and the draws
      were skipped until the recompile finished. Applies in play too (streaming cache writes, logs).
      Fixed: (1) a source that resolves watches only candidates whose directory exists (the ancestor climb stays
      for sources that do not exist yet); (2) only changed shader text (.hlsl/.hlsli/.h/.inc/.fx/.fxh) or lost
      notifications force a new revision, anything else rechecks the file's own time and size; (3) while a
      changed shader recompiles, its old pipeline keeps drawing (`MaterialShaderPipeline::previous`, retired
      when the new one builds; kept on a compile error). Still Trail, 120 slow frames: 0 pipeline misses (205
      before), largest frame-to-frame change 3,084 pixels (104,584 before).

- [x] Slow walk, every frame (2026-11-23): forest trail, 6 m/s (0.1 m a frame), 1000 frames, wind frozen
      (`.local/transitions/walk_forest`, `pops.py`: per-pixel steps a frame's neighbours do not share, then the
      largest compact step region per frame, crops in `walk_blobs.png`, `walk_top_*.png`). Found:
      1. **Camera inside trees** (frames 690-775, 869-906, 979; the largest and most frequent events): the trail
         runs under crowns at eye height and once through a trunk; leaf cards fill the view and come and go
         frame to frame, the view jumps leaving the trunk. Nothing fades foliage near the camera.
      2. **Ground-cover tiles switch whole** (frames 18, 782, 790, 876, each with `grass_tiles_built` /
         `grass_lod_switches` 3): a bright bush turns dark in one frame (the scanned bushes' coarse level, the
         "dark card" noted under Temporal upscaling), a bush appears whole beside the trail.
      3. Shadows: the 42 refreshes of cascades 1-3 on this walk show no step beyond the frames around them
         (0.87-1.14 of the local median); frames 8 and 18 jump the dapples on the path right after the measured
         frames began (and the frame time with them), not yet explained. Needs a targeted test (standing in
         dapple, turning) for the reported far-to-near quality switch.
      4. Frames 250-300: elevated steps all over (11.5 k against 5 k), no single event: shimmer, not yet
         looked at.

- [x] (1) Near-camera foliage fade and limb clearance on trails (2026-11-23).
      * `ApplyNearFoliageFade` (Object.hlsl, first in `ApplyGeometryFade`): cutout materials (flag 32: leaves,
        ferns, bushes, grass) dissolve per pixel from 1.25 m to 0.35 m from the camera, with the rotating
        screen dither of the LOD dissolves (TAA resolves it to a blend). Camera records only
        (`shadowCasterFade.z < 0.5`): shadows keep the plant; the foliage depth pre-pass (PSShadowMain on
        camera records) clips the same pixels as the lighting pass.
      * Trees keep their limbs off trails: `tools/measure_tree_reach.py` measures each species' reach from
        the trunk per 0.25 m of height (the big and large oaks: a bare trunk to 2 m, then limbs 5-8 m out;
        conifers ~2.5 m from the ground); LandscapeDriver `TreeReach` / `LimbsClearOfTrail`: in the band
        0.8-2.6 m above the ground (sunk edge shrubs lower it), reach x scale + 1.5 m must stay short of the
        track's centre line (`TrailDistance`, the metres TrailMask already computed). A tree that fails
        gives way to its stand's smaller species, else is left out; edge-pass trees are left out.
        `PaintRevision` bumped (generated layers share the cache key).
      Re-walked (`walk_forest_after`, `walk_before_after.png`): the stretch through crown and trunk (frames
      680-775) max step 98,483 -> 7,296 pixels, mean 10,919 -> 3,660; the trail runs clear under the canopy.
      Left: leaves 1-1.3 m over the eye (the clearance band stops at 2.6 m, the fade starts at eye + 1.25 m)
      dither away visibly against the sky while walking (frames 960-982): raise the band's top to ~3 m, or
      narrow the fade.

- [x] (2) Ground cover checked (2026-11-23): no ground-cover pops left in steady walking. `ENGINE_GRASS_LOG=1`
      prints where each tile starts to fade in: after start-up every new tile appears exactly at the range
      edge (80/80 m bushes, meadow, forest floor; 50/50 ferns; 200/200 far grass), where the per-plant shrink
      fade keeps it invisible. The two "bush" events of the first walk were other things: frame 18 a shadow
      jump (below), frame 791 the camera leaving an oak crown (fixed by the clearance). The scanned bushes'
      levels match in colour (0.70-0.73) and normals; level 1 has 76 %, level 2 51 % of the card area (thinner,
      not darker); not seen as a pop on this walk.

- [x] **Stale shadow cascade** (walk frame 18 -> 19, the largest event left; 2026-11-23): a bush, rock and trunk
      went from lit to shadowed in one frame as cascade 1 refreshed (mask 3). Confirmed as the cached image:
      with `ENGINE_SHADOW_STEPPED_CASCADES=0` the same spot is shadowed from the first frame
      (`.local/transitions/shadow_step_test.png`). Stepped middle cascades refreshed only when the camera
      crossed a step, so whatever changed after one was drawn stayed out of it.
      Fixed in `SceneRendererShadows.cpp`: one middle cascade a frame, in turn, is checked against a signature
      of the shadow casters inside its light-space square (`ShadowCascadeContent`: casting items, and the
      instance blocks of shared prefab parts, by identity and instance/geometry revision; boxes projected
      with |M| x extent), recorded whenever the cascade is drawn (`m_directionalShadowCascadeContent`); a
      changed one refreshes (at most every 8 frames). The far cascade keeps its own rule (kilometres, several
      ms). The frame-18 jump was not a caster change (the signature matched): the image was drawn early in the
      still warm-up, likely while tree levels and leaf textures were still streaming, which nothing
      announces; so a middle cascade also refreshes once 120 frames old (~2 s). Result: the warm-up redraws it,
      the bush region changes 1.3-2.0 k pixels a frame (as without stepped cascades) instead of 26 k at frame
      18 (`walk_periodic`). Cost (signature check alone, interleaved forest/meadow trail runs, on / off):
      frame 12.37-12.72 / 12.92-13.09 (forest), 11.23-11.69 / 12.03-12.84 (meadow), shadow GPU and record
      unchanged: within noise. `ENGINE_SHADOW_CONTENT_REFRESH=0` turns the content check off.
      Not done: blending old and new cascade images, so a refresh that does change a stale image still changes
      it at once (now within ~2 s of it going stale, not at the next step). [Done 2026-11-24, below.]

- [x] **Far tree shadows appearing late, cascade refreshes changing shadows at once** (user 2026-11-24:
      "shadows just appearing under far trees"). Three parts, all in `SceneRendererShadows.cpp`:
      1. Far cascade region refresh: the far tile keeps a caster signature per cell of an 8 x 8 grid
         (`ShadowCascadeRegionContent`, same identity/revision hash as `ShadowCascadeContent`); every 4 shadow
         frames the changed cells are found and only they are cleared and redrawn (scissor + the cached
         projection cropped onto them, so the texels are the same), at most a quarter of the tile per refresh,
         nearest the camera first. Before, the far tile waited for structure changes to be 240 frames old
         (4-10 s), so streamed-in far trees had no shadow and theirs then appeared at once.
         `ENGINE_SHADOW_FAR_REGIONS=0` restores that; frames.jsonl `shadow_far_region_cells`.
      2. Refresh blend: the atlas has a third row of tiles (2 x 3; 24 MB at 2048 instead of 16). A cached
         cascade past the nearest that is redrawn (stepped centre moved, casters changed, the 120/600-frame
         refreshes) goes to a spare tile, and the lighting lookup fades from the previous image (reached
         through the new projection plus a clip offset: stepped cascades only translate) to the new one over
         20 frames (`ENGINE_SHADOW_BLEND_FRAMES`, `ENGINE_SHADOW_REFRESH_BLEND=0` off). ObjectGPUData +48 bytes
         (`shadowAtlasTiles`, `shadowRefreshBlend[2]`). Only the sun-lighting lookup blends, and the old image
         gets one bilinear tap: every call site is inlined, and blending all of them (fog shafts, canopy taps)
         with full PCF took Object.hlsl PSMain from 156 to 343 s in fxc (now 186 s). Region refreshes are
         not blended (they draw into the tile in place).
      3. Tree shadow hand-off: LandscapeDriver's forest layer `shadowFadeWidth` 1 -> 30 m
         (`ENGINE_TREE_SHADOW_FADE`): the LOD1 casters and the impostor casters cross-fade (dither) over
         190-220 m instead of swapping at 220 m.
      Checked on grass_test (Vulkan, wind frozen): blend/regions on vs off at a still pose 463 of 518 k
      pixels differ, same frame time; a walk with 600-frame blends kept shadows under their trees. Not
      checked in the landscape (too heavy for the dev PC): (1) needs far trees streaming in, (3) only exists
      there.

- [x] **Forest-edge trees a level down: the detail budget's floor** (user 2026-11-23: "trees on the edge of
      forest have low lod's/quality ... texture budget too low?"). Not textures: texture budget 10,688 MiB,
      ~1.7 GB used. The detail budget sat at its 0.6 floor on every frame of every walk (its geometry estimate
      is always over its 6 ms allowance), pulling tree levels in to 0.6x: forest-edge trees at 20-40 m drew a
      level down (Detail view green against red; `.local/edge/cmp_ForestTrailWalk_a.png`, floor | scale 1).
      Full 400 m trail walks (4000 frames, `.local/edge/long_*`, `compare_walks.py`), floor / `ENGINE_DETAIL_SCALE=1`:
      frame median 12.77 / 13.05 ms forest, 12.98 / 12.71 meadow (no measurable cost: the frame is CPU-bound);
      forest tree placements LOD0/1/2 8/10/82 % -> 9/21/70 %, upper view not at full detail 45 -> 33 %
      (meadow 83 -> 79 %, mostly grass and far forest). Fixed: `DetailBudgetSettings::minimumScale` default
      0.6 -> 1 (ProjectLayers.h, ProjectLoader fallback); the budget can still raise detail, a lower floor is a
      project setting. The larger fix stays "Next" 6 (levels by size on screen, the budget as overload guard).

- [x] **Bushes rise out of the ground** (user 2026-11-23: "should just fade in using alpha"). Shrubs and bushes
      (placementReach bit 10) now dissolve in at full size at their range edge: VSMain passes the range fade
      as `placementOpacity` in TEXCOORD7.w (`placementDissolve`), the pixel entry points read it
      (`ReadPlacementOpacity`) and `ApplyGeometryFade` clips it with the rotating screen dither. Grass keeps
      the shrink. Shaders compile (fxc: Object.hlsl VSMain/PSMain/PSShadowMain, GrassFootBend, GrassBlades);
      not yet judged in a capture.

- [x] **Flicker walk after the fixes** (2026-11-23, `.local/transitions/walk_flicker`, `flicker.py`): largest
      flicker region per frame max 876 pixels at half resolution (0.17 % of the image; the first walk had
      regions to ~105 k), largest step region max 1,865; the 63 cascade refreshes (more with the 120-frame
      rule) show no step beyond their neighbours (0.00-1.12 of the local median). What is left is oak leaves
      passing at the screen edges right by the camera (frames 716-738): fast parallax and the near-fade
      dither, not things switching.

- [x] **Bushes vanished for ~20 frames and popped back; real-speed capture, flag button, TAA edge strip**
      (2026-11-23/24): done, see todo_history.md "Moved from todo.md on 2026-11-24". Diagnostics left in place
      (all off unless set): `--capture-async`, `--flag-button` / `ENGINE_FLICKER_BUTTON=1`,
      `ENGINE_TRACE_LAYER=<name>` (per-stage counters in frames.jsonl), `ENGINE_VISIBILITY_MONITOR=<file>`
      (CPU reference cull vs the GPU's visible list per batch), `ENGINE_TAA_HISTORY_DEBUG=1`, and A/B switches
      `ENGINE_GPU_SYNC_EACH_FRAME`, `ENGINE_STATIC_ITEM_CACHE`, `ENGINE_INSTANCE_POOL_STEAL`,
      `ENGINE_GRASS_LOD_FADE_SECONDS`, `ENGINE_GRASS_APPEAR_SECONDS`. Scripts in `.local/transitions/`
      (`flag_pops.py`, `flag_review.py`, `pairdiff.py`, `flicker.py`, `gpu_mismatch.py`). Test scene
      `Scenes/terrain/spin_test.scene` (`tools/build_spin_test_scene.py`): one flat cell, ~960 trees, scanned
      bushes everywhere, spin in place with `--look-yaw 12`.

- [x] Paths shaded per pixel at every terrain level (2026-11-22, agents/terrain). The trails are a contour of
      two-octave value noise, so instead of tracing polylines the terrain shader evaluates the same noise with an
      analytic gradient (`TerrainPathWeight` in Object.hlsl; generic `TerrainManager::ContourPaths`, set by
      LandscapeDriver from the `TrailMask` constants): near cells blend the gravel layer per pixel (only over
      grass, forest soil and gravel, inside the forest height band), far tiles blend the far gravel colour (off
      steep ground), the near cells' fade to the macro map keeps it. Past the pixel footprint the soft edge
      widens and the strength falls by the same share, so far trails stay one faint line. The paint, macro and
      far colours leave trails out; the cell heights sink 12 cm along the track; ground cover (mesh fields and
      the GPU blades' paint) keeps off it via `TerrainManager::ContourPathShare`, the CPU twin of the shader
      function. `ENGINE_TERRAIN_PATHS=0` paints them at the vertices again.
      Captures (`C:/repos/EngineProject-terrain/.local/terrain/c1`, `c2`, before | after): Raised view over a
      forest trail (blotchy blocks → one clean track, near and at ~300 m), MeadowSlope (the meadow trail at
      600-800 m was a row of pale dashes, now a continuous thin path), Trail (ground view, ferns keep to the edge).
      Cost: none measurable (interleaved field and trail walks: terrain GPU 0.52-0.54 / 0.86 ms either way,
      frame 16.3-16.5 / 16.0-16.3 ms).

- [x] Far objects at the far terrain height (2026-11-22, agents/terrain). Far tiles draw each cell as two
      triangles between its corner heights; far placements were on full-detail heights, |far - full| mean 2.9 m,
      median 1.6, p90 6.6, p99 19 m over the 2,209 fine cells (4-13 px at 500 m-1 km). `SnapToFarSurface`
      (TerrainManagerFarObjects.cpp) moves each far placement by the difference when its block is built; cells
      that draw their own terrain (far stamp cells) keep full detail. `ENGINE_FAR_OBJECT_SURFACE=0` turns it off.
      In the dense far forest the change barely shows in stills (Vista, MeadowSlope crops); cost none.
      Not done: loaded cells drawn at stride 2/4 (8-16 m grid, 256-384 m) keep their objects on full detail,
      and coarser far tile levels leave a residue within their screen error.

- [x] Baseline (2026-11-22, `n1_*` runs; the machine was in its slow state, so compare interleaved runs,
      not these absolute frame times):

- [x] Shared prefab parts, behind `ENGINE_SHARED_PREFAB_PARTS=1` (2026-11-22; running scenes only, the
      editor keeps per-layer objects). `Components/Terrain/SharedPrefabParts` (registry on the Scene: part
      key = source geometry, material, level, shadow role, every mesh setting, project layer; blocks go live
      when their layer finishes the asset, as the objects were enabled; releasing a layer only queues the
      detach, applied once a frame in frame preparation), `Mesh::SetInstanceBlocks`, one tile per block in
      `SceneInstanceBatching` (each source registered once per pass, a stamp on the instance data), blocks
      culled against the view's side planes in the tile loop and in texture footprints (camera or any
      cascade). Cell layers, near blocks and far blocks all go through it (one code path).
      Mesh objects ~5,500 → ~790. Captures at ForestFloor, ForestEdge, MeadowSlope, Vista match the per-cell
      objects (`sp_cmp_*.png`; differences are wind and cloud timing).

- [x] Found on the way: the shadow view requested texture footprints again although the camera's request
      covers every cascade. Off by default now (`ENGINE_SHADOW_FOOTPRINTS=1` brings it back): shadow record
      -0.5 ms with per-cell objects, -1.8 ms with shared parts; resident textures 770 / 773 MiB either way.

- [x] On by default for running scenes (2026-11-22, `agents/perf`; `ENGINE_SHARED_PREFAB_PARTS=0` turns
      it off). Captured on/off at Backlit, Trail, Cliffs, Escarpment, ForestWall, FieldWalk (plus the four
      earlier): same content, differences are wind and clouds (`.local/perfagent/s0_cmp_*.png` in the perf
      worktree). DX11 and Vulkan render ForestFloor the same with it (DX11 frame 15.9 → 10.0 ms, Vulkan
      16.6 → 16.2; single stills). Stills, DX12, frame off → on: Backlit 16.5 → 13.9, Trail 15.9 → 14.7,
      Cliffs 14.7 → 14.4, Escarpment 17.1 → 12.8, ForestWall 17.4 → 16.8, FieldWalk 17.6 → 17.3.
      One shared-parts run at Backlit was killed by the runner's 12 GB watchdog (13.3 GB); two reruns
      peaked at 3.4 GB (shared parts runs peak 2.8-4.0 GB against 3.9-7.9 off). Not reproduced.

- [x] Where grass time goes (2026-11-22, `agents/perf`, `ENGINE_GPU_WORKLOAD_COUNTERS=1`; the grass stage's
      counters are now in frames.jsonl as `gpu_grass_vs_invocations` / `_primitives` / `_ps_invocations`):
      grass is **vertex-bound**. FieldWalk 9.5 ms = 15.1 M vertex invocations, 5.0 M triangles, only 15.8 M
      pixel invocations (~7.6 per pixel) for 221 k plants; MeadowSlope 8.1 ms = 9.1 M vertices, 21 M pixels.
      The far ring alone (134 k tufts, 54 triangles each at every level) was 3.3 ms and half the vertices
      at FieldWalk (ring off: 6.8 ms, 7.5 M vertices, pixels unchanged); tall fields are 2.2 ms.

- [x] Far ring with real levels (Tsushima's fewer, wider blades with distance): `far_grass_tuft` LOD1 keeps
      45 % of its blades at 1.9x width, LOD2 22 % at 3.4x, one segment each (54 → 7 → 6 triangles;
      `tools/generate_meadow_grass.py`, LOD0 unchanged), at 45 and 90 m, chosen per plant
      (`MeadowGrass::ApplyFar`; `ENGINE_FAR_GRASS_LODS=0` keeps the full tuft). FieldWalk grass 9.5 → 8.0
      ms (15.1 → 8.4 M vertices); field walk 16.6-16.8 → 15.1-15.2 ms (grass 9.4 → 8.1, interleaved);
      MeadowSlope 8.1 → 7.9, ForestEdge and Vista unchanged. Look: the far meadow band past ~45 m reads a
      little smoother (`.local/perfagent/s3a_crop_*.png` in the perf worktree).

- [x] GPU grass blades, on by default since 2026-11-22 (`ENGINE_GPU_GRASS=0` keeps the mesh grass;
      `agents/perf`). The meadow's fine grass and tall fields are generated on the GPU every frame; shrubs, accents, bushes and the far ring stay
      meshes. How: each meadow `GrassField` in blade mode (`gpuBlades`, set by LandscapeDriver) hands its
      cell's height and grass paint grids to the scene's `GrassBlades` (Assets/Grass), which packs every
      loaded cell into one buffer and owns one carrier object (a 4-segment blade template, one dummy
      placement, `Mesh::gpuBladeField`). In the camera view the carrier's batch runs
      `Shaders/GrassBlades/GrassBlades.hlsl` instead of the placement cull: one group per 2 m patch of a
      camera-centred grid (frustum and occlusion-buffer culled), blades from a hash of patch and index,
      grass paint, the same tall-field noise as `GrassField::TallField` in cell-local coordinates (the
      fields line up with the far ring's tall tufts), Voronoi clumps (height, lean, hue; fountains: upright
      at the heart, arching toward the rim), rank thinning past the keep distance ((7 / d)^2, survivors
      widened by 1 / sqrt(share), the last kept blade grows in) and a shrink fade over the last 10 m of the
      60 m reach. The material shader `Assets/Shaders/GrassBlades.hlsl` builds each blade (bent quadratic
      curve, the generator's leaf outline and palettes, edge-on widening in view space, the grass minimum
      pixel width, the shared grass wind) and shades it with the meadow's `PSGrassMain`. The far ring fades
      in over the blades' last 10 m instead of from 30 m. No CPU tile builds for the fine grass.
      Numbers (interleaved field walks, off / on): frame 16.3-16.7 / 12.2-12.7 ms, grass GPU 8.6-8.9 /
      4.5-4.6 ms, frames > 50 ms 3-6 / 0-2. Stills: FieldWalk grass 9.0 → 4.8 ms (vertices 8.5 M → 2.7 M),
      MeadowSlope 8.4 → 4.3, ForestEdge 6.1 → 4.3. Per-frame grass CPU at cell crossings: p99 6.6-7.2 →
      3.2-3.3 ms (the 25-35 ms commit did not show in either build).
      Look (1440p tiles `.local/foliage/blhi3_on_*` against `blhi_off_*` in the perf worktree): MeadowSlope
      and ForestEdge close over the soil like the mesh grass; FieldWalk reads a little lighter and more even
      (the mesh meadow has darker pockets between tussocks).
      Found on the way: until a material shader has compiled, its draw falls back to the base pipeline; a
      carrier drawn that way read blade records as pool indices and hung the GPU (root SRVs have no bounds
      checks). Carriers now draw only once their shader is resolved.

- [x] Blades finished and made the default (2026-11-22, `agents/perf`).
      Grass stage at FieldWalk split (debug switch `ENGINE_GPU_GRASS_DEBUG`): leftover meshes 2.9 ms, blade
      pixels ~1.6, blade vertices ~0.15, the generation pass ~0.05. Most of the rest was not grass at all:
      every instanced batch ran reset, barrier, cull, barrier right before its draw (two GPU drains per
      batch, ~190 per frame). The camera view now records all its culls first and runs them in one
      compute pass with two barriers, then replays the draws (`ENGINE_HOIST_CULLS=0` restores the old
      order): FieldWalk 11.9 → 10.9 ms, grass 4.2 → 2.6, foliage depth 2.2 → 0.25 (cull work shows as
      ~1.9 ms opaque other); ForestFloor 13.9 → 11.4; Vulkan FieldWalk 12.4 → 9.7; DX11 10.1-10.3 →
      9.9-10.0. Blade mode also drops the meadow's remaining tall_grass_clump shrub.
      Pockets: a 2.4 m tussock Voronoi; toward the border of two tussocks short blades thin (90 %), lose
      up to 70 % of height and darken toward the roots, so FieldWalk reads as tussocks with shadowed
      hollows like the mesh grass (1440p `.local/foliage/pk2_*`, `pk3_*`); no measurable cost.
      Backends: Vulkan had no SPIR-V for the generation shader (no grass in blade mode); now built.
      DX11 draws blades through its indirect path. A renderer that cannot draw blades marks the field
      unavailable and the meadow rebuilds its mesh grass (`ENGINE_GPU_GRASS_DEBUG=4` simulates it).
      Default on, interleaved (blades / mesh): stills FieldWalk 10.7-10.9 / 14.0-15.0 ms, MeadowSlope
      12.0-12.5 / 15.4-15.5, ForestEdge 11.9-12.1 / 13.7-13.8; walks field 11.4-11.7 / 14.6-14.7 (p99 39 /
      48-53, grass 2.4 / 6.6-6.9), meadow 13.2-13.3 / 13.7-13.8, forest edge turning 13.2 / 13.5-14.1.

- [x] Octahedral impostors with normal, depth and occlusion atlases, lit with the near trees' leaf model and
      drawn through 1 as the last level: crown texture and shading to ~2 km and the blue shift with distance
      (expectations table, "Far forest"; reference gap (c)).

- [x] First pass 2026-11-22 (ahead of 2, on today's mesh grass): `meadow_tall_grass`
      (`tools/generate_meadow_grass.py`): a metre-wide patch of 7 crowns x ~50 fine blades,
      0.7-1.05 m, mostly upright, dark roots and bright tips (the references' two tones), a few
      wispy seed stems; 1,775 / 976 / 304 triangles; half wind response (full response laid the
      long blades flat). `GrassField` tall fields (`tallFieldDensity`, `tallFieldSize`,
      `tallFieldCoverage`, `shortUnderTallShare`): terrain-space noise; inside a field tall patches
      grow at up to 1.8/m² and the short sward thins to 30 %. Landscape: fields over ~55 % of the
      meadow, 28 m noise, `groundRootShade` 0.85 (`ENGINE_LANDSCAPE_TALL_FIELDS=0` for the old
      meadow). FieldWalk: the foreground is a closed tall mass (`.local/terrain-stamps/tg_zoom3.png`);
      grass GPU 7.5 → 7.8 ms, frame 19.4 → 19.1 (single runs).

- [x] Root-to-tip shading (2026-11-22, agents/look): PSGrassMain darkens the lower three quarters of
      a blade (ambient floor 0.6 -> 0.4, sun 0.82 -> 0.45, backlight too); the mass has dark depths.
      Wind lean not done.

- [x] Far ring in tall fields (2026-11-22): `far_tall_tuft` (0.75-1.05 m, 72 / 10 / 6 triangles with
      the perf branch's far LOD scheme) at 0.9 per m² inside the meadow's tall fields, same field noise
      (`ENGINE_LANDSCAPE_FAR_TALL=0` off). The far meadow keeps a tufted top instead of a lawn band.

- [x] **Bushes pop up from the ground while moving** (user 2026-11-22). Cause: the meadow's bushes were
      distance-thinned like the fine grass, so from ~16 m (0.2 of the 80 m reach) they shrank in and out by
      rank and visibly grew out of the ground near the camera. Fixed 2026-11-22: `Mesh::InstanceRange::thinned`
      (false for the grass field's non-fine species) sets padding bit 1024 / `selection.w` bit 26, and
      InstanceCull.hlsl and Object.hlsl skip the thinning for those ranges. Bushes are now full size to 56 m.
      Cost: none measurable (FieldWalk walk 19.3 ms median, grass 9.52 ms, against 19.5 / 9.57 before).

- [x] Bushes, low shrubs, nettles and saplings are scanned now (2026-11-22). `tools/scan_bush_cards.py`
      renders the Poly Haven scans (shrub_04, shrub_02, nettle_plant; CC0) into one alpha-masked card
      atlas (`ScanBushCards_diff.ktx2`, leaves graded toward the meadow green, stems toward bark), then
      builds plants from crossed card pairs with outward volume normals, both sides as separate
      triangles: `scan_bush` 1.5 m (720 / 360 / 144 triangles), `scan_bush_tall` 2.3 m (960 / 480 /
      192), `scan_shrub`, `scan_nettle`, `scan_sapling`; the old bush was ~33k triangles. A sprig scan
      cannot be simplified below ~1,200-1,900 triangles (every leaf is its own UV island), so whole
      scans were not an option. They draw from their own field, layer "Procedural meadow bushes"
      (`MeadowGrass::ApplyBushes`, material `Materials/Grass/scan_bush.material`) on meadow and forest
      soil, 80 m reach; LandscapeDriver drops the generated bushes, bramble and shrub from the meadow
      and forest floor fields (`ENGINE_SCAN_BUSHES=0` restores them). PSGrassMain leans textured
      plants' normals 0.15 toward the sky instead of 0.55, so they keep a shaded side.
      Cost: FieldWalk walk, interleaved A/B, 19.8 / 20.1 ms median against 20.1 / 19.8 with the
      generated bushes; grass GPU 9.43 / 9.22 against 9.81 / 9.70 ms.

- [x] Ferns are scanned now (2026-11-22). fern_02's texture is five flat fronds; they fill the bottom
      half of the atlas (now 2048x4096), and each fern is a rosette of arching, drooping frond strips:
      `scan_fern` ~1.2 m across (288 / 128 / 40 triangles) in the bush field's thickets, and
      `scan_fern_large` ~1.8 m (384 / 160 / 48) with `scan_fern` in a new layer "Procedural forest
      ferns" (`MeadowGrass::ApplyForestFerns`: forest soil, 0.8 per m² in 10 m thickets covering 45 %,
      50 m reach). The meadow field keeps only its tall grass clumps as shrubs; the forest floor field
      no longer draws `forest_fern`. Both scanned fields now pick detail per plant.
      Cost: not resolved. The machine drifted slower through the session (the generated build alone
      went from 17.4 to 23.2 ms on the same forest walk); the cleanest interleaved pairs give +0.6 to
      +1.0 ms frame and +0.5 ms command record in the forest (one more field's tile draws), grass GPU
      +0.1 ms, nothing on the meadow. Re-measure on a fresh machine. 32 m fern tiles (fewer draws)
      were tried: not all built 60 frames in, and they would arrive a whole tile at a time.

- [x] **Stamps are terrain brushes, not separate data** (user 2026-11-23: "stamps are terrain paint brush tools,
      should convert the brush shape to the stamp mesh and raise/lower/sculpt terrain data using the brush shape
      / stamp heights"). `TerrainBrushShape` (Components/Terrain) rasterizes a .stamp's finest level from above
      (stamp-space Y up, 128x128): the highest surface per sample above its outline's base, and a feathered
      coverage mask. `Terrain::BrushOptions::shape` (with `shapeYaw`) replaces the round falloff for Raise,
      Lower, Smooth, Flatten and Paint, and Raise and Lower also scale by its height map (user: "should act
      similar to a normal raise brush tool, but using a mesh with height map that raises certain places
      higher"): each frame held, a point rises by strength x mask x (height / tallest), so the stamp's peaks
      climb fastest and holding builds its shape. Editor: tool "Stamp" (that raise brush with a stamp shape
      required; Shift lowers; Strength as for Raise) and a Shape picker (Circle or any stamp under
      TerrainData/Stamps, or drop a .stamp) for the sculpt brushes, with a Turn setting and the turned outline
      drawn draped over the ground. The edits are height data like any brush stroke (undo, .terrainedit).
      Removed with it: world.cliffs (the builder's placement list and `--cliffs`/`--conform`), LandscapeDriver's
      cliff list, runtime cliff pieces, cliff macro marks and `cliffStamps`; the three landscape .sculpt cells
      and tools/cliff_sculpt.py; runtime stamp drawing (`Terrain::stamps`/`stampMaterials`/`stampPlacements`,
      `TerrainStampInstance`, `TerrainSculpt`, `TerrainManager::SetTerrainStamps`, far stamp cells and
      `farStampRadius`), the shader's stamp-grid surface, and stamp rock and sculpts in the volume build (volume
      cells are heights plus volume edits). The triplanar rock shader path stays (VolumeTerrain's steep faces,
      Sculpts/cliff_rock.material). The landscape cache key changed (first launch rebuilds cell caches).
      Tests: `TestStampBrush` in TerrainEditingToolsTests (raster, dabs building the shape, Lower taking it
      back out, turned stamp across a cell seam).

- [x] Phase 1, engine (2026-11-23): `Terrain::lateralOffsets` (x, z per vertex; `Terrain.
      lateralOffsets` in scenes), `GridPosition`, normals from the displaced neighbours, baked LOD
      levels; collision uses the mesh triangles. Overhang faces keep their winding
      (OrientTerrainSurfaceUp trusts vertex normals) and the slope-gated biplanar mapping already
      textures them. Checked with `build_stamp_test_scene.py --undercut 6` (east wall's top edge
      6 m out, 2 m past its foot, stamps dropped there): looking up from the foot, the face hangs
      over the camera, underside shaded (`.local/stamps/u4/up.png`: plain | undercut).

- [x] Phase 2, experiment (2026-11-23): `terrain_stamp_morph.displace` relaxes vertices near rock
      toward their neighbours' mean and projects them onto the nearest stamp point's tangent plane
      (0.35 m inside; `DisplacedStampInset` in Terrain.cpp), ground elsewhere, capped at 0.95 cell
      sideways; `build_stamp_test_scene.py --displace [--no-stamps]`. The bare terrain takes the
      rock's shape (`.local/stamps/d1/grid.png`), but with the stamps' patches on top, borders the
      grid could not reach hang as shards (`.local/stamps/d2/cmp.png`); morph-only looks cleaner.

- [x] Stage 1 (2026-11-23): `TerrainVolume` (.volume: i16 signed density, positive inside,
      x fastest then z then y; cached loader) and `MeshSurfaceNets` (one vertex per crossed cell
      at the mean edge crossing, a quad per sign-changing edge wound to face the air, normals
      from the density gradient); `VolumeTerrain` component (Mesh from the volume; collides
      through a `MeshObjectCollider` + static `RigidBody`, as terrain cells do).
      `tools/terrain_volume.py`: ground from heights (slope-scaled distance) smooth-unioned
      (1.5 m fillet) with the stamps as solid rock behind their faces (6 m deep) and edit
      brushes (rounded boxes, sphere subtraction); writes `TerrainData/Volumes/volume_test.volume`
      (129x41x129 at 1 m, 1.4 MB, ~3 s) and `terrain/volume_test.scene` (stamp test viewpoints,
      material `Materials/Terrain/volume_rock.material`, triplanar).
      Result (`.local/stamps/v1/all.png`, plain | edits): block and stamps are one rock surface,
      no patches or seams; a mass raised against the east wall, out sideways and down into the
      floor, fuses with the wall into an arch; a cave cut under the north rim opens into the
      block. Two dropped cubes rest on the arch top and the ground (`.local/stamps/v4/drop.png`).
  Perf against the height grid + stamps (same content, 1920x1080, GPU stage medians, two
  interleaved runs; whole-frame times were too noisy): Wall 7.3 ms terrain vs 4.1-4.4 ms
  volume; Above 2.6 vs 2.2 ms; 5,500 vs 45,500 triangles. The volume's single rock material
  was much cheaper per pixel than the layered terrain shader (`.local/stamps/perf/`).

- [x] Texturing (2026-11-23): `VolumeTerrain` shades through the terrain's layered path (a
      `TerrainBatchAppearance` per chunk, `worldExtent` for the world-mapped top and side
      projections): `flatMaterial` (grass) facing up, `steepMaterial` (rock) on steep faces,
      overhangs and ceilings, blended per vertex by slope (`steepStart` .. `steepEnd`). Vertex
      colour must be black there: the shader adds textured layers onto it (white washed it out).

- [x] Stage 2 (2026-11-23): `.volume` v2 is sparse (32^3-sample chunks; uniform chunks are one
      value; v1 still loads). `MeshSurfaceNets` meshes a block at a stride (levels 0-3 = every
      1st/2nd/4th/8th sample) with skirts along open edges (1.5 cells into the ground) so chunks
      at different levels leave no gaps. `VolumeTerrain` keeps 32-cell chunks as runtime child
      objects: level by camera distance (64/128/256 m, draw distance), 10 % hysteresis, Mesh
      runtime-LOD cross-fade; meshing on worker threads (nearest first, up to 4); chunks past
      the draw distance are dropped; chunks within `collisionDistance` get a hidden level-0
      mesh with a MeshObjectCollider on a static RigidBody.
      Test world: `tools/terrain_volume.py --size 512` (hills around the block, 513x52x513,
      23.6 MB sparse, ~15 M samples built a slab at a time). Far view of the whole world:
      47 k triangles in 256 chunk draws (~560 k at level 0 everywhere); draw distance 250 m:
      77 draws, 34.6 k triangles. Drop test passes with chunk colliders
      (`.local/stamps/s3/all.png`, `.local/stamps/s4/drop.png`).

- [x] 3a (2026-11-23): `BuildTerrainVolume` (TerrainVolumeBuild.cpp), the C++ twin of
      tools/terrain_volume.py: ground (height - y) x normal.y smooth-unioned with the stamps
      as rock (hash-grid nearest point, 2 m reach; skipped where the ground is deeper than
      the rock can reach); only chunks near the surface or rock are computed.
      `VolumeTerrain.fromTerrain`: builds on a worker from a copy of the terrain's data,
      shades with the terrain's own layers (painted weights on gentle ground, `steepLayer`
      on steep faces) and hides the terrain's mesh, stamp patches included.
      `build_stamp_test_scene.py --volume`. Matches the offline volume (33.5 k vs 32.8 k
      triangles at Corner; `.local/stamps/t3a/cmp.png`); the block's top is solid and runs
      into the rim (`.local/stamps/t3b/rim.png`). The first build sampled heights with the
      cell's span instead of the grid spacing (SampleTerrainSurface takes the spacing):
      flat ground, only the stamps' rock, a hollow ring. Startup 6.4 s with a 6 m rock
      search on the main thread -> 1.9 s (as a prebuilt volume).

- [x] 3b (2026-11-23): `TerrainManager.volumeCells` / `ENGINE_VOLUME_TERRAIN=1`:
      `ConfigureCellPhysics` gives each finished loaded cell a fromTerrain `VolumeTerrain`
      (`volumeSteepLayer` 4 = rock, `volumeSampleSpacing` 2 m: the heights are a 4 m grid)
      instead of its grid collider, sculpted cells included (below). Density builds (2 at once) and chunk meshing (half the cores, up to
      6) are capped across all volumes. The cell's mesh hands over only once every chunk
      the camera wants is shown (hiding it at build start left cells invisible for 4-7 s).
      `ENGINE_VOLUME_TERRAIN_LOG=1` prints each cell's handover time, chunks, triangles.
      Fixed on the way: chunks parented with MoveObject kept their world origin (the test
      cell sat at the origin, the landscape's do not); SetParentLocal. Cells' volumes now
      run one sample past their edges (surface nets stops half a sample short: blue slots
      between cells and against far tiles, `.local/landscape/trim6/gaps.png`).
      Trimmed landscape (`.local/landscape/landscape_trim.scene`: loaded radius 1, far and
      far-object radius 4; first run rebuilt the cell cache past 8 GB, later runs 3.4-4.2
      GB). Cliffs (cell -7,15): height terrain 43.7 ms frame, terrain GPU 14.2 ms; volume
      cells 20.9 ms, 5.1 ms; nine cells ready in 1.0-1.8 s each, 0.9-1.6 MB density.
      Escarpment view: 28.5 ms. A height-terrain Escarpment baseline hung (the known DX12
      hang) and outlived the watchdog for an hour; the runner now force-kills the tree.

- [x] Rock look (2026-11-23): steep faces take the sculpts' scanned rock
      (`rockMaterial`, cliff_rock.material) as a layer-array slice after the painted
      layers, projected from three sides (the sculpt shader path, `uvChannels[4]`), not the
      dark rock paint layer (`.local/landscape/trim4/cliffs.png` -> `trim13/cmp.png`).

- [x] Escarpment conversion (2026-11-23): sculpted cells (-1..1, -6) build their volume
      from the sculpt's finest triangles (distance to the nearest triangle, sign by vertical
      ray parity), queries clamped inside the cell. Holes along the cell borders came from
      coarse levels reading past the cell's edge: the volume now carries a 9-sample margin
      and chunks mesh only the interior (`TerrainVolume.interiorFirst/Cells`); neighbours'
      stamps are passed in (`neighbourStamps`); skirts are off. No gaps left
      (`.local/landscape/trim13/cmp.png`, top: height path / volume). Escarpment 26.2 ms,
      sculpt cells ready in 4-6 s; Cliffs 24.6 ms. The black shards at Cliffs are in the
      height-path baseline too (crag/ledge prefab objects), not the volume.

- [x] 3c (2026-11-23): seams and edit storage.
      Seams: the volume's margin now holds the real ground past the cell's edge
      (`TerrainVolumeInput.neighbourHeights`, loaded cells' or the height archive's, via
      `TerrainManager::FeedVolumeNeighbours`; cells built before the archive is known are
      rebuilt when it arrives) and the neighbours' sculpts (`sculpts`, used inside each
      sculpted cell's footprint instead of clamping queries to the cell). The grid's bottom
      sits on a multiple of 32 samples (a chunk), so every cell samples one world lattice and
      cuts it into chunks in the same places: before, each cell's own lowest point set it and
      neighbours' coarse vertices missed each other.
      `.local/tests/TerrainVolumeTests.cpp` (target TerrainVolumeTests) checks that two
      neighbours mesh identical vertices along their edge at strides 1, 4 and 8. Against
      the far tiles: the thin dark slits along the escarpment's top edge (row -6 against
      far row -7) are shorter than before and on a par with the height path, which has them
      too (`.local/landscape/trim17/low_cmp.png`: height, volume before, volume now).
      Volume chunks also take their cell's ground-cover tint now (draw preparation read it
      from a Terrain on the drawn object only): the meadow under the escarpment was lime
      instead of grass-shaded (`trim17/aerial_cmp.png`).
      Edits: `TerrainVolumeEdit` (sphere, capsule, box with yaw and rounding; add or
      subtract, smooth blend), per cell in `<cellDataDirectory>/<x>_<z>.volumeedit` (text, cell
      space, applied in order after ground and stamps). An edit belongs to the cell holding
      its centre and must be under a cell across; the eight neighbours read it, so it can
      cross a border. `TerrainManager::AddVolumeEdit` saves and rebuilds the cells it reaches
      (`VolumeTerrain::Refresh`: the old chunks draw until the new ones are ready).
      `VolumeTerrain.editFile` for standalone cells; `tools/build_stamp_test_scene.py
      --volume --edits FILE`. Seen: a tunnel through the test block, a crater, an arch
      (`.local/stamps/edits/`), and a shaft and a tunnel across the -1/0 border of the
      escarpment with no step at the border (`.local/landscape/trim18/zoom.png`; test data
      in `.local/landscape/data`, the real landscape has no edits).
      Not covered: far tiles, the height archive and height queries (placement, grass,
      trees, brushes) keep the height field, so trees and grass stand over a carved hole;
      no editor tool yet calls AddVolumeEdit.

- [x] Re-sculpt 2026-11-22: folds untangled in 3D after the medians (946-2,387 quads per cell remain
      turned over, ~0.5 %; likely overhangs a single sheet cannot hold) and cavity darkening doubled
      (radius 2.5 m, 0.9 per metre). Borders still identical (513 points, tint difference 0). The bright
      slivers are gone (`esc_side_v3.png`). The "hole" at the x=0 border (`hole_zoom_v3.png`) is a real
      crevice: camera rays through it hit front-facing triangles 5-14 m deeper than the rock around it,
      in shadow.

- [x] Far: within `farStampRadius` (8 cells) a cliff or sculpted cell draws as its own terrain at
      stride 4 (far stamp cells, see the performance section in Status); checked at 450 m.

### Research references (foliage performance, web search 2026-11-22)

- [x] Measured (2026-11-22, `agents/perf`, shared parts on). The pre-pass pays for itself many times:
      without it (`ENGINE_FOLIAGE_DEPTH_PREPASS=0`) leaf shading is 12-15 ms (forest walk 15.0-15.1 →
      23.5-23.9 ms). With it: depth 3.4-4.0 ms + leaves 1.3-2.0 ms. The pre-pass is fragment-bound:
      `ENGINE_GPU_WORKLOAD_COUNTERS=1` shows 72 M pixel-shader invocations at ForestFloor (~35 per pixel at
      1080p) and 29-30 M at Vista, against 3.3-3.7 M triangles.

- [x] Tried and removed (no gain): terrain, bark, rocks and grass before the pre-pass (pre-pass -0.3-0.4 ms
      but bark +0.6 and Vista terrain +0.7, walk unchanged at 15.1-15.2 ms); camera-view tiles sorted
      near to far (pixel-shader invocations unchanged: the pre-pass shader discards and writes depth, so
      early depth rejection does not happen whatever the order).

- [x] Done 2026-11-22 (`agents/perf`): stepped cascades, on by default (`ENGINE_SHADOW_STEPPED_CASCADES=0`
      turns them off). The cache already existed (cascades re-rendered only when their centre moves, at
      most two a frame while walking, the far one every 240-600 frames); the middle cascades followed the
      camera texel by texel, so walking re-rendered ~2 cascades a frame. Now every cascade past the
      nearest moves in 1/8-range steps like the far one, draws plants in their rest pose, and leaves the
      rotating wind refresh to the nearest. Interleaved walks: cascades re-rendered per frame 1.96-2.22 →
      1.13; shadow GPU 2.6-3.1 → 1.1-1.4 ms; forest walk 14.6-14.9 → 13.1-13.4 ms, walk + turning
      15.4-15.5 → 13.9-14.0 ms (frames > 33 ms 17 → 3-13). Captures at ForestFloor, ForestEdge,
      MeadowSlope, Vista, FieldWalk: same shadows, differences are wind. New frames.jsonl key
      `shadow_cascades_refreshed` (bit mask).

- [x] Longer shadow ranges (2026-11-22, `agents/perf`). Interleaved walks (forest, field, turning) and
      stills at ForestEdge, ForestWall, MeadowSlope, Vista, FieldWalk and a mid-range view with trees at
      ~100-250 m (`--camera-position 40,-8,-80 --camera-target 140,-27,90`, "MidB"); looks from stills
      with `ENGINE_WIND_TIME=100` (freezes wind; cloud shadows still move).
      1. Trees cast with their LOD1 mesh to 220 m (was 90), `ENGINE_TREE_SHADOW_DISTANCE`. Alone it changed
         nothing: the near cascades ended at 150 m, and `foliageCasterDistance` (150 m) caps every
         wind-animated caster, the oaks among them (it would have left trees at 150-220 m with no shadow,
         since the impostors only cast past 220 m).
      3. Near cascades to 250 m (was 150) with the split blend 0.65 → 0.79, so the first cascade keeps its
         ~18 m (second 43 → 49 m, third 49-250 m), and the foliage caster budget 250 m: the
         `RealtimeShadowSettings` defaults, project loader fallbacks and template. A/B switches
         `ENGINE_SHADOW_NEAR_DISTANCE` (keeps the authored first split), `ENGINE_FOLIAGE_CASTER_DISTANCE`;
         the old behaviour is tree 90 / near 150 / foliage 150. Projects that write 150 m keep it.
         Cost 1+3 (new / old): shadow GPU forest walk 1.33 / 1.18 and 1.54 / 1.48 ms, turning 1.35 / 1.30
         and 1.36 / 1.52; shadow recording +0.1-0.36 ms CPU in most stills; frames within drift. On by
         default. Look: near shadows unchanged (ForestFloor 0.02 % pixels), ForestWall's canopy at
         90-220 m self-shadows more deeply, elsewhere ≤ 0.2 % (the meadow ridge at ~150 m in MidB).
      2. Rocks, boulders, logs and stumps cast to 150 m (was 80), `ENGINE_PROP_SHADOW_DISTANCE`: forest walk
         shadow GPU 1.23 / 1.46 ms against 1.36 / 1.56 off, frames 15.2 / 15.7 against 15.2 / 16.2: no
         measurable cost, on by default. Scanned bushes, shrubs and ferns (`ENGINE_SCAN_BUSH_SHADOWS=1`,
         out to their 80 m reach): shadow GPU +0.7 ms (2.01 / 2.25), shadow recording +0.8-1.1 ms CPU,
         forest walk frames +1.3-2.5 ms with more spikes, for a subtle look (bushes self-shadow and read
         more solid at MeadowSlope). Off by default.

### Foliage Phase 1: Leaves lit as volumes (biggest visual win, ~free)

- [x] Oak-pack impostors (`*_Impostor`, the pack's Billboard_Mat) are not re-baked, so they still carry the
      old flat leaf lighting. The forest clusters were re-baked from the cooked meshes (Foliage Phase 1c). [2026-11-24: every tree, the oaks included, re-baked as octahedral impostors from LOD0.]

### Foliage Phase 2: Pay for it (performance)

- [x] ~~**Trimmed cards**~~ (cook step): not worth it without a visible change; checked 2026-11-23. Leaf
      pixels pass only where sampled alpha x fade >= cutoff (hashed coverage and edge fade only remove), so
      a trim is invisible if it keeps every texel that can pass at any mip the LOD uses. The oak leaf atlas
      (`Texture 6 6`, 4096², 22 % opaque at 0.32) maps at ~1,270 texels/m, so cards sample mip ~4 at 16 m
      and mip 7 at the end of LOD0; TextureCook's coverage-preserving mips grow the passing area with
      level. Share of the texture that can pass when sampling up to mip k (grown 3 texels per level for
      bilinear and anisotropic footprints): k=0 30 %, 2 42 %, 3 52 %, 4 64 %, 5 78 %, 6 91 %, 7 100 %. A
      look-preserving trim of LOD0 (mip 7) keeps everything; one exact to ~16 m would keep ~64 % of the
      area for 4-9x the leaf triangles and thin crowns past that. A stats tool was written and removed
      (the session's `tools/trim_leaf_cards.py`, never committed). Options that do change the look:
      trim per LOD at a chosen mip, or a lower-resolution, tighter-packed leaf atlas.

- [x] **Temporal upscaling** (2026-11-23, DX12 game renderer). The game now applies an internal render scale:
      the scene's `renderScale` times the quality preset's (as the editor's game view did), or
      `ENGINE_RENDER_SCALE=0.5..1` for A/B runs. `DX12GameRenderer` keeps the swap chain at window size and
      sizes the depth target, viewport and scene colour by the scale (`GetRenderWidth/Height`,
      `ApplyRenderScale`, applied between frames like a resize); scene views record at the render size.
      With TAA, `TemporalResolvePS` upscales when the scene is smaller than the history (`TemporalUpscale`:
      3x3 samples placed by the jitter, Gaussian weights, more weight for a sample landing near the output
      pixel, same history clipping and reprojection). Without TAA, `UpscalePS` stretches bilinearly.
      Benchmark header: `render_width`, `render_height`, `temporal_upscaling`. Checked on
      lighting_showcase, 960x540, RTX A2000 Laptop: opaque GPU 0.357 → 0.194 ms at 0.67; edges close to
      native, slightly softer (`.local/taau/edges.png`: native | upscaled | bilinear).
      Detail follows the output: `SceneRenderer::SetDetailResolutionScale` (output / render pixels, set
      by the game before recording) makes LOD coverage, the detail budget, texture footprints and the
      LOD selection's screen view use output pixels. Without it a near scanned bush in grass_test dropped
      to a coarse level with eroded leaves and a dark card at 0.67, with TAA or bilinear alike.
      grass_test turning at 1920x1080 (`--look-yaw 3`, no drive so poses match; two interleaved pairs,
      RTX A2000 Laptop): frame 25.5 / 25.5 → 16.5 / 16.4 ms; GPU grass 15.7 → 10.7, terrain 3.6 → 2.0,
      clouds 2.8 → 1.4, AO 0.6 → 0.2 ms. Look (`.local/taau/turn2/full3.png`, native | 0.67 TAA |
      0.67 bilinear): same LOD everywhere, upscaled slightly softer, no ghost trails while turning;
      bilinear has stair-stepped edges. Not checked: walking (drives move by time, so runs at different
      frame rates end at different places).

### Foliage Phase 5: Trees

- [x] **Octahedral impostors** (2026-11-24): `tools/bake_octahedral_impostors.py` rebakes every tree's far
      level (Oaks, Conifers, Broadleaf: 23) as a hemi-octahedral impostor, 8 x 8 views of 128 px (1024 colour
      and normal atlases, `<Tree>_Octa_{Color,Normal}.ktx2`), from the prefab's LOD1 parts with their own
      textures (cooked textures read with the new `TextureCook --decode`, since the generators' PNG sources
      are not kept). Views carry real normals (rounded toward the crown) and an occlusion term (dark deep in
      the crown and low down). The impostor mesh and material are overwritten in place (prefabs unchanged).
      Object.hlsl VSMain: three quads, each takes one of the three views around the direction to the camera
      (or the light, in shadow passes), lies in that view's plane through the crown centre, and draws the
      pixels of its barycentric weight's dither interval (ApplyGeometryFade interval mode); the view's camera
      basis is the tangent frame, so the baked normals light with the leaf model. Detected per mesh
      (uvChannels[3].x = grid size); cluster impostors keep the 6-view path. spin_test from 100+ m, sun
      behind: forest mean colour (17, 24, 15) against (17, 23, 18) with full meshes, same silhouettes,
      6.7 ms against 18 ms. Left: depth atlas (no per-pixel depth or parallax; impostors intersect as flat
      quads), soft at the 33-60 m forced test distance (128 px views; meant for 220 m+), the cluster
      impostors.

### Also completed 2026-11-24 (not listed in todo.md)

- [x] **Game renders at the display's real size**: GameApplication calls
      `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`. On the 125 % display the game drew 1536 x 960 and
      Windows stretched it to 1920 x 1200 (soft, pixelated), and 1920 x 1080 captures were clamped to 1536 x 889.
- [x] **Grass flutter fades with distance** (grainy far meadow): `GrassWindBladeDetail` (GrassWind.hlsli) fades
      the per-blade flutter and cross-sway out from 6 to 24 m; the broad gust fronts stay, a little calmer far
      off. Cause of the noise: sub-pixel blades quivering out of step, which TAA cannot follow (it reprojects
      by depth only). `ENGINE_WIND_FRAME_STEP=<s>` advances wind per frame for captures that save every frame.
- [x] **Octahedral impostor view blend sharpened** (weights^4): the nearest view draws the whole tree most of
      the time; plain barycentric weights kept nearly every far tree in a three-way dither (grain).
- [x] **Trees on slopes to 42 degrees** (`TreeMinNormalY` 0.86 -> 0.74, where the paint turns to rock): forest
      paint ran to ~42 degrees while trees stopped at ~31, so steep forest slopes were bare soil up close. A
      placement rule only.
- [x] **No made-up forest anywhere**: far tiles and the macro color map are ground only; the canopy past
      `TerrainManager::farCanopyDistance` (2.2 km on the landscape; far blocks draw every real tree as its
      impostor up to it) is splatted from the real placements (LandscapeDriver: each cell painted as the
      cells are, `PaintCell`, its forest generated as the far blocks do, each crown's measured top-down leaf
      area, `TreeCrown` from tools/measure_tree_crowns.py, into the 16 m grid). The generic forest-cluster
      impostors (stands at invented positions) are off. Painted canopy from forest paint had shown forest where
      the real trees were sparser and turned to bare ground as cells loaded ("trees disappear walking in").
- [x] **Erosion paint in the near cells** (lush valley floors, scree, gully rock), the same function as the
      macro map (`ApplyErosionPaint`), so ground keeps its look from near shading to macro colour. Lushness
      rides per vertex (`Terrain::lushness`, uvChannels1.w), kept in the static cell cache (formats 6/7).
- [x] **Terrain tiling from afar**: past the hex-tiled range a second albedo sample at 0.37 scale, turned 37
      degrees, hides the repeats; the macro colour blend starts at 300 m (was 512).
- [x] **Far levels cover what LOD0 covers**: tools/match_lod_coverage.py scales LOD1/LOD2 leaf cards about their
      centres (oak LOD1 89.9-96.7 % -> 98.8-100.2 % of LOD0, conifer LOD1 105-122 % -> 101-104 %); impostors
      are baked from LOD0 with a cutoff matched to its coverage. Lowering the alpha cutoff instead was tried and
      dropped (cards turned into rectangles).
- [x] `TextureCook --decode <in.ktx2> <out.rgba> [max size]` for offline tools whose PNG sources are gone.
- [x] **Landscape starts on a walking camera**: `MeadowWalkCamera` (SplineFollower) walks MeadowTrailWalk (now
      its last 160 m, the final straight) at 1.4 m/s there and back; captures switch path-following cameras off
      and use the explorer.
