# Agent instructions

## Components and scene scripts

- Keep reusable mechanics in `Engine/Core/Components`. A component may appear in only one bundled scene and still be reusable; judge its behavior, not its use count.
- Self-contained features built on the base systems (grass today) live in `Engine/Core/Features/<Name>`, with the feature's components, renderer helpers and stats together. Base systems (`Components`, `Rendering`) must not include `Features`; they reach a feature through hooks or registered type names. Register feature components in `SceneSerializerRegistry.cpp` (the Engine is a static library, so self-registering statics can be dropped by the linker).
- Content such as asset paths and tuned presets belongs to scene scripts, not engine code (for example `Scripts/Grass/MeadowGrass`, applied through `TerrainManager::SetObjectLayerSetup`).
- Put scene-specific timing, animation-to-IK transitions, object choreography, and demo triggers in `Engine/Core/Assets/Scripts`, grouped by feature.
- Portal and warp demo timing belongs in `Engine/Core/Assets/Scripts/Portals` and `Engine/Core/Assets/Scripts/Spatial`. Keep `SpatialManipulator` focused on reusable portal and warp mechanics.
- Preserve compatibility components such as `CameraTrack` when they maintain serialized scene behavior, even if only one bundled asset uses them.

## Tests

- Keep tests in `.local/tests`, with lowercase `tests`. Do not create `Engine/Tests`.
- `CMakeLists.txt` uses `ENGINE_LOCAL_TEST_ROOT` for optional regression tests. Guard new test targets with a source-file existence check so a checkout without ignored local tests still configures.
- `.local` is generally ignored. Add a narrow `.gitignore` exception when a test must be committed with a change.
