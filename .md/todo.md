# Engine roadmap

Last audited against the codebase: 2026-10-01.

Status convention:

- `[x]` Implemented and present in the repository.
- `[ ]` Not implemented or not complete enough to meet the stated acceptance criteria.
- `[ ] PARTIAL` Useful infrastructure exists, but the complete feature or validation target is still missing.
- `DEFERRED` Intentionally blocked on a prerequisite or product decision.

## Next priorities

## Asset editing

### File-type editor documents

- [x] HIGH: Replace the Assets explorer's scene/prefab special cases and OS-shell fallback with an internal editor registry keyed by normalized extension and, where necessary, probed asset type. Scene and prefab files open as independent documents, XML is routed by its probed root type, registered material/texture/mesh/model/skeleton/animation/audio/script types route to their typed document, and unsupported types retain OS associations. Dedicated authoring controls remain tracked separately below.
- [x] HIGH: Name every internal editor tab/window from the opened filename, including its extension. Keep the normalized canonical path as the stable hidden window identity and save target, expose it in a tooltip, allow equally named files from different folders to remain open, and append the dirty marker consistently.
- [x] MEDIUM: Separate reusable panel type names from asset-document titles and give each open document its own dirty state, undo/redo history, save strategy, selection state, and dependency-refresh path.

### Isolated Object, Mesh, and Skeleton stages

- [ ] HIGH: Generalize Prefab Stage into an isolated Asset Stage with Object, Mesh, and Skeleton focus modes. Build a transient scene containing only the focused subject plus the minimum camera, lighting, material, and deformation dependencies; unrelated siblings and attached items must not participate in display or selection.
- [ ] HIGH: Object mode must edit one object and optionally its child hierarchy while filtering Properties and Hierarchy to that focused object graph.
- [ ] HIGH: Mesh mode must display one mesh with geometry, UV, material, vertex-color, and skin-weight tools and save to the native mesh asset rather than serializing the transient scene.
- [ ] HIGH: Skeleton mode must display one bone hierarchy with an optional non-selectable ghosted skinned mesh, bind-pose controls, animation preview, and IK constraints while excluding unrelated model objects.
- [ ] MEDIUM: Add a document-supplied tool context to `SceneView` for selection filters, gizmos, creation actions, context menus, and asset drops. Add document-supplied projections/filters to the shared Hierarchy and Properties panels so hidden dependencies remain usable without appearing as editable subjects.
- [ ] MEDIUM: Keep `AssetPreviewCache` thumbnail-only. Interactive single-asset editing must reuse the independent `SceneView` document path instead of growing a second viewport implementation.

### Mesh authoring

- [ ] HIGH: Add a dedicated Mesh Edit stage with vertex, edge, and face selection; box/lasso selection; transform with snapping and configurable pivots; extrude, inset, bevel, loop cut, weld, split, delete, duplicate, bridge, and fill operations; indexed-topology validation; and undo/redo.
- [ ] HIGH: Add UV inspection/editing, per-face material-slot assignment, vertex-color painting, and overlays for normals, tangents, seams, non-manifold edges, and degenerate triangles.
- [ ] HIGH: Share mesh vertex selection with skin-weight editing so geometry and influences operate on one authoritative asset rather than temporary copies.
- [ ] HIGH: Introduce an authored-geometry transaction separate from runtime `SetDeformedVertices`. A commit must validate topology and attributes, update bounds and GPU resources, increment asset revisions, atomically save the native mesh, refresh dependents, and retain undo data.

### Skeleton and skin authoring

- [ ] HIGH: Add a dedicated rig editor for inserting, deleting, reparenting, and reordering bones as one validated, undoable operation. Update Skeleton joint order, inverse-bind matrices, AnimationBone palette/parent indices, Model node bindings, mesh influences, and animation channels together.
- [ ] HIGH: Replace fragile child-index Model node paths with stable imported node identities, or provide an explicit remapping transaction for every hierarchy edit. Inserting, deleting, or reparenting must never silently make an existing node index resolve to another object.
- [ ] HIGH: Add vertex/influence selection and weight painting with normalization, locked influences, mirroring, maximum-influence enforcement, visualization, undo/redo, and persistence to the native mesh asset.
- [ ] HIGH: Add an explicit skeleton editing mode that distinguishes temporary posing from rest-pose edits and provides mirrored transforms plus clear/apply-pose commands.
- [ ] HIGH: Add an Apply Rest Pose operation that validates all joints and affected meshes before rebuilding inverse-bind matrices.
- [ ] MEDIUM: Add a timeline/curve editor for creating, deleting, and retargeting bone tracks instead of exposing imported animation channels as read-only statistics.

## Deferred feature gates

- [ ] DEFERRED: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites above are complete.
- [ ] DEFERRED: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
