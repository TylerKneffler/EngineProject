# Architecture Refactor Plan: Scene → Object → Component

Source: architecture audit, 2026-11-17. Goal: keep the Scene → Object → Component
model the engine already uses, but move responsibilities out of the classes that
absorbed them, remove the Core → Editor dependency, and organize directories as
root → base classes → subclasses.

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done · `[?]` needs decision

---

## Audit summary

The core model is sound: `Scene` owns `Object`s; `Object` owns hierarchy plus a
`Component` list; `Component` has guarded lifecycle dispatch; `Script` is a
`Component`, so user code and engine code share one path. Problems are
concentrated in a few god classes and in directory sprawl.

### Classes with too many responsibilities

| Class | Size (approx.) | Responsibilities that don't belong |
|---|---|---|
| `Scene` (`Core/Scene/Scene.h`) | 1,455-line header, ~196 methods, 19 friend classes | Physics, audio, clock, particle budget, GPU resources/pipelines, shadow caches, perf stats, UI input, portal raycasts, editor selection, editor camera follow, editor weight paint |
| `Scene::Render` (`Core/Scene/Rendering/SceneRendering.cpp`) | ~2,857 lines in one function | Entire frame. The 19 `SceneRenderer*` friend classes reach into Scene private state instead of owning it |
| `EditorState` (`Editor/State/EditorState.h`) | 1,074-line header, ~247 methods, 20 .cpp files | Animation, timeline, rig UI, mesh editing, skeleton, lighting, import, documents, history, panels |
| `TerrainManager` (`Core/Components/Terrain/TerrainManager.h`) | ~111 methods, 14 .cpp files | Streaming, far-terrain cache/worker, colliders, sun shadow, serialization, editing, prefab instances: a subsystem posing as a component |
| `Mesh` (`Core/Components/Mesh/Mesh.h`) | ~134 methods, 10 .cpp files | Geometry, LOD, morph, pose, slicing, serialization, terrain |
| `Component` base (`Core/component.cpp`) | ~800 of 1,247 lines are inspector UI | `DrawProperties`, asset picker, reference picking, static editor state |
| `Object` (`Core/Object.h`) | ~half the header is prefab bookkeeping | Prefab snapshot/override caches, editor-only flags |

Pattern to avoid: splitting a class across `ClassTopic.cpp` files shortens the
files but leaves the class with every responsibility. Each split below must
produce a real collaborating class with its own state.

### Breaks from the Scene → Object → Component model

- Core → Editor dependency: 52 includes of `Engine/Editor/UI/IEditorUi.h` across ~70 Core component files.
- Editor state on runtime types: selection, preview object, 2D mode and camera follow on `Scene`; overlays and hidden flags on `Object`.
- Systems disguised as components: `TerrainManager`, `AnimationManager`, `SpriteAnimationManager`.
- `Transform` is both a `Component` subclass and a value member on `Object`. This is acceptable, but document it.

### Directory problems

- Base types loose in `Core/`: `Object.h`, `component.h`, `Script.h`, `SceneManager.h`, `ComponentReference.h`, `PropertyMacros.h`.
- Component implementations away from their headers: `RigidBody`/`Cloth` headers in `Components/Physics`, implementations in `Core/Physics/{RigidBody,Cloth}`.
- Rendering in 4 roots: `Graphics/`, `Renderers/`, `Rendering/`, `Scene/Rendering/`.
- Lighting in 3 places: `Components/Lighting`, `Rendering/Lighting`, `Scene/Lighting`.
- `Time/` and `Timing/` both exist.
- `Model/` is a grab bag of settings, asset formats and an algorithm (`MeshLodGenerator`).
- Stale serialization copies at `Serialization/Scene*SerializationBehavior.cpp` (excluded by CMake regex; diverged from live copies).
- Empty `Physics/World/`.
- `Components/Obj/` holds only LOD headers. `Animation/SceneCameraLightTrack` belongs in `Cinematics/`.
- `Editor/Core/` is confusing next to `Engine/Core`. Loose build files in `Editor/` root.

Accepted exceptions: small single-purpose dirs (`IO`, `Math`, `Memory`, `Input`,
`Startup`), app entry points (`Game/`, `Server/`, `VideoExporter/`),
`Assets/Scripts` (scene-specific behavior per `agents.md`).

---

## Phase 1: Cleanup (low risk)

- [x] Delete stale `Serialization/Scene{Document,Object,Prefab}SerializationBehavior.cpp` and remove the CMake exclusion regex.
- [x] Remove empty `Core/Physics/World/`.
- [x] Merge `Core/Timing/` into `Core/Time/` (`FramePacer.h`; namespace unchanged).
- [x] Move `RigidBody`/`Cloth` component implementations next to their headers:
  `Core/Physics/RigidBody/RigidBodyPhysics.cpp` → `Core/Components/Physics/RigidBody.cpp`,
  `Core/Physics/Cloth/ClothPhysics.cpp` → `Core/Components/Physics/Cloth.cpp`.
- [x] Rename `Compoonents` → `Components` (owner confirmed the spelling was not intentional). `git mv` plus include rewrite across 285 files. `agents.md` is updated.
  - **Breaking for external projects** that include `Core/Compoonents/...` paths directly; no forwarders were added for that directory.

## Phase 2: Base types at their roots

Target layout:

```
Core/Scene/        Scene.h, SceneManager.h, ComponentRegistry.h, Object.h
Core/Components/  Component.h, Script.h, ComponentReference.h, PropertyMacros.h
                   Transform.h, ...subclass dirs (Animation/, Mesh/, Physics/, ...)
```

- [x] Move `Object.{h,cpp}` and `SceneManager.{h,cpp}` into `Core/Scene/`.
- [x] Move `component.{h,cpp}` (renamed `Component.*`), `Script.h`, `ComponentReference.*`, `PropertyMacros.h` into the components root.
- [x] Rewrite all engine and `.local/tests` includes to the new paths (220 files).
- [x] Leave forwarding headers at the old `Core/*.h` paths so external projects keep compiling. **Follow-up:** delete them once projects have migrated.
- [x] `Components/Obj/` → `Components/Mesh/Lod/`.
- [x] `Animation/SceneCameraLightTrack` → `Cinematics/`.
- [ ] Remaining loose `Core/` files (`Project`, `ProjectLoader`, `AssetRecord`, `Window`) → decide on `Core/Project/` and `Core/Platform/` homes.

## Phase 3: Remove Core → Editor dependency and editor state from runtime types

- [x] Split a narrow `Engine::Core::IInspectorUi` (`Core/Inspection/IInspectorUi.h`) out of `IEditorUi`: the 37 widget methods component inspectors use, plus `InspectorColor` (`EditorUiColor` is now an alias of it). `IEditorUi` derives from it and keeps the 88 editor-only methods. `DrawProperties` and the picker helpers take `IInspectorUi&`. Core includes of `Editor/...` went from 52 to **0**.
  - **Breaking for external projects:** scripts overriding `DrawProperties(Engine::Editor::IEditorUi&)` must change to `DrawProperties(Engine::Core::IInspectorUi&)`. The in-repo scripts (`FoxRagdollBlend`, `TerrainGen`, `Rotate`) are updated.
- [x] Extract generic inspector code from `Component.cpp` into `Engine::Core::ComponentInspector` (`Core/Inspection/ComponentInspector.{h,cpp}`): the generic field drawer, reference and asset pickers, multi-edit, static session state and the Win32 file dialog. `Component.cpp` went from 1,247 to ~110 lines. `Component` keeps one-line delegates (`DrawProperties`, `DrawPropertiesMulti`, `DrawReferenceProperty`, `DrawAssetPathPicker`), so component overrides are unchanged. `m_multiEditTargets` and `IsEditorValueMixed` are gone from `Component`; field groups are exposed read-only through `FindEditorFieldMetadata`. Editor session calls moved from `Component::*Editor*` to `ComponentInspector::{SetAssetDirectory, ClearReferenceSearches, HasReferencePick, CancelReferencePick, CaptureReferencePick}`.
  - Follow-up: `DrawGeneric` still special-cases `Material` (the shader field) and `Mesh` (meshPath drops). Replace these with a per-field custom-drawer hook.
- [ ] Move editor selection, preview object, mesh-edit pose, weight paint, bone visibility, 2D mode and camera follow off `Scene` into an editor-side `EditorSceneContext`. The renderer reads it through a small overlay/debug-view struct passed into render.
- [ ] Move `editorHidden`/`editorOverlays` off `Object` into an editor-side map keyed by `ObjectId`.
- [ ] Extract prefab-instance bookkeeping from `Object` into `PrefabInstanceLink` (held by pointer, null for non-prefab objects).

## Phase 4: Scene renderer extraction (highest payoff)

- [ ] Introduce `SceneRenderer`, owned by `Scene`, holding all GPU resources, pipelines, shadow caches, occlusion pyramid, frame buffers and render stats currently on `Scene`.
- [ ] Give `SceneRenderer` a narrow read interface into the scene (objects, component registry, settings, snapshot) instead of 19 `friend` declarations.
- [ ] Break `Scene::Render` into passes: frame prep → visibility/LOD → shadows → opaque → particles → portals → post/UI. Target: no function over ~200 lines.
- [ ] Move render perf counters (`GetLast*Count`, grass perf, shadow timing) into a `RenderStats` struct owned by the renderer.
- [ ] Consolidate rendering directories:
  ```
  Core/Graphics/            RHI interfaces (IGraphicsContext, IShader, ...)
  Core/Graphics/Backends/   DX11/, DX12/, Vulkan/, RendererFactory
  Core/Rendering/           SceneRenderer + passes, Lighting/, Portal/, Sprites/, culling, LOD
  ```
  Move `PostProcess.cpp` out of `Graphics/`; move `ExportFrameSequence`, `HardwareSimulation`, `AutoQuality` to fitting homes (export / quality).
- [ ] Consolidate lighting: components stay in `Components/Lighting`; scene-side lighting code joins `Rendering/Lighting`.

## Phase 5: Split remaining god classes

- [ ] `TerrainManager`: keep a thin component; extract `TerrainStreamer`, `FarTerrainCache` (+ worker), `TerrainColliderBuilder`, `TerrainSunShadow`, `TerrainEditSession`, `TerrainPrefabPlacement`.
- [ ] `Mesh`: separate `MeshGeometry` data, `MeshLod`, `MeshMorphs`, `MeshSlicer` (editor/tool), serialization.
- [ ] `EditorState`: extract `AnimationEditor`, `MeshEditSession`, `SkeletonEditor`, `DocumentManager`, `EditorHistory`; `EditorState` becomes the coordinator.
- [ ] Decide on a scene-service/system layer for `AnimationManager` / `SpriteAnimationManager`, or keep them as components that delegate to system classes.
- [ ] `Model/` → `Assets/` (asset formats) + `Settings/` (project/scene/quality/video settings); `MeshLodGenerator` to mesh code.

## Phase 6: Editor directory tidy

- [ ] `Editor/Core/` → `Editor/Tools/`.
- [ ] `GameBuildManager`, `BuildTreeLock`, `CodeEditorLink` → `Editor/Build/` (alongside `VSCodeBridge/`).
- [ ] Large views to split when touched: `PreferencesView` (2,024), `EditorGizmoSystem` (1,959), `ImGuiEditorUi` (1,861), `AssetsExplorerView` (1,798).

---

## Rules for every phase

- Build `Engine`, `Editor` and `Game` (debug preset) after each step. Run the `.local/tests` regression targets that touch the moved code.
- One concern per commit; pure moves separate from behavior changes, so `git log --follow` works.
- Sources are found by `GLOB_RECURSE CONFIGURE_DEPENDS`, so moves need a re-configure but no CMake edits.
- Keep serialized type names and component field names unchanged: scenes and prefabs on disk must load without migration.
