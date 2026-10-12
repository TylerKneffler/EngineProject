# Engine roadmap

Last audited against the codebase: 2026-10-12.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

Paint priority: **Required** completes the advertised mesh texture paint workflow or protects its saved results; **Important** improves routine authoring but does not block existing paint modes; **Optional** adds specialized brushes or external-format convenience. Parent items below remain unchecked while any child is open.

## Next priorities

- [ ] Add localized stretch and compression for skinned mesh surfaces, driven by bone motion and mapped collision impulses. Preserve skin weights, cap deformation, and restore the authored shape after impacts.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
