# EngineProject feature list

This list describes the source tree as of October 22, 2026. **Current** means an implementation exists in this repository; it does not imply that every workflow is complete or production ready. **Planned** means a proposed addition, not a committed release or delivery date.

## Current features

### Engine and projects

- Windows x64 editor, standalone game, and headless scene runner.
- Project Hub, project templates, project settings, and portable Windows game export.
- Scene hierarchy, transforms, components, scene serialization, prefabs, and asset references.
- C++ gameplay scripts with editor hot reload and VS Code debugging integration.
- Editor Play mode, undo and redo, console, problems view, and performance view.

### Rendering and content

- DirectX 11, DirectX 12, and Vulkan renderers on Windows.
- Meshes, sprites, materials, cameras, lights, skyboxes, HDR environment lighting, and shadows.
- glTF/GLB, FBX, and MMD character import paths; engine-native mesh, material, rig, animation, and prefab assets.
- Skeletal animation editing and playback, clip crossfades and layers, skinning, morph targets, and IK tools.
- CPU and GPU particle systems, including fluid particle simulation tools.
- Portal and spatial warp rendering and traversal.
- Scene-specific procedural terrain generation and streaming demos.
- Video and frame-sequence export tools.

### Physics, audio, and UI

- Rigid bodies, primitive and mesh colliders, triggers, collision filtering, and continuous collision detection.
- Cloth simulation, ragdoll and bone physics integration.
- WAV, OGG, and MP3 playback; spatial audio, directional sources, and mixer buses.
- Retained screen UI with canvas layout, images, text, and buttons. Runtime text currently supports printable ASCII.

## Planned features

These are candidate priorities based on gaps in the current implementation. Their order is a suggested starting point and can change with project goals.

1. **Runtime save games:** a versioned API for saving and restoring game state and checkpoints, separate from editor scene authoring.
2. **Navigation and pathfinding:** navmesh generation, obstacle-aware path queries, and reusable navigation agent components.
3. **Internationalized runtime UI:** Unicode text shaping and glyph support, font fallback, and a localization asset workflow.
4. **Input actions and rebinding:** named actions, device mappings, and runtime remapping for keyboard, mouse, and controllers.
5. **Networking and multiplayer:** transport, connection lifecycle, replicated state, and client/server synchronization. The existing headless runner is a simulation tool, not a network server.
6. **Cross-platform support:** platform abstraction, builds, packaging, and testing beyond Windows x64.

## References

- [README](README.md) for supported platforms, setup, and the game loop.
- [Feature guide](.md/feature-guide.md) for detailed current behavior and limitations.
- [Roadmap notes](.md/todo.md) for ongoing implementation notes.
