# Engine roadmap

Last audited against the codebase: 2026-10-08.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

## Asset editing

### Mesh authoring

- [ ] Add a dedicated Mesh Edit stage with vertex, edge, and face selection; box/lasso selection; transforms with snapping and configurable pivots; extrude, inset, bevel, loop cut, weld, split, delete, duplicate, bridge, and fill operations; and undo/redo.
- [ ] Extend the existing basic UV preview/editor with per-face material-slot assignment, vertex-color painting, and overlays for normals, tangents, seams, non-manifold edges, and degenerate triangles.
- [ ] Share mesh vertex selection with skin-weight editing so geometry and influences operate on one authoritative asset rather than temporary copies.
- [x] Add an authored-geometry transaction separate from runtime `SetDeformedVertices`. It validates topology and attributes, persists indexed native mesh data, updates bounds/GPU resources and authored revisions, atomically saves, refreshes dependents, and retains indexed undo data. This is the canonical mesh validation and persistence task.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
