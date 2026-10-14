# Calibration materials

These three self-authored, tileable atlas textures fit the stock `Assets/Mesh/cube.obj` 3 x 2 UV layout. Each cube face receives one complete pattern.

- `calibration_checker.material`: eight divisions per face; compare apparent object size and spot distortion.
- `calibration_ruler.material`: 32 minor divisions, emphasized every eighth division, and face-local coordinates; compare relative positions.
- `calibration_direction.material`: asymmetric arrows and diagonals; track direction, rotation, and portal orientation.

The divisions are **relative to each mesh face**. A scaled cube does not preserve a fixed world-unit spacing, so these are visual guides rather than meter rulers. Recreate the PNG atlases with `python Engine/Core/Assets/Textures/Calibration/generate.py` and Pillow.

Design references: [Blender UV test grid](https://docs.blender.org/manual/id/3.1/modeling/meshes/uv/applying_image.html), [Unreal custom UV checker with grid lines and arrows](https://dev.epicgames.com/documentation/unreal-engine/making-your-first-texture-graph-in-unreal-engine), [Kenney CC0 prototype textures](https://kenney.nl/assets/prototype-textures), and [Screaming Brain Studios CC0 grid pack](https://opengameart.org/node/174673). The included graphics were drawn for this project; no third-party image was copied.
