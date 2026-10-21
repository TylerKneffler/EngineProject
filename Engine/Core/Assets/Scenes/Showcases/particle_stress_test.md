# Particle stress test

Open `particle_stress_test.scene` and start playback. The 4 × 3 grid contains
12 CPU simulated emitters with distinct colors and motion. The box sources
cover a 2.9 × 2.9 unit footprint, and the cloud sources fill a 2.7 unit
diameter sphere. Each emits 4,000 particles per second, lives for 1.3 seconds,
and is capped at 6,000. After warmup, the expected live count is about
62,400 particles.

The white center emitter bursts 8,000 particles at startup and every two
seconds. Its particles live for 0.45 seconds, so it tests transient load on
top of the sustained grid. Combined emitter capacity is 80,000, below the
scene's 100,000 particle budget and render limit.

The camera looks down on the grid to keep all 12 sources visible. Compare
frame time at startup, after warmup, and during a repeated burst.
