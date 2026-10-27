# Agent instructions

## Components and scene scripts

- Keep reusable mechanics in `Engine/Core/Compoonents` (the directory spelling is intentional). A component may appear in only one bundled scene and still be reusable; judge its behavior, not its use count.
- Put scene-specific timing, animation-to-IK transitions, object choreography, and demo triggers in `Engine/Core/Assets/Scripts`, grouped by feature.
- Portal and warp demo timing belongs in `Engine/Core/Assets/Scripts/Portals` and `Engine/Core/Assets/Scripts/Spatial`. Keep `SpatialManipulator` focused on reusable portal and warp mechanics.
- Preserve compatibility components such as `CameraTrack` when they maintain serialized scene behavior, even if only one bundled asset uses them.

## Tests

- Keep tests in `.local/tests`, with lowercase `tests`. Do not create `Engine/Tests`.
- `CMakeLists.txt` uses `ENGINE_LOCAL_TEST_ROOT` for optional regression tests. Guard new test targets with a source-file existence check so a checkout without ignored local tests still configures.
- `.local` is generally ignored. Add a narrow `.gitignore` exception when a test must be committed with a change.
