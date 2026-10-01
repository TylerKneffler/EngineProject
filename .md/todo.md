# Engine roadmap

Last audited against the codebase: 2026-10-01.

Status convention:

- `[x]` Implemented and present in the repository.
- `[ ]` Not implemented or not complete enough to meet the stated acceptance criteria.
- `[ ] PARTIAL` Useful infrastructure exists, but the complete feature or validation target is still missing.
- `DEFERRED` Intentionally blocked on a prerequisite or product decision.

## Next priorities

- [ ] HIGH: Add backend-neutral texture-array/cubemap resources and comparison-sampler support across DX11, DX12, and Vulkan.
- [ ] HIGH: Build point-shadow groundwork: radial-depth encoding, per-face caster culling, and a strict per-frame face/render budget; then add point-shadow regression tests.
- [ ] HIGH: Add baked probe visibility data and use it to reject or attenuate cross-wall probe interpolation; cover leaks and invalid/out-of-volume samples with regression tests.
- [ ] MEDIUM: Add renderer diagnostics and explicit memory/work budgets for probe data and future realtime GI, including visible budget exhaustion behavior.

## Deferred feature gates

- [ ] DEFERRED: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites above are complete.
- [ ] DEFERRED: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
