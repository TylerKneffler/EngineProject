# Splines and procedural geometry

The engine already contained `SplinePath`, `SplineFollower`, and `CameraTrack`.
`SplinePath` uses ordered direct child transforms as world-space control points,
with linear or Catmull–Rom interpolation, adjustable tension, open/closed paths,
and an arc-length lookup table for approximately equal-distance sampling.
Followers provide timing, easing, looping, ping-pong and orientation. Camera
tracks reuse followers. These components previously did not generate roads,
fences, pipes, or deformed mesh sections.

## Research

Splines separate the path from its consumers. In level construction, the main
consumers are extrusion, repeated rigid objects, and mesh deformation. Unity
separates [instantiation and extrusion](https://unity.com/blog/engine-platform/building-better-paths-with-splines-in-2022-2),
and its [extrusion documentation](https://docs.unity3d.com/Packages/com.unity.splines@2.8/manual/extrude-mesh.html)
shows generating a mesh along a curve. Unreal's
[Spline Mesh Component](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/USplineMeshComponent)
deforms an existing static mesh using a spline segment, with explicit forward
axis, boundaries and an up direction. This engine reuses its existing path and
adds these geometry consumers in `SplineGeometry`.

Distance-based placement avoids bunching copies where control points are close.
A moving orientation frame defines right/up/forward along a 3D curve. The new
component transports that frame between samples to avoid world-up flips near
vertical sections, and distributes residual twist around a closed loop.

## Using SplineGeometry

1. Add `SplinePath` and `SplineGeometry` to an empty scene object through Add Component.
2. In SplinePath, use **Add Control Point**, then move the child points using their
   normal transform gizmos. Hierarchy order controls traversal. Generated runtime
   children are excluded from the path.
3. Select a geometry mode in SplineGeometry. An empty **Path Object** uses the path
   on the same object; other geometry objects can share a named path.
4. Add a `Material` to the geometry owner to set its appearance.
5. Move the points or change the settings. The generated mesh refreshes in the
   scene view and during rendering. **Rebuild Spline Geometry** forces a refresh.

| Mode | Purpose | Controls |
| --- | --- | --- |
| Road | Continuous strip following the curve | Width, Segment Length, Meters per UV Repeat |
| Pipe | Circular tube, optionally capped on open paths | Radius, Radial Segments, Segment Length, UV repeat |
| Repeat Mesh | Rigid copies, such as posts or modular fence panels | Source Mesh, Forward Axis, Spacing, Mesh Scale |
| Bend Mesh | Repeated source sections bent along the curve | Source Mesh, Forward Axis, Section Length, cross-section scale |

Open `Engine/Core/Assets/Scenes/Splines/spline_geometry_demo.scene` for a shared
curved road, repeated fence posts, bent rail, and pipe. The included rail mesh has
16 subdivisions along its two-unit Z length so it bends smoothly.

All distances are in world units, including width, radius, section length and
spacing. Offset is expressed in the moving frame's right/up/forward directions.
The initial Up vector sets the initial orientation; subsequent frames are
transported along the curve. Nonuniform owner scale is handled by mapping output
back into the owner's local space, including normals and tangents.

**Fit Whole Sections to Path** rounds the requested section count and adjusts
spacing to divide the path length evenly. Bend Mesh stretches each section along
its forward axis to meet the next section. Turning fit off preserves the requested
section length and omits an incomplete final bent section. Rigid copies start at
distance zero and repeat before the end of the path, without a duplicate at a
closed seam. Their original length is controlled by Mesh Scale, so rigid panels
can overlap or leave gaps around bends; use Bend Mesh for continuous rails.

Forward axes map source cross-sections cyclically:

| Forward | Source right | Source up |
| --- | --- | --- |
| X | Y | Z |
| Y | Z | X |
| Z | X | Y |

For repeated/bent meshes, the minimum source forward coordinate anchors the start
of each section. Cross-section coordinates retain the source origin. Bend Mesh
stretches the forward extent to the section length; its forward Mesh Scale value
therefore only affects rigid repetition. Source UVs and vertex colors are retained;
procedural road/pipe UV V measures distance divided by Meters per UV Repeat.

## Lifecycle, checks, and current limits

`SplineGeometry` is registered for scene loading and Add Component. It creates one
runtime-only mesh child and derives its material from the owner. Generated geometry
is not saved; settings and control points are saved and rebuild it after loading.
Removing the component queues removal of its generated child. The render preparation
boundary refreshes geometry on settings, point transforms/order, owner transform,
source file timestamp or material changes. CPU callers can use `BuildGeometry`
without a graphics provider, or `PrepareGeometry(nullptr)` to publish a headless mesh.

The CPU build has an 8,192-segment and 1,500,000-vertex budget. Invalid dimensions,
missing assets/paths, singular transforms and zero-length paths report an inspector
message and hide stale output. Repeated copies currently combine into a single mesh;
this is intended for moderate construction runs, not millions of instanced objects.

This version does not create junctions/intersections, terrain carving or snapping,
lanes, banking/roll keys, automatic collision or LODs. Roads are surface strips with
no thickness. Source meshes use a single owner material; prefab hierarchies and
multiple source materials are not imported. Bent meshes need sufficient longitudinal
subdivisions in the source asset. Tight curves can fold wide profiles; sharp corners,
cusps and self-intersections require authoring adjustments. Arc lengths and curved
geometry are sampled approximations controlled by path sample count and Segment Length.

Regression checks cover road width/UVs, pipe winding/caps, closed seams, vertical
sections, rigid spacing, fitted bent joins, nonuniform transforms, output reuse,
control-point filtering, serialization and invalid/budget-limited input.
