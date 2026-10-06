# Engine roadmap

Last audited against the codebase: 2026-10-01.

Status convention:

- `[x]` Complete, implemented and present in the repository.
- `[ ]` Incomplete, not implemented or not complete enough to meet its acceptance criteria.

## Next priorities

### Mesh-collider contacts driving skinned bones

Current state: `fox_mesh_collider.scene` keeps the mesh collider selected during animation, the IK blend, and the final fall. `FoxRagdollBlend` stops IK body simulation at the dynamic-body handoff, while `AnimationManager::holdCurrentPoseWhenStopped` preserves the blended pose. The dynamic collider is a convex hull of the current pose. `MeshObjectCollider::GetContacts()` exposes a bounded snapshot of Bullet contacts, with nearest morphed/skinned surface position and interpolated palette weights when mapping succeeds. `Skeleton` now accumulates bounded contact bends on weighted bones; damping stops angular motion without restoring the previous pose. Animation or IK can supply a new base pose. This is a visual articulated response; it does not transfer momentum to bone rigid bodies or solve a coupled root/bone system. The separate `fox_ragdoll.scene` uses per-bone world contacts.

#### Collision representation and ownership

- [ ] Choose a collision representation that supplies contacts on or near the current skinned, morphed surface. The existing dynamic convex hull can contact at a point far from the visible concave mesh; Bullet cannot use the deforming concave triangle surface as an ordinary dynamic rigid body. Evaluate a surface contact proxy alongside the root body, or another supported deformable-body representation.
- [ ] Give the root body sole ownership of global translation, rotation, gravity, and floor response. Give the bone solver sole ownership of articulated pose. Keep per-bone primitive shapes out of external world contacts in mesh-collider mode, and prevent the two solvers from independently moving the same transform.
- [ ] Define behavior for self-intersection, multiple skinned meshes, missing or zero weights, missing bones, and moving colliders.

#### Contact data and skin-weight mapping

- [x] Expose a bounded latest-step snapshot from the manifolds already read in `PhysicsWorld::Step`: contact point, normal, separation, impulse, and other body.
- [ ] Add stable contact identity and substep accumulation/lifetime rules so a force consumer cannot apply the same impulse twice or miss an earlier fixed substep.
- [x] Map a contact to the nearest current morphed/skinned triangle and barycentric coordinates, rejecting convex-hull contacts beyond a configurable distance.
- [ ] Replace the linear triangle scan with a refittable query structure and define mapping behavior when several visible surface patches are equally close.
- [x] Interpolate and normalize both `joints0/weights0` and `joints1/weights1` from the expanded triangle stream into palette-index weights.
- [x] Resolve palette indices to available `AnimationBone` components in the contact snapshot; a missing bone remains null.
- [ ] Preserve source vertex/triangle identity for indexed meshes and diagnostic use.
- [ ] Keep CPU contact geometry consistent with the renderer's morph and skin order. Cache topology and update pose data only when morph weights or bone transforms change.

#### Coupled bone response

- [x] Add a bounded, weighted contact-to-bone pose response in `Skeleton`, with tunable strength, damping, and bend limit. Respect each `IKBone` joint's rotation limits; apply spring recovery only for a bone configured with a Spring joint.
- [ ] Upgrade the pose response to a coupled joint solver with physical mass, parent-chain limits, and root reaction accounting where required.
- [ ] Keep pose bones active after the animation handoff and anchor them to the root body. Solve constraints without the drift observed when the current pose-only IK bodies and dynamic mesh-collider body both have independent free motion.
- [x] Add damping that stops angular motion while retaining bounded contact deformation after contact ends. A Spring joint may recover toward its base pose.
- [ ] Refit or rebuild the mesh contact representation from the resulting bone and morph pose at a defined point in the fixed-step order. Avoid collider/render lag, repeated rigid-body recreation, and feedback oscillation.

#### Scene integration and verification

- [x] Keep reusable contact mapping in `Engine/Core/Compoonents/Physics/MeshObjectCollider`, reusable pose response in `Skeleton`, and fox-specific timing and triggers in `Engine/Core/Assets/Scripts/Physics`.
- [x] Expose response strength, damping, and maximum bend on `Skeleton`, and surface-distance threshold on `MeshObjectCollider`.
- [ ] Add debug overlays for contact points, mapped triangles, bone weights, and applied impulses. Show whether a contact was mapped or rejected.
- [ ] Remove the mesh-collider scene's final IK shutdown when the coupled solver is stable. Keep its mesh collider selected through animation, landing, and rest; preserve the per-bone ragdoll scene as a separate comparison.
- [ ] Test that a foreleg contact mainly bends its weighted foreleg chain and a tail contact mainly bends the tail, including a point influenced by multiple bones and a pose with an active morph target.
- [x] Test post-handoff weighted contacts produce bone pose motion and preserve the existing fox floor-clearance regression.
- [ ] Test no snap at animation-to-IK handoff, stable settling, and visible-mesh/contact-surface alignment throughout the fall with a documented floor-clearance tolerance. In a 30-second fixed-step run, the mesh-collider root moved about 0.0001 units over the last five seconds while its bones changed up to 0.038 radians; the per-bone ragdoll root moved 0.017 units, and its most mobile bone moved 0.069 units and rotated 0.423 radians. Stronger per-bone damping reduced the latter drift but did not eliminate it or fix visible floor overlap. Define a settle threshold and tune or fix the remaining motion before marking this complete.
- [ ] Test moving obstacles, rotated and scaled foxes, repeated play/reset, missing references, and the existing per-bone ragdoll behavior. Keep deterministic fixed-step regressions in `.local/tests` and measure collision-update time and allocations before enabling the feature by default.
- [ ] Fit the per-bone ragdoll's collision proxies to the visible skinned fox, or add a supported mesh-surface contact proxy. The IK bodies contact the floor with negligible manifold penetration, but the visible belly/head/tail can still pass below it because their primitive shapes do not cover the weighted mesh. Keep this separate from mesh-collider mode.

## Asset editing

### Mesh authoring

- [ ] Add a dedicated Mesh Edit stage with vertex, edge, and face selection; box/lasso selection; transforms with snapping and configurable pivots; extrude, inset, bevel, loop cut, weld, split, delete, duplicate, bridge, and fill operations; and undo/redo.
- [ ] Extend the existing basic UV preview/editor with per-face material-slot assignment, vertex-color painting, and overlays for normals, tangents, seams, non-manifold edges, and degenerate triangles.
- [ ] Share mesh vertex selection with skin-weight editing so geometry and influences operate on one authoritative asset rather than temporary copies.
- [ ] Add an authored-geometry transaction separate from runtime `SetDeformedVertices`. It must validate topology and attributes, persist indexed native mesh data, update bounds/GPU resources and asset revisions, atomically save, refresh dependents, and retain undo data. This is the canonical mesh validation and persistence task.

### Skeleton and skin authoring

- [ ] Add a dedicated rig editor for inserting, deleting, reparenting, and reordering bones as one validated, undoable operation. Update Skeleton joint order, inverse-bind matrices, AnimationBone palette/parent indices, Model node bindings, mesh influences, and animation channels together.
- [ ] Replace fragile child-index Model node paths with stable imported node identities, or provide an explicit remapping transaction for every hierarchy edit. Inserting, deleting, or reparenting must never silently make an existing node index resolve to another object.
- [ ] Add vertex/influence selection and weight painting with normalization, locked influences, mirroring, maximum-influence enforcement, visualization, undo/redo, and persistence to the native mesh asset.
- [ ] Add explicit temporary-pose and rest-pose editing controls, including mirrored transforms and clear/apply-pose commands.
- [x] Apply Rest Pose validates every joint transform before rebuilding inverse-bind matrices.
- [ ] Validate all affected skinned-mesh bindings and make Apply Rest Pose an atomic undoable transaction that leaves every dependent mesh valid on failure.
- [ ] Add a timeline/curve editor for creating, deleting, and retargeting bone tracks instead of exposing imported animation channels as read-only statistics.
- [ ] Add stage-specific IK constraint authoring.

## Deferred feature gates

- [ ]: Point-light shadows. Start after the texture-array/comparison-sampler foundation and point-shadow budget, radial-depth, and regression prerequisites are complete.
- [ ]: Dynamic diffuse GI. Reconsider after static irradiance probes have visibility/leak handling and diagnostics, and explicit GPU budgets are established.
