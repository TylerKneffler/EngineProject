# Engine roadmap

Last updated: 2026-10-13.

Status convention:

- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria. Completed items are in [todo_history.md](todo_history.md).

Priority labels: **Required** blocks an advertised workflow or protects saved results; **Important** improves routine authoring; **Optional** adds specialized features.

## Next priorities
- [ ] **Required, VMD scene fidelity:** Verify camera transform conventions and source axis/unit conversion against a real camera VMD fixture, and map VMD light direction to the engine directional light orientation. Add matching runtime behavior for VMD shadow mode/distance and independently bound model visibility/IK switches, then test save/reload and frame-rate-independent playback. Baked MMD clips play model visibility keys; the generic VMD Properties scene track remains unbound. The current component applies camera projection and light position/color but not those remaining scene properties. Export camera and light motion as separately reusable `.anim` assets once animation clips can target camera and light properties; they currently remain separate VMD track prefabs. `dance1.vmd` contains no camera or light keys, so the saved MMD dance scene cannot validate those conventions.
- [ ] **Important, independent material feature:** Add subsurface scattering for skin and other translucent surfaces, with linear-light diffuse scattering, RGB radius, thickness/transmission, and backend/performance checks. See [rendering requirements](skinned-soft-tissue-research.md#separate-feature-subsurface-scattering-and-layered-skin-material).

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
