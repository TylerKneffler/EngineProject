# Fluid basin test

Open `particle_fluid_test.scene` in the Engine Sandbox. The scene starts with
a distributed body of water in the basin, then a continuous
55-particle-per-second stream leaves the dark outlet and joins it.
The water visual merges the particles into one screen-space surface. A
kinematic paddle moves sideways, and the small gold
dynamic probe receives gameplay impulses from fluid contact.

`FluidTestSceneDriver` owns the initial pool placement, the switch to the
outlet, the paddle motion, and a reset every 12 seconds. The water emitter owns its fluid solver settings, including 4
substeps, pressure, viscosity, static bounds, and rigid-body coupling. Change
**Fluid rigid coupling** to **Fluid only** to see the fluid react without
moving the probe. The DX11 image regression captures
`.local/artifacts/particle-fluid-scene.bmp`.
