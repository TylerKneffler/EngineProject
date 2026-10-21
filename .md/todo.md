# Engine roadmap

Last updated: 2026-10-21.

Status convention:

- `[x]` Complete
- `[ ]` Incomplete

## Next priorities

### Particle systems and fluid-ready emitters


- [x] **GPU fluid neighbor search:** Port the verified CPU grid/query contract to the GPU compute path. Compare neighbor sets and boundary cells with the CPU reference before scaling particle counts.
- [x] **First fluid solver:** Implement a small 2D/3D SPH or position-based-fluids prototype using fixed substeps, density/pressure or density constraints, viscosity, gravity, and static boundaries. Expose fluid-specific parameters separately from general emitter controls; validate stability, mass/count behavior, and reproducible reset.
- [x] **Fluid rendering and coupling:** Add a fluid material/visual mode, then evaluate screen-space smoothing or surface reconstruction for coherent water. Add rigid-body coupling and moving boundaries only after the basic solver is stable; define whether interactions affect gameplay physics or visual effects only.
- [x] **Verification and examples:** Keep focused regression tests in `.local/tests` and guard their optional CMake targets with source-file existence checks. Create one ordinary VFX scene and one fluid test scene; place their scripted demonstrations in `Engine/Core/Assets/Scripts/Particles`.

Implementation references: [Wicked Engine GPU particles](https://wickedengine.net/category/devblog/2017/11/07/gpu-based-particle-simulation/index.html) and [SPH extension](https://wickedengine.net/category/devblog/2018/05/21/scalabe-gpu-fluid-simulation/index.html), [SPlisHSPlasH](https://github.com/InteractiveComputerGraphics/SPlisHSPlasH), and [CompactNSearch](https://github.com/InteractiveComputerGraphics/CompactNSearch).
