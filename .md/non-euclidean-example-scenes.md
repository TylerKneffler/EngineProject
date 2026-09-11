# Non-Euclidean Test Scene Candidates

This document collects potential scenes for exercising the engine's non-Euclidean spatial implementation. Candidate status is noted when implementation begins.

1. **S-Curve Corridor**

   Two opposite bends connected smoothly. Tests curvature continuity, camera direction recovery, lighting through multiple bends, and whether the final exit is parallel to the entrance.

   Status: Implemented as `Engine/Core/Assets/Scenes/s_curve_corridor.scene`, with automated coverage in `MatrixLayerPortalTests`.

2. **Impossible Staircase Loop**

   Four stair flights connect cyclically so continuously climbing returns to the starting room. Tests orientation accumulation, gravity handling, repeated traversal, and floating-point drift.

   Status: Implemented as `Engine/Core/Assets/Scenes/impossible_staircase_loop.scene`, with automated loop-transform coverage in `ImpossibleStaircaseLoopTests`.

3. **Room Larger Inside**

   A small exterior doorway opens into a room several times larger than its exterior footprint. Tests scale transitions, collision dimensions, camera near-plane behavior, audio distance, and light range.

   Status: Implemented as `Engine/Core/Assets/Scenes/room_larger_inside.scene`, with automated scene and traversal coverage in `RoomLargerInsideTests`.

4. **Infinite Hallway**

   A short corridor repeats through hidden spatial connections. Landmarks identify each apparent repetition. Tests long-running traversal stability, recursion limits, culling, and accumulated transform error.

5. **Mobius Hallway**

   One circuit returns the traveler to the same position with their lateral orientation inverted. Tests handedness, normals, winding, controls, camera roll, and mirrored rendering.

6. **Gravity Cube**

   Each doorway leads to a different wall of the same room, making that wall become the floor. Tests gravity rotation, grounded state, character orientation, rigid bodies, and camera stabilization.

7. **Branching Geometry**

   The same doorway leads to different rooms depending on entry angle, position, or direction. Tests conditional spatial mappings, deterministic ray selection, collision agreement, and editor visualization.

8. **Nested Pocket Spaces**

   A room contains a small box whose interior contains another full room, recursively for several levels. Tests nested warp composition, transform precision, picking, lighting isolation, and render recursion budgets.

9. **Converging Corridors**

   Three physically separate corridors occupy the same rendered exit space without colliding until they cross their respective boundaries. Tests spatial-chart isolation, overlapping rendering, audio, and raycast ownership.

10. **Diverging Doorway**

    Travelers entering one door emerge from one of several exits based on their position across the aperture. Tests piecewise portal mapping, seams, velocity continuity, and split-object traversal.

11. **Twisted Tunnel**

    A straight source corridor gradually rotates its cross-section through 180 or 360 degrees. Tests camera roll, character up vectors, mesh deformation, tangent frames, and normal mapping.

12. **Expanding-Contracting Lens**

    A symmetric tunnel enlarges toward its center and returns to its original scale. Tests reversible deformation, optical-size consistency, light attenuation, collision scaling, and zero net scale drift.

13. **Nonuniform Door Pair**

    A rectangular doorway connects to a trapezoidal or skewed aperture. Tests piecewise mapping, corner correspondence, ray deformation, object splitting, and behavior near aperture edges.

14. **Curved Junction**

    A curved tunnel divides into two curved exits while maintaining smooth sightlines. Tests branching culling, curved-ray selection, lighting visibility, and continuity where warp volumes meet.

15. **Orbit Room**

    Walking straight across the room follows a circular path and eventually returns to the starting point without portals. Tests periodic formula warps, coordinate wrapping, directional continuity, and physics broadphase behavior.

16. **Spherical World Room**

    A locally flat room wraps onto the inside or outside of a sphere. Tests nonlinear surface mapping, local tangent movement, horizon rendering, gravity normals, and objects crossing chart seams.

17. **Hyperbolic Gallery**

    Repeated doorways appear to reveal exponentially more rooms than physically exist. Tests aggressive view multiplication, visibility pruning, recursion limits, and stable apparent scale.

18. **Moving Portal Pair**

    Connected doors translate and rotate while objects and camera rays cross them. Tests dynamic portal frames, velocity transfer, interpolation, collision rebuilding, and temporal rendering stability.

    **Implemented:** `Engine/Core/Assets/Scenes/moving_portal_pair.scene` uses a deterministic, independently phased motion driver on both endpoints, an automatic rigid-body probe, moving frame geometry, and a live camera view. `MovingPortalPairRegression` samples portal rays throughout the motion, checks inverse mapping and temporal continuity, and confirms that the synchronized moving trigger transfers the probe's linear and angular velocity through the current portal frame.

19. **Portal on a Moving Platform**

    One endpoint is attached to an elevator or rotating platform. Tests parent transforms, relative velocity, character grounding, destination motion, and object ownership during traversal.

20. **Crossing Warp Volumes**

    Two nonlinear volumes overlap, such as a bend intersecting a shrinking region. Tests deterministic priority composition, Jacobians, inverse behavior, lighting, and boundary continuity.

    **Implemented:** `Engine/Core/Assets/Scenes/crossing_warp_volumes.scene` overlaps a priority-0 curved chart with a priority-10 exponential shrink chart and visualizes the result with a colored lattice and mapped light. `CrossingWarpVolumesRegression` verifies order-dependent composition, the composed Jacobian and ray direction, numerical inverse recovery, smooth finite-volume boundaries, and realtime light placement.

21. **One-Way Spatial Window**

    One side behaves like transparent warped glass, while the reverse side is an ordinary wall or shows a different space. Tests directional visibility, ray filtering, collision policy, and editor selection.

22. **Time-Offset Portal Lab**

    A moving probe appears at the destination with a controlled delayed or earlier visual state. Useful later for testing temporal buffers, though it requires functionality beyond purely spatial transforms.

23. **Acoustic Maze**

    Rooms are visually close but spatially distant, while sound follows connected paths rather than Euclidean distance. Tests audio-space mapping, attenuation, direction, and obstruction.

24. **Shadow Around a Bend**

    A light outside one end of a curved tunnel illuminates an object beyond the bend. Tests whether lighting and shadow sightlines follow warped space instead of the embedded straight-line chord.

25. **Multi-Scale Object Loop**

    An object repeatedly crosses differently sized portals and should return to exactly its original position, orientation, velocity, and scale after one complete cycle. Tests transform reversibility and long-term drift.
