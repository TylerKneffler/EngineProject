# Fluid river test

Open `particle_fluid_river.scene` in the Engine Sandbox. Water is emitted at
the high end of a sloped bed, runs below a low bridge, falls from the end of
the bed, and spreads in a wider catch basin. The bed, bridge, banks, basin,
and end wall use static rigid colliders. Gravity and the fluid solver move the
particles; the emitter has no scene script that moves them along the route.

The water surface joins nearby particles into one silhouette. Moving particles
also stretch along their velocity and narrow across it, so a fast falling
stream looks thinner while slower water in the basin looks broader. This is
visual deformation; the SPH particle count still determines the amount of
water. A small number of particles cannot fill an arbitrarily large basin.
The water material shades the interior a deeper blue and adds subtle moving
highlights and edge reflections as the flow changes.

The local scene regression checks that particles occupy the source, the bridge
region, and the basin after seven seconds. Its DX11 capture is saved as
`.local/artifacts/particle-fluid-river-scene.bmp` and checks visible coverage
along the stream and in the basin.
