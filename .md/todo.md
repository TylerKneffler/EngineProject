# Engine roadmap

Last audited against the codebase: 2026-10-08.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

### Animation authoring

- [ ] Build a separate Animation Editor for posing the skeleton and creating or editing animation clips. Keep its pose transforms and timeline separate from the Skeleton Edit bind pose and skin weights.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
