# Engine roadmap

Last audited against the codebase: 2026-10-08.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

### Skeleton and skin authoring

- [x] Add Skeleton mode to scene and prefab editors with nested Edit Bone, Add Child, Remove Bone, and Weight Paint tools. Paint the selected bone's vertex weights with an opaque gray heat-map override, circle/square brushes, adjustable size, strength, and falloff, and add/subtract/replace/smooth/normalize actions. Save weight edits to the shared mesh asset with stroke undo/redo.
- [ ] Finish Skeleton Edit viewport controls with hierarchy-driven active skeleton highlighting, configurable snapping and pivots for bind-pose transforms, and unified undo/redo across bone and weight asset edits.
- [ ] Add bind-pose bone authoring: create, delete, duplicate, rename, reparent, and mirror bones; edit joint position, orientation, and bone roll; validate parent cycles and joint references; update inverse bind matrices and dependent skinned meshes when the bind pose changes.
- [ ] Extend Weight Paint with bone locking and mirrored painting, and add precise painting across sparse meshes and overlapping surfaces while retaining normalized weights and one undo step per stroke.
- [ ] Add skin binding tools to assign or rebind a mesh to a skeleton, generate initial weights, assign selected vertices to bones, inspect and repair unweighted vertices or missing joints, and save/reload the authored skeleton and weights. Keep shared mesh references and existing animation joint references valid, or report mappings that require repair.
- [ ] Add regression tests for save/reload, hierarchy edits, skin binding, weight operations, and undo/redo, including the static default-pose view in Skeleton Edit and Weight Paint.

### Animation authoring

- [ ] Build a separate Animation Editor for posing the skeleton and creating or editing animation clips. Keep its pose transforms and timeline separate from the Skeleton Edit bind pose and skin weights.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
