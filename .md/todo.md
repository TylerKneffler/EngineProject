# Engine roadmap

Last audited against the codebase: 2026-10-08.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

## Asset editing

### Mesh authoring

- [x] Add a dedicated Mesh Edit stage with vertex, edge, and face selection; box/lasso selection; transforms with snapping and configurable pivots; extrude, inset, bevel, loop cut, weld, split, delete, duplicate, bridge, and fill operations; and undo/redo.
- [x] Expose Mesh Edit mode in scene and prefab views with one hierarchy-driven active mesh, a searchable Mesh Tools panel, vertex/edge/face modes, move, extrude, inset, edge split, face delete, shared-asset save, and undo/redo.
- [x] Add an authored-geometry transaction separate from runtime `SetDeformedVertices`. It validates topology and attributes, persists indexed native mesh data, updates bounds/GPU resources and authored revisions, atomically saves, refreshes dependents, and retains indexed undo data. This is the canonical mesh validation and persistence task.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
