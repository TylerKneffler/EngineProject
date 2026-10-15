# PMX near-anchor comparison

## Reference boundary

The PMX 2.1 reference records Near as a per-anchor byte (0 or 1) and
recommends `btSoftBody::appendAnchor` for the rigid-body connection. It does
not specify how MikuMikuDance maps Near to Bullet's arguments. Bullet's third
`appendAnchor` argument is `disableCollisionBetweenLinkedBodies`; when true,
Bullet adds that rigid body to the soft body's collision-disabled list. These
facts make the engine's `appendAnchor(node, body, nearMode)` mapping plausible,
but they do not establish MMD parity.

Sources: [PMX Editor 2.1 reference](https://gist.github.com/FlandreDaisuki/90ae5abf3138a15994526b6bfec73c2c),
[Bullet `appendAnchor` implementation](https://pybullet.org/Bullet/BulletFull/btSoftBody_8cpp_source.html).

## Engine behavior

- The PMX decoder accepts Near values 0 and 1. The asset writer preserves the
  value in `Cloth::Anchor::nearMode`.
- `Cloth::BindAnchors` passes Near to Bullet's collision-disable argument.
  The anchor itself remains attached for either value and can exchange force
  with a dynamic rigid body.
- A full physics reset destroys the soft body and rigid body; the next step
  creates both and binds the anchor again. A rigid-body configuration rebuild
  increments its generation; `BindAnchors` detects that change, recreates the
  soft body, and binds the anchor to the new body.
- `MmdDanceReplay::Restart` resets body and facial animation clocks and applies
  the opening pose. It does not reset the physics world, so a dance loop
  carries rigid-body velocities and soft-body deformation into the next loop.

The synthetic PMX regression checks anchor survival after a full physics reset
and rigid-body rebuild. It does not measure Near-on/off contact response against
MMD. A reference capture from MMD using the same PMX fixture, with Near off and
on, is needed to resolve the collision mapping and playback reset comparison.
