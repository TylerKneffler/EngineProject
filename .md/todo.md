# Engine roadmap

Last updated: 2026-10-13.

Status convention:

- `[x]` Complete
- `[ ]` Incomplete

## Next priorities
- [ ] **First, Mococo Play crash:** Reproduce the reported crash in the Editor and capture its exception or dump. The corrected scene passes full-dance DirectX11 rendering and three Play/restore cycles; its static morph buffers now avoid 402 MiB of CPU upload copies. The original crash cause is still unconfirmed.
- [ ] **Important, independent material feature:** Add subsurface scattering for skin and other translucent surfaces, with linear-light diffuse scattering, RGB radius, thickness/transmission, and backend/performance checks. See [rendering requirements](skinned-soft-tissue-research.md#separate-feature-subsurface-scattering-and-layered-skin-material).

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
