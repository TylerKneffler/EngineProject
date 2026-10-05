# Engine roadmap

Last audited against the codebase: 2026-10-01.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

## Asset editing

### Mesh authoring

- [ ] Add a dedicated Mesh Edit stage with vertex, edge, and face selection; box/lasso selection; transforms with snapping and configurable pivots; extrude, inset, bevel, loop cut, weld, split, delete, duplicate, bridge, and fill operations; and undo/redo.
- [ ] Extend the existing basic UV preview/editor with per-face material-slot assignment, vertex-color painting, and overlays for normals, tangents, seams, non-manifold edges, and degenerate triangles.
- [ ] Share mesh vertex selection with skin-weight editing so geometry and influences operate on one authoritative asset rather than temporary copies.
- [ ] Add an authored-geometry transaction separate from runtime `SetDeformedVertices`. It must validate topology and attributes, persist indexed native mesh data, update bounds/GPU resources and asset revisions, atomically save, refresh dependents, and retain undo data. This is the canonical mesh validation and persistence task.

### Skeleton and skin authoring

- [ ] Add a dedicated rig editor for inserting, deleting, reparenting, and reordering bones as one validated, undoable operation. Update Skeleton joint order, inverse-bind matrices, AnimationBone palette/parent indices, Model node bindings, mesh influences, and animation channels together.
- [ ] Replace fragile child-index Model node paths with stable imported node identities, or provide an explicit remapping transaction for every hierarchy edit. Inserting, deleting, or reparenting must never silently make an existing node index resolve to another object.
- [ ] Add vertex/influence selection and weight painting with normalization, locked influences, mirroring, maximum-influence enforcement, visualization, undo/redo, and persistence to the native mesh asset.
- [ ] Add explicit temporary-pose and rest-pose editing controls, including mirrored transforms and clear/apply-pose commands.
- [x] Apply Rest Pose validates every joint transform before rebuilding inverse-bind matrices.
- [ ] Validate all affected skinned-mesh bindings and make Apply Rest Pose an atomic undoable transaction that leaves every dependent mesh valid on failure.
- [ ] Add a timeline/curve editor for creating, deleting, and retargeting bone tracks instead of exposing imported animation channels as read-only statistics.
- [ ] Add stage-specific IK constraint authoring.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
