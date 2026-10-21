# Fluid solver prototype

`FluidParticleSolver` is a reusable CPU 3D weakly compressible SPH solver. It
uses the verified `ParticleNeighborGrid` for density, pressure, and viscosity
neighbors. The emitter keeps its 60 Hz clock, spawn order, lifetime, capacity,
and reset behavior. Each solver tick uses 1 to 16 fixed internal substeps.

Enable **Fluid** on a `ParticleEmitter` to select this solver. Its own editor
section contains smoothing radius, particle mass, rest density, pressure
stiffness, viscosity, substeps, axis-aligned static bounds, boundary bounce,
and a speed limit. These fields serialize with the emitter. Gravity and drag
remain shared forces. Fluid mode uses the CPU simulation path, even when GPU
simulation is enabled. For a stationary container, use world-space particles
and specify bounds in world coordinates. If using local-space particles, bounds
are local to the emitter and gravity is transformed into local space.

This is a small prototype for tens or hundreds of particles. Positive pressure acts when density exceeds rest density;
the viscosity term reduces relative velocity. Acceleration and speed limits
help keep extreme settings finite. Boundaries clamp particle centers and
reflect the outward velocity by the chosen bounce fraction. Coincident
particles receive no pressure direction until they separate by another force.

`FluidParticleSolverTests` checks pressure separation, viscosity, gravity,
finite state and containment over 240 ticks, unchanged count, deterministic
reset, emitter restart, and settings serialization.

## Water visual and rigid boundaries

The **Fluid water visual** option reconstructs one screen-space surface for
each CPU fluid emitter. Its shader projects the particles, accumulates a
smooth density field, and draws one isosurface with a blue water
material. Simulated thickness darkens the interior; a soft rim, moving flow
highlights, small ripples, and a screen-space normal add surface variation.
The material uses the scene simulation clock, so reset and frame export
reproduce the same animation. It writes the frontmost contributing particle depth, so
solid scene geometry can occlude the water. The radius scale controls how
readily nearby particles join. The DX11 regressions save
`.local/artifacts/fluid-water-dx11.bmp` and
`.local/artifacts/particle-fluid-scene.bmp`.

Projected discs stretch along particle velocity and narrow across it while
keeping roughly the same screen area. This makes a fast stream and waterfall
read differently from slow water pooled in a wider container. The river scene
at `Engine/Core/Assets/Scenes/Showcases/particle_fluid_river.scene` tests that
behavior in a sloped channel with a low bridge and catch basin; its image is
`.local/artifacts/particle-fluid-river-scene.bmp`. Visual stretching does not
replace the SPH density response or create more water when a basin is larger
than the available particle volume.

The surface path samples up to 512 render particles per emitter. Larger
emitters keep their full simulation count but use evenly spaced particle
samples for reconstruction, so they retain the water material. This is a view-facing surface, not a
volumetric mesh; its highlights approximate reflection, and it does not
refract the scene or resolve transparency between
separate water emitters.

**Fluid rigid coupling** has three settings: Off, Fluid only, and Fluid and
dynamic body. The latter two sweep the particle radius against rigid bodies,
including a relative-motion sweep for a moving dynamic or kinematic boundary.
Fluid only changes particle position and velocity, so it is visual-only for
gameplay physics. Fluid and dynamic body also applies an equal-direction
opposite reaction impulse scaled by particle mass and the impulse control; that mode
affects gameplay physics on the next physics step. Static and kinematic bodies
receive no impulse. Collision hits remain available through the emitter's
existing hit batch. This is one-way contact sampling at the emitter's 60 Hz
tick, so very fast or deforming boundaries may need more detailed coupling.
