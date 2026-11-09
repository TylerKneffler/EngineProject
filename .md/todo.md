# Engine TODO

## Status

### Completed

- [x] Oak meadow distant foliage brightness regression narrowed to the texture LOD shading simplification path and the scene-level LOD settings were adjusted to preserve near/far consistency.
- [x] Intermediate large-oak canopy LOD work completed and published, with approved geometry and validation coverage retained.
- [x] Texture streaming, material-cost, and cold-stop profiling investigations completed with recorded evidence under `.md` and `.local` logs.
- [x] Screen-size mesh LOD and placement validation work completed and retained in the project history.

### Incomplete

- [ ] Validate the oak meadow visual fix in a clean rebuild and confirm no regression across backends.
- [ ] Re-run the oak scene bench and image comparison checks after the LOD shading fix.
- [ ] Review remaining large-file refactors if they become part of active work.

## `.cpp` files over 1000 LOC

1. `Engine/Core/Scene/Rendering/SceneRendering.cpp` — 8099
2. `Engine/Editor/EditorState.cpp` — 6319
3. `Engine/Editor/EditorStateAnimation.cpp` — 6111
4. `Engine/Core/Compoonents/Obj/TerrainManager.cpp` — 5251
5. `.local/tests/GrassPerformanceTests.cpp` — 3857
6. `Engine/Core/Compoonents/Obj/Mesh.cpp` — 2728
7. `Engine/Editor/Core/Gizmos/EditorGizmoSystem.cpp` — 2526
8. `Engine/Core/Assets/Scripts/TerrainGen/TerrainGen.cpp` — 2060
9. `Engine/Editor/UI/ImGui/ImGuiEditorUi.cpp` — 1863
10. `Engine/Editor/EditorStateSkeleton.cpp` — 1804
11. `Engine/Editor/Core/View/Views/AssetsExplorerView.cpp` — 1798
12. `Engine/Editor/Core/View/Views/PreferencesView.cpp` — 1643
13. `Engine/VideoExporter/VideoExportApplication.cpp` — 1507
14. `Engine/Editor/Core/View/Views/PerformanceView.cpp` — 1441
15. `.local/tests/OakBackendCapture.cpp` — 1379
16. `Engine/Core/Physics/Adapters/BulletPhysicsAdapter.cpp` — 1369
17. `Engine/Editor/Core/View/Templates/EditModes/EditToolsViewTemplate.cpp` — 1357
18. `Engine/Editor/Core/Importers/PmxModelDecoder.cpp` — 1356
19. `Engine/Core/Physics/RigidBody/RigidBodyPhysics.cpp` — 1310
20. `Engine/Core/Scene/Hierarchy/SceneHierarchy.cpp` — 1304
21. `Engine/Editor/Core/Importers/FbxModelDecoder.cpp` — 1300
22. `Engine/Core/Compoonents/Animation/AnimationManager.cpp` — 1275
23. `Engine/Core/component.cpp` — 1252
24. `.local/tests/TerrainStreamingPerformanceTests.cpp` — 1249
25. `Engine/Editor/EditorApplication.cpp` — 1157
26. `Engine/Editor/Core/View/Views/SceneView.cpp` — 1146
27. `Engine/Editor/Core/View/Views/Scene/ScenePlacementAndPicking.cpp` — 1100
28. `Engine/Core/Compoonents/Particles/ParticleEmitter.cpp` — 1077
29. `Engine/Core/Compoonents/Physics/SpatialManipulatorTraversal.cpp` — 1061
30. `Engine/Editor/UI/ImGui/Panels/ImGuiPanelHost.cpp` — 1023
31. `Engine/Editor/Core/Importers/MmdDanceAssetBinder.cpp` — 1010
32. `Engine/Editor/Core/Importers/ModelAssetWriter.cpp` — 1006

### Refactor guidance

- Break each oversized file into focused modules that match the current project layout: `Core/Scene/`, `Core/Components/`, `Core/Rendering/`, `Editor/Core/View/`, and `Editor/Core/Importers/`.
- Do not create generic `*Support`/`*Helpers` umbrella files; extract real responsibilities into dedicated classes and folders instead.
- For `SceneRendering.cpp`, create a dedicated scene-renderer subdirectory such as `Engine/Core/Scene/Rendering/Renderer/` or a feature-specific folder, and keep the orchestrating `SceneRenderer` thin while the extracted classes handle object batching, shadow setup, BRDF LUT generation, post-process prep, or visibility work.
- Target roughly `~1000 lines per file` and `~1000 lines per class` as a guideline, not a hard limit. If a class grows beyond that, split by responsibility rather than adding more helper utilities.
- Prefer small, cohesive classes over large monolithic controllers; each extracted type should own one responsibility such as scene setup, validation, import, UI, or simulation.
- Separate data/serialization logic from rendering/update logic so files remain easier to test and reason about.
- Move feature-specific logic into dedicated files/directories, not a catch-all support header. A class should have a clear domain name and purpose.
- Keep public APIs narrow and explicit; avoid hidden cross-coupling between editor state, rendering state, and scene serialization.
- Preserve the current project conventions: keep runtime systems under `Engine/Core`, keep editor tooling under `Engine/Editor`, and keep test-only diagnostics under `.local/tests` or project-local validation folders.
- Refactor in incremental slices: extract one responsibility at a time, update includes, and verify the affected build or focused tests before moving on.
- Prefer composition over giant inheritance trees; for example, split large editor/state files into state, UI, command, and validation subsystems.
- When a file is clearly mixing scene concerns with asset/mesh import or editor commands, split that logic into clearer domain-specific files/directories rather than creating more global helpers.

## Notes

- Stale numbering and duplicate ordering issues were removed.
- The oak meadow brightness issue is now tracked as an active visual-validation task rather than a hidden dependency chain.
- Historical work remains documented in `.md/todo_history.md` and the related design notes under `.md/`.