# Engine roadmap

Last audited against the codebase: 2026-10-09.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

### Animation authoring

- [x] Build a separate Animation Editor for posing the skeleton and creating or editing animation clips. Keep its pose transforms and timeline separate from the Skeleton Edit bind pose and skin weights.
- [x] Open Animation Editor from an explicitly selected skeleton or skinned mesh. Resolve that rig's model, Animation Manager, and all meshes bound to its skeleton; show the chosen rig in the editor. Support prefabs with multiple rigs without silently choosing the first skeleton or manager.
- [x] Search the project Assets directory from the Animation Editor for rigged prefabs and open a chosen rig. Generate standalone `.rig` assets on skeletal model import or animation save, and reference them from Skeleton and Animation Manager components.
- [x] Move bundled skeletal prefab bind data into `.rig` assets and load those rigs through scene prefab references; stop writing duplicate inline joint and inverse-bind arrays for rig-backed prefabs.
- [ ] Validate a clip's referenced bones against the selected rig before previewing or editing it. Allow clips that key only some bones. Report missing or ambiguous bone mappings and require repair before applying unresolved channels; do not treat matching node indices alone as proof that clips from different models are compatible.
- [ ] Add regression tests for explicit rig selection, multiple rigs in one prefab, sparse bone channels, incompatible clip mappings, and mapping repair across save/reload.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
