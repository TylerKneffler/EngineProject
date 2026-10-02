# Engine roadmap

Last audited against the codebase: 2026-10-01.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

## Asset editing

### Editor documents and isolated stages

- [x] Route supported assets through internal document editors selected by normalized extension and, where needed, probed asset type; preserve OS associations for unsupported types.
- [x] Name document tabs/windows from the opened filename including extension; use normalized canonical paths for hidden identity and save target, expose paths in tooltips, allow same-named files from different folders, and show dirty markers.
- [x] Separate reusable panel names from document titles and give each document its own dirty state, undo/redo history, save strategy, selection, and dependency refresh path.
- [x] Open scene/prefab documents, individual mesh documents, and imported model skeleton documents in independent transient editing scenes. Object child-hierarchy and Skeleton ghost visibility can be toggled; imported model documents can switch between Object and Skeleton focus.
- [ ] Complete Asset Stage generalization: add Mesh focus switching within imported models; define and enforce each mode's minimum camera, lighting, material, skinning, and deformation dependencies; verify unrelated attachments never render and unrelated objects are never selectable across supported asset types.
- [x] Object mode edits an isolated prefab/object root with an Include child hierarchy toggle. For non-imported prefabs, Hierarchy context actions can add children and delete non-root descendants; viewport/world creation and external asset drops are disabled. Imported model hierarchy mutation remains disabled to protect node bindings.
- [x] Mesh mode displays one mesh, edits per-vertex position/UV/color and normalized skin influences, exposes its material component in Properties, and saves native `.mesh` data (OBJ edits save to a sibling `.mesh`).
- [x] Mesh view provides a 2D UV island/wire preview with vertex picking, direct UV dragging, and a context action to reset the selected vertex UV.
- [x] Skeleton mode filters hierarchy and selection to bones; supports a non-selectable associated skinned-mesh ghost, bone overlays, Apply Rest Pose, and temporary animation preview with clip selection, playback, looping, speed, scrubbing, and snapshot restoration on stop.
- [x] `SceneView` accepts document-scoped selection, creation, drop, transform, and tool-drawing controls; Hierarchy and Properties use the focused document/selection.
- [ ] Add safe Skeleton-stage context actions, editable dependency projections in Properties, and focused-stage component/dependency creation without admitting unrelated objects or invalidating model bindings.
- [x] Keep `AssetPreviewCache` thumbnail-only. Interactive single-asset editing reuses the independent `SceneView` document path rather than adding a second viewport implementation.

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
