# Particle collision containment

Open `particle_collision_containment.scene` and start playback. Six static box
colliders form an enclosed room with interior bounds of x/z = ±3 and y = 0–5.
The cyan frame shows the boundaries while the collider faces remain invisible
so the particles can be inspected through the walls.

The emitter uses CPU simulation, bounce collision mode, 0.95 restitution,
and 0.02 friction. It starts with 200 particles and continues emitting at
180 per second. GPU simulation is disabled because this example exercises
the particle collision path.

The enclosure regression in `.local/tests/ParticleEmitterTests.cpp` launches
256 particles and checks their positions for four simulated seconds. It also
requires a hit on each of the six walls.
