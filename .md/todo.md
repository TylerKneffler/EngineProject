# Engine roadmap

Last updated: 2026-10-19.

Status convention:

- `[x]` Complete
- `[ ]` Incomplete

## Next priorities

### Model Manipulator: point controllers and AutoPosing

The current Scene and Animation views can pick a mesh area or bone, drag in the camera plane, adjust depth, choose Selected/Body Part/Full Body scope, and pin joints. Full Body uses multi-effector IK. Points mode shows joint position, segment direction, and segment twist handles. Ctrl-click selects multiple joint points for grouped movement. Active controller targets and fixed pins serialize with their AnimationBone components. Roadmap toward [Cascadeur Point Controllers](https://cascadeur.com/help/rig/rig_structure/rig_elements/point_controllers) and [AutoPosing](https://cascadeur.com/help/tools/animation_tools/autoposing):

- [x] **High, visible point-controller rig:** Draw joint position, segment direction, and segment twist points on resolved skeletons. Ctrl-click selects multiple joint points for grouped movement. Mesh and bone picking remain separate input modes. The Debug Editor build passes; live interaction review remains below.
- [x] **High, static controller activation and persistence:** Active points use soft full-body targets; inactive points remain unconstrained; fixed position/rotation pins remain hard targets. The viewport shows active and inactive point colors, and controller targets plus pin state serialize on AnimationBone. The focused serialization test and Debug Editor build pass.
- [x] **High, keyed controller state and sampling:** Animation clips store per-bone controller keys for active state, target, and fixed position/rotation pins. The Animation Editor's Key controllers button records the current rig at the playhead. Scrubbing switches to the latest key at or before the sampled time; before the first key the controller is inactive. Clip serialization and step sampling pass the focused regression test, and the Debug Editor builds.
- [x] **High, controller Auto Key:** With Auto Key enabled, dragging a point records its controller target at the playhead; Active and pin toggles also key their new state. Controller state is captured before pose keying resamples the preview, and it shares the pose edit's undo snapshot where available. The Debug Editor build and focused controller test pass.
- [x] **High, controller timeline editing:** Controller key times appear on their bone rows and in clip summaries. The existing timeline drag and delete actions move or remove the controller key at a selected bone/time alongside any ordinary bone keys there. The focused controller regression covers move and delete; the Editor source compiles, but linking the executable was blocked because `Editor.exe` was in use.
- [ ] **High, controller export:** Bake controller-constrained poses to ordinary bone channels for export, and check that moved model roots use the intended target space.
- [ ] **High, whole-body pose response:** Make moving one active controller produce a coherent pose across connected joints while respecting fixed controllers and other active targets. Extend the current deterministic multi-effector IK workflow with weighted pose preferences or prediction; validate behavior on reaching, stepping, pelvis movement, and off-center pulls.
- [ ] **Medium, controller-level rig behavior:** Provide per-controller IK/FK and hierarchy-selection settings, direction/facing controls, and optional additional points for hands, fingers, props, and non-humanoid rigs. Define how these controls interact with the existing Selected/Body Part/Full Body scope setting.
- [ ] **Medium, constraints and visual feedback:** Maintain intended distances between connected points, show stretched or compressed connections, and provide a way to restore their configured lengths. Add the body coupling and balance constraints needed for natural full-body dragging.
- [ ] **High, interaction validation:** Exercise mesh, bone, and point picking; camera-oriented drag and scroll depth; multi-selection; pinned targets; and animation keying in both editor views. Check usability and stability near straight limbs and unreachable targets in a live GUI session.
