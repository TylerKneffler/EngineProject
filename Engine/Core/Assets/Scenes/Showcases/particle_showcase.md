# Particle VFX gallery

Open `particle_showcase.scene` in the Engine Sandbox. The six stations show
fire, smoke, muzzle flash with tracers, sparks, a fountain, and snow. The
`ParticleVfxShowcaseDriver` on **Particle Gallery | cue controller** fires the
muzzle and tracer emitters every 0.65 seconds. Their timing belongs to the
scene script; each emitter retains reusable spawn and simulation settings.

The scene uses CPU particles so all stations work on DX11, DX12, and Vulkan.
The image regression captures `.local/artifacts/particle-vfx-scene.bmp` on
DX11 at a muzzle cue. Reset the scene to replay the same seeded effects.
