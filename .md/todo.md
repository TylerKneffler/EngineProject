# Engine roadmap

Last updated: 2026-10-15.

Status convention:

- `[x]` Complete
- `[ ]` Incomplete

## Next priorities

- [ ] **PMX model import:** Implement the staged PMX importer and Properties controls below.

## PMX remaining features

Implement new runtime behavior through the reusable components below; let the PMX decoder translate file-specific data into them.

### Bone evaluation

- [ ] Compare one MMD reference pose with the imported rig at each deform layer and record the first mismatch.
- [ ] Match animated local-axis and fixed-axis transforms on those reference poses.
- [ ] Verify external-parent targets and after-physics append/IK order with an authored test rig.
- [ ] Compare physics-driven bone feedback over repeated playback and reset.

### Soft bodies and materials

- [ ] Build rope soft-body topology from PMX material vertices and render its simulated deformation.
- [x] Map PMX soft-body cluster settings to Bullet and test a clustered fixture.
- [ ] Implement near-anchor behavior and two-way forces between soft bodies and dynamic rigid bodies.
  - [x] Use Bullet anchors for two-way force exchange; a synthetic PMX fixture verifies that cloth wind moves an anchored dynamic body.
  - [ ] Compare near-mode attachment and collision behavior with MMD, including playback reset and rigid-body recreation.
- [ ] Compare soft-body and rigid-body motion with MMD over playback and reset.
- [ ] Import custom and built-in toon ramps, then evaluate toon lighting in the material shader.
- [x] Import sphere textures and implement PMX multiply, additive, and additional-UV sphere modes.
- [ ] Render material edge outlines using imported color and width, including skinned meshes.
- [ ] Verify material morph effects on toon, sphere, and edge output with a render fixture.

The default `PmxModelDecoder` CTest runs synthetic fixtures (0.94 seconds in Debug on 2026-10-15); run `PmxModelDecoderTests.exe --bundled` for the full bundled-model import check.
