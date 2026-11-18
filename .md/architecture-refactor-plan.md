# Architecture Refactor Plan: Scene → Object → Component

Source: architecture audit, 2026-11-17. Goal: keep the Scene → Object → Component
model the engine already uses, but move responsibilities out of the classes that
absorbed them, remove the Core → Editor dependency, and organize directories as
root → base classes → subclasses.

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done · `[?]` needs decision

## Status (2026-11-17)

**Done:** every phase (1–6). `Scene` holds object memory, the update index and
structure events, plus three services by decision (physics world,
`SceneRenderer`, `SpatialSystem`). `TerrainManager`, `Mesh` and `EditorState`
are split into collaborators; directories follow root → base → subclass.
Work is on branch `architecture-refactor` (worktree
`c:/repos/EngineProject-refactor`), not merged into `main` yet.

| Commit | Step |
|---|---|
| `02ca6ee` | Phases 1–2, Phase 3 inspector split (`IInspectorUi`, `ComponentInspector`), `Compoonents` → `Components` |
| `e970af8` | 4.1 Editor view state out of `Scene` (`SceneViewOverlays`, `ISceneObserver`, `Editor/State/EditorViewState`) |
| `26c18e7` | 4.2a Lifecycle driver → `Engine::Runtime::Game` |
| `09090d7` | 4.2b Audio → `Game` |
| `0ec265c` | 4.2c `SceneManager` folded into `Game`; physics world stays with `Scene` |
| `b377f25` | 4.3 Strict cascade for generic lifecycle dispatch |
| `266f76b` | 4.4 Render state and passes → `SceneRenderer` |
| `a5355ac` | 4.5 `Render` split into named passes over `RenderView` |
| `3115c06` | 4.6 Spatial mapping and portal tracing → `SpatialSystem` |
| `13c9908` | 4.7 Rendering directory reorganization |
| `af3b7cd` | Phase 2 loose `Core/` files → `Core/Project`, `Core/Platform` |
| `f91a14f` | Phase 5 `Model/` → `Settings/`, `AssetFormats/`, data types |
| `657533b` | Phase 6 `Editor/Core` → `Editor/Tools`, `Editor/Build` |
| `878930f` | Phase 3 `PrefabInstanceLink` |
| `b985c9c` | Phase 3 inspector field hooks (no `Material`/`Mesh` special cases) |
| `49e53a2`…`3130458` | Phase 5 `TerrainManager` → `TerrainSunShadow`, `TerrainColliderBuilder`, `TerrainPrefabPlacement`, `FarTerrainCache`, `TerrainEditSession`, `TerrainStreamer` |
| `a157bb2`…`aed738a` | Phase 5 `Mesh` → `MeshMorphs`, `MeshRuntimeLods`, `MeshSlicer` |
| `0878a72`…`1f15152` | Phase 5 `EditorState` → `EditorHistory`, `MeshEditor`, `SkeletonEditor`, `DocumentManager`, `AnimationEditor` |

**How 4.2–4.7 were verified:** all hosts (Engine, Editor, Game, Server,
VideoExporter) build. Headless Server output is bit-identical to the
pre-refactor build on 6 scenes × 180 frames (physics, ragdoll, portals,
scripts), and wall time is unchanged within noise. From 4.4 on: 9 scenes,
`.local/tests` (18 tests including the six `GameStandalone_*` GPU runs), and
`GameBackendCapture` frames on 11 scenes (DX11/DX12/Vulkan) compared pixel by
pixel with a build of the pre-refactor commit; deterministic scenes match exactly.

**Still needs a manual check (UI and runtime paths not covered headlessly):**
- Editor: Properties panel (texture path edit/Browse/Apply, asset drag-drop, dragging a Mesh header onto a collider's meshPath, Material shader field, reference "Pick", multi-select edit), hierarchy and edit-tools overlay toggles, terrain placement selection outline, deleting objects that have overlays.
- Editor play mode: play → stop → play, and a scene transition during play.
- Animation editor (after `AnimationEditor`): open an animation from an asset/object/clip, playback, timeline scrubbing, add/copy/paste keys, bake limbs, rig toolbar and constraints, pose library, save and reopen, animation undo/redo, closing with unsaved changes.
- Documents (after `DocumentManager`): open/close/save a prefab stage, model/prefab scene-asset documents, material/texture asset documents; dirty titles; closing with unsaved changes; editor shutdown with documents open.
- Skeleton edit mode (after `SkeletonEditor`): bone add/remove/parent, skin binding, weight paint with undo/redo interleaved with scene undo, bind pose refresh.
- Mesh edit mode (after `MeshEditor`): enter/exit, vertex paint and texture paint strokes with undo/redo, mesh edits and save, prefab and scene-asset document sessions.
- Undo/redo (after `EditorHistory`): scene edits, selection restore, prefab document, scene-asset documents, skeleton binding edits, history limit in Preferences, dirty flag after save.
- Prefabs (after `PrefabInstanceLink`): override marking in Hierarchy/Properties, Apply/Revert, Unpack, saving a scene with modified prefab instances.
- Game exe: main-menu Play button (async scene transition), audio playback.

**Known issues found during the refactor (not caused by it):**
- [x] Fixed in `1ff91e5`: black sky on DX11/DX12 (stale `float4x4 invVP` at the top of the `Skybox.hlsl` cbuffer shifted every field), sky lighting never built (the environment map needed CPU pixels the texture had already dropped after upload), and reflective surfaces rendering flat white (the split-sum BRDF LUT summed its samples without averaging, lost in `f4bd6fe`). Checked in `hdri_showcase`, landscape, playable, portal and lighting captures on DX12/DX11/Vulkan.
- DX11 frames are colour-shifted against DX12/Vulkan (hazier sky, more saturated ground, e.g. ground 107,167,24 vs 94,138,41). The same shift is in the pre-refactor baseline, so it is in the DX11 frame path, not the skybox. Not investigated yet.
- `.local/tests/TerrainCapsuleTraversalTests.cpp` does not build: besides the old `Core/Components/Obj/TerrainManager.h` include (fixed in the worktree copy), it calls `TerrainManager::ApplyBrush` and `Scene::Start/Update`, which main no longer has either.
- `TerrainPlacementEditingTests` fails at "scene load failed": it loads `oak_meadow.scene`, deleted in `cda4fcd`.
- `skinned_soft_tissue_demo` segfaults intermittently in the headless Server, both before and after the refactor.
- The plain `Debug` configuration can't build `bc7enc` (`/O2` with `/RTC1`). The `devoptimized` preset (RelWithDebInfo) works.

**Breaking changes for external projects / scripts:**
- Include paths: `Core/Compoonents/...` → `Core/Components/...` (no forwarders). `Core/Object.h`, `Core/component.h`, `Core/Script.h`, `Core/ComponentReference.h`, `Core/PropertyMacros.h` still forward.
- `DrawProperties(Engine::Editor::IEditorUi&)` → `DrawProperties(Engine::Core::IInspectorUi&)`.
- `Engine::Core::SceneManager` removed → `Owner->GetScene()->GetGame()->RequestSceneLoad(path)` / `LoadScene` / `RequestDefaultSceneLoad`.
- `Scene::Start/Update/UpdateFixedFrame/SetFixedTimeStep/GetAudio/GetLastUpdateTimings` → `Engine::Runtime::Game`. `GetDeltaTime/GetClock/HasStarted` remain on `Scene`.

**Breaking changes since Phase 4 (details in each step below):**
- Render calls go through `scene.GetRenderer()`; spatial queries through `scene.GetSpatial()`; nested render/spatial types moved (`Renderer::SceneRenderer::PrepareTimings`, `Engine::Scene::SpatialQueryDomain`, ...).
- Moved include paths (no forwarders): `Core/Renderers/*` → `Core/Graphics/{,Backends/}`, `Core/Scene/Rendering/*` → `Core/Rendering/SceneRenderer*`, `UIRenderer`, `PostProcess`, `ExportFrameSequence`, quality headers, `Core/Project.h`/`ProjectLoader.h`/`AssetRecord.h`/`Window.h`, `Core/Model/*` settings and asset formats, `Editor/Core/*` → `Editor/Tools/*`.
- `Object` prefab fields → `GetPrefab()`, `SetPrefabAsset()`, `ClearPrefab()`, `PrefabLink().…`.
- `TerrainManager` far-terrain and edit APIs → `GetFarTerrain()` / `GetEditing()`.
- `Mesh::SliceByPlane` → `MeshSlicer::SliceByPlane`.

**Next:** the manual checks above (editor UI paths have no automated
coverage), then merge `architecture-refactor` into `main`. Follow-ups that
need an owner decision: delete the Phase 2 forwarding headers in `Core/`
once external projects have migrated; investigate the DX11 colour shift.

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
- [x] Remaining loose `Core/` files: `Project`, `ProjectLoader`, `AssetRecord` → `Core/Project/`; `Window` → `Core/Platform/`. Pure moves, no forwarders (**breaking** for external includes of `Core/Project.h`, `Core/ProjectLoader.h`, `Core/AssetRecord.h`, `Core/Window.h`). `Core/` now holds only subdirectories and the five Phase 2 forwarding headers.

## Phase 3: Remove Core → Editor dependency and editor state from runtime types

- [x] Split a narrow `Engine::Core::IInspectorUi` (`Core/Inspection/IInspectorUi.h`) out of `IEditorUi`: the 37 widget methods component inspectors use, plus `InspectorColor` (`EditorUiColor` is now an alias of it). `IEditorUi` derives from it and keeps the 88 editor-only methods. `DrawProperties` and the picker helpers take `IInspectorUi&`. Core includes of `Editor/...` went from 52 to **0**.
  - **Breaking for external projects:** scripts overriding `DrawProperties(Engine::Editor::IEditorUi&)` must change to `DrawProperties(Engine::Core::IInspectorUi&)`. The in-repo scripts (`FoxRagdollBlend`, `TerrainGen`, `Rotate`) are updated.
- [x] Extract generic inspector code from `Component.cpp` into `Engine::Core::ComponentInspector` (`Core/Inspection/ComponentInspector.{h,cpp}`): the generic field drawer, reference and asset pickers, multi-edit, static session state and the Win32 file dialog. `Component.cpp` went from 1,247 to ~110 lines. `Component` keeps one-line delegates (`DrawProperties`, `DrawPropertiesMulti`, `DrawReferenceProperty`, `DrawAssetPathPicker`), so component overrides are unchanged. `m_multiEditTargets` and `IsEditorValueMixed` are gone from `Component`; field groups are exposed read-only through `FindEditorFieldMetadata`. Editor session calls moved from `Component::*Editor*` to `ComponentInspector::{SetAssetDirectory, ClearReferenceSearches, HasReferencePick, CancelReferencePick, CaptureReferencePick}`.
  - [x] Follow-up done: `DrawGeneric` no longer names `Material` or `Mesh`. `Component::DrawEditorField(ui, key, value, edited, changed)` lets a component draw one serialized field itself (`Material` draws `shader` with `DrawShaderProperties`), and `Component::GetDragAssetPath()` lets a dragged component header fill a compatible path field (`Mesh` returns its file; `meshPath` fields still accept any mesh source). `ComponentInspector.cpp` no longer includes `Material.h`/`Mesh.h`.
- [x] Move editor selection, preview object, mesh-edit pose, weight paint, bone visibility, 2D mode and camera follow off `Scene` (finished by Phase 4, step 4.1; 2D mode stays as scene settings state).
  - [x] **Step A:** consolidated the 14 editor fields into `Engine::Scene::EditorViewState` (`Core/Scene/EditorViewState.{h,cpp}`), held by `Scene` as `m_editorView`, with the terrain-placement selection logic moved into `EditorViewState::SelectObject`. The renderer (11 files) reads `m_editorView.*`. `Scene`'s existing accessors forward to it, and `Scene::GetEditorView()` exposes it. `Scene.h` no longer includes `TerrainPlacementHandle.h`.
  - [x] **Step B:** deleted 10 forwarding accessors from `Scene` (`Set/GetSelectedObject`, `Set/GetSelectedPlacementId`, `Set/GetEditorSelectedMesh`, `SetEditorBonesVisible`, `AreEditorBonesVisible`, `SetPreviewObject`, `IsEditorMode2D`). The compiler identified each Scene call site, so same-named methods on `HierarchyView`/`PropertiesView` were left alone. 108 sites in 23 editor files plus `.local/tests/TerrainPlacementEditingTests.cpp` now use `GetEditorView()`. Four methods stay on `Scene` because they do real scene work: `SetEditorMeshEditPose`/`IsEditorMeshEditPose` and `SetEditorWeightPaint` mark the prepared render frame stale, and `SetEditorMode2D` reconfigures graphics.
  - [x] **Step C:** superseded and done by Phase 4, step 4.1 (editor state left `Scene` entirely).
- [x] Move `editorOverlays` (and its flag enum) off `Object`. **Correction:** it went into `Scene`'s `EditorViewState`, which gave `Scene` new editor bookkeeping. Phase 4, step 4.1 moves it into the editor.
- [x] Keep `editorHidden` on `Object` (deliberate exception). `IsEnabledInHierarchy()` checks it on every ancestor in update, render and pick loops, so moving it to a map would put hash lookups in those hot paths.
- [x] Extract prefab-instance bookkeeping from `Object` into `Engine::Prefab::PrefabInstanceLink` (`Core/Prefab/PrefabInstanceLink.h`): asset, source snapshot, override cache and source transform. `Object` holds it by `unique_ptr`, allocated on first use (`PrefabLink()`), so scene-only objects carry one null pointer instead of a string, a shared pointer, three vectors and five flags. Accessors: `GetPrefab()`, `SetPrefabAsset()`, `ClearPrefab()` (was `Prefab.reset()`), `GetPrefabLink()`; the snapshot helpers are unchanged. **Breaking for external projects** using the public fields: `obj->Prefab` → `obj->GetPrefab()`, `obj->PrefabOverrideCacheValid` → `obj->PrefabLink().overrideCacheValid` (likewise `hasOverrides`, `sourcePosition`, ...). Server output unchanged on the prefab scenes; prefab override save/revert in the editor is added to the manual-check list.

## Phase 4: Scene is the object memory manager

**Definition (owner-confirmed, revised 2026-11-17):** `Scene` manages **object
memory**: creating, storing, identifying, parenting, moving and destroying
`Object`s, publishing structural changes, and indexing which objects need
ticking. The **lifecycle driver** lives in `Engine::Runtime::Game`, which uses
scenes. `Object` and `Component` keep their lifecycle **methods** and cascade.

### Target ownership

```
Core/Runtime/Game             the simulation, shared by the exe and editor play mode
├── Scene(s)                  object memory (below); Game absorbs SceneManager
│                             (active scene, transitions)
├── SimulationClock           delta time, fixed step, frame index
├── Physics, Audio            moved out of Scene
├── lifecycle driver          Start → Update → fixed/physics → late, parallel
│                             scheduling, update timings
│                             Game → Scene's update index → Object → Component
└── SpatialSystem             portal/warp mapping and raycasts

Scene                         objects, ids, hierarchy, add/remove/move, queued
                              mutations, generation/snapshot, component registry,
                              update index, structure events (ISceneObserver)
Object / Component            lifecycle methods + cascade (Object → its
                              components → child objects)

SceneRenderer (Core/Rendering) reads a Scene; owns all GPU state; takes optional
                              SceneViewOverlays (render input) per view.

Engine/Game (exe)             GameApplication: window, input, renderer, pacing; owns a Game
Engine/Editor                 edit mode: scenes only, no Game ticking.
                              play mode: a Game over a copy of the scene (same
                              loop as the exe). Owns EditorViewState per scene.
```

**Lifecycle: methods vs driver.** The methods (`Start`/`Update`/`Enabled`/
`Disabled`/`OnDestroy`) and their cascade stay on `Object`/`Component`. The
driver (when to tick, fixed step, play/pause, parallel scheduling, ordering
against physics and audio) is host policy, so it lives in `Game`. Running the
exe and editor play mode through the same `Game` stops their loops drifting
apart. Edit mode doesn't create a `Game`.

**Strict cascade.** `Game`/`Scene` talk only to `Object`s; only `Object`s talk to
their `Component`s. Two current bypasses get fixed: worker-thread updates (Scene
reads `object->Components` to build scheduler nodes, which becomes
`Object::CollectWorkerUpdates`) and removal (Scene calls
`component->DispatchDestroy()`, which becomes an `Object` method).

**Service access:** `Object::GetScene()->GetGame()` (non-owning), so the ~40
files using `GetScene()->GetPhysics()` / `GetDeltaTime()` keep a direct path.

### Inventory (2026-11-17): ~230 `Scene` members

| Group | ~Members | Destination |
|---|---|---|
| Object memory: objects, pending add/remove, ids, queued mutations, generation, snapshot, registry, structure revision | ~20 | **stays in Scene** |
| Render: pipelines (~60), buffers/mapped ptrs, shadow/occlusion/cluster caches, material shaders, frame items, LOD/instancing, UI renderer, lighting pipelines, skybox/environment, render stats | ~190 | `SceneRenderer` |
| Physics, audio, particle budget | 3 | `Game` |
| Clock, delta time, lifecycle driver (`Start`/`Update`/`UpdateFixedFrame`), update timings | ~10 | `Game` |
| Update registry (index of objects to tick), lifecycle revision | ~5 | **stays in Scene** (memory index) |
| Graphics-preparation queue, prepare timings | ~6 | `SceneRenderer` |
| Spatial update scheduler | 1 | `SpatialSystem` (in `Game`) |
| Grass/perf heat stats, diagnostics | ~6 | `SceneRenderer` (RenderStats) |
| Gameplay/UI input routing | 3 | `Game` (input) |
| Editor view state | 1 (`m_editorView`) | Editor |

### Steps (build + `.local/tests` after each)

- [x] **4.1 Editor state out.** `Core/Rendering/SceneViewOverlays.h` is render input (selection, placement, edit mesh, pose, weight paint, preview), bound with `Scene::SetViewOverlays` (null = defaults). `Core/Scene/SceneObserver.h` (`ISceneObserver`) carries the removing/clearing/destroying events; `Scene` no longer clears editor fields itself. `EditorViewState` moves to `Engine/Editor/State`: one per scene, it binds overlays, observes its scene, and owns selection, the overlays map, bones visible, and camera focus/follow. `SetEditorMode2D` turned out to be scene/settings state (called on load and in `Init`, games included), so it stays as `Scene::SetMode2D`/`IsMode2D`. `editorCamera` is the scene's fallback viewer (render, audio listener, scripts) and stays until 4.4. **Done:** 195 editor call sites in 27 files now use `Engine::Editor::EditorViewState::For(scene)`. `Scene` has no editor members or editor methods left; `AdvanceEditorPreviews` moves out in 4.2 and `includeEditorVisuals` in 4.4. Clearing a scene now also stops camera follow, which previously kept a dangling target. `TerrainPlacementEditingTests` also compiles `EditorViewState.cpp`.
- [x] **4.2 `Game` runtime** (`Engine::Runtime::Game`, `Core/Runtime/Game.{h,cpp}`), in three steps:
  - [x] **4.2a Lifecycle driver.** `Game(Scene&)` uses the scene, binds itself (`Scene::GetGame()`), owns the `SimulationClock`, and runs `Start`, `Update` (the 8-phase job graph, unchanged), `UpdateFixedFrame`, `AdvanceManualFrame`, fixed/variable step and update timings. `Scene` lost `Start/Update/UpdateFixedFrame/AdvanceManualFrame/SetFixedTimeStep/UseVariableTimeStep/GetLastUpdateTimings/AdvanceEditorPreviews` and the clock. It keeps a small, documented simulation-step support API (`BeginUpdatePass`/`EndUpdatePass`, `GetUpdateObjects`, `PrepareTransformHierarchy`, `ApplyPoseConstraints`, `EnforceParticleBudget`) plus read-throughs: `GetDeltaTime/GetClock/GetFrameIndex/GetElapsedTime` read the bound Game (zero/stopped without one), and `HasStarted` is set by `Game::Start` so objects added to a running scene still start on commit. Hosts: `GameApplication`, `ServerApplication`, `VideoExportApplication`, editor play mode (a `Game` per play session, destroyed before the edit scene is restored), `EditorPlayPipeline`, `ExportFrameSequence`, `SceneManager` (restarts loaded scenes through the bound Game; none in edit mode), the failed-load restore in `ScenePersistence`, profilers/benchmark, and tests. `AdvanceEditorPreviews` moved to `Engine/Editor/Runtime/EditorPreviews`. `SetUpdateProfilingEnabled` stays on `Scene` for now because the renderer uses the same flag for its timings (moves in 4.4).
  - [x] **4.2b Audio → `Game`.** `Game` owns `Engine::Audio::Audio`: it updates the listener and drains the mixer each frame, stops sources when the scene clears (it observes the scene as an `ISceneObserver`) and when the Game ends, and handles `SynchronizeToTime` for the exporter. `Scene` lost `m_audio`/`GetAudio`. Server output is still bit-identical.
  - [x] **Physics world stays with `Scene` (owner decision); `Game` steps it.** The world is the scene's collision representation and is used without a running simulation: `GroundedFootIK::FindGround`/`ChainIK::ApplyGroup` raycast during edit-mode pose evaluation, `ParticleEmitter::ResolveCollisions` runs in edit-mode previews, and `~RigidBody`/`~Cloth`/`Disabled`/`SpatialManipulator::ClearSpatialWarpState` tear down bodies when the editor deletes objects. `Game::Update` does prepare/simulate/finish. The step settings (`ConfigureFixedStep`/`ConfigureStepBudget`) also stay on the world: they are stored by the Bullet adapter with its accumulator and substep state, and the editor sets the budget once on the scene.
  - [x] **4.2c `Game` absorbs `SceneManager`.** Scene transitions are per-Game: `LoadScene`, `RequestSceneLoad`, `RequestDefaultSceneLoad`, `LoadSceneAsync`, `ProcessPendingSceneLoad`, `CancelPendingSceneLoad`, default scene path and the loaded callback. Loads replace the Game's scene in place and restart it. The async reader moved to `Core/Runtime/AsyncSceneReader.h` (one worker per Game, joined in `~Game`). Deleted `Core/Scene/SceneManager.{h,cpp}` and the `Core/SceneManager.h` forwarder. **Breaking for external projects** that call `Engine::Core::SceneManager`; use `Owner->GetScene()->GetGame()->RequestSceneLoad(path)`. `MainMenuGameManager` is updated. `Game` does not take *ownership* of its scene: loads happen in place on the same `Scene`, so hosts keep owning scenes and nothing needs the ownership change.
- [x] **4.3 Strict cascade** (generic lifecycle only). `Scene`/`Game` call lifecycle methods only on `Object`s; `Object` dispatches to its own components through new non-virtual methods: `CollectWorkerUpdates(nodes)` (replaces `Game` reading `object->Components` to build parallel update jobs), `FenceBackgroundWork()` and `DispatchComponentDestroy()` (replace `Scene` fencing and destroying components directly on removal and in `Scene::FenceBackgroundWork`). `~Object` reuses them. **Deliberately not routed through `Object`, for performance:** the flat update index (Scene walks only objects with updating components, not the whole tree), typed system passes (post-physics rig/portal/soft tissue, pose constraints, particle budget, physics teardown by type), and index/snapshot bookkeeping. Verified: server output bit-identical on 6 scenes; wall time unchanged within noise (−0.9% to −2.1% on fox_ragdoll, physics_stress and snake, 5 alternating runs × 900 frames).
- [x] **4.4 `SceneRenderer`.** `Engine::Scene::Renderer::SceneRenderer` (`Core/Scene/Rendering/SceneRenderer.h`, replacing the old placeholder) owns all ~200 render members, the render-only nested types (`FrameRenderItem`, `PrepareTimings`, `ShadowDebugSnapshot`, `RetiredGraphicsResources`, `NativeRenderPacket`, ...) and every render method: `Render`, `PrepareRenderFrame`, graphics preparation, native packets, buffer/pipeline/texture retirement, material shaders, lighting bake, UI layout and embedded UI input, view overlays and render stats. **Ownership (decision, like the physics world):** `Scene` holds its renderer (`std::unique_ptr`, forward-declared) and hosts reach it through `Scene::GetRenderer()`. Making hosts own renderers would have meant rewiring every editor view, preview and importer for no behaviour gain; the renderer only reads the scene. `Scene::Init` still sets up the fallback camera and 2D mode, around `SceneRenderer::InitResources`/`BuildPipelines` (same order as before). `Scene::GetGraphicsProvider` forwards (components create their own GPU resources). **Stays on `Scene` (object lifetime):** deferred destruction of render-only objects (`m_retiredRenderObjects`, collected by `SceneRenderer::TakeRetiredGraphicsResources` via `Scene::TakeRetiredObjects`), the GPU-wait hook and `SetDeferredGraphicsRetirement`, the transform hierarchy, and `editorCamera` (the scene's fallback viewer object). Structure notifications call `SceneRenderer::MarkStructureChanged`; `ClearObjects` calls `OnSceneCleared`. The 19 helper classes take `SceneRenderer&` and reach scene state through `GetScene()`; `Scene` lost all 19 friend declarations and keeps one (`SceneRenderer`, used for snapshot suppression during frame preparation and transform preparation). `RenderStats` grouping was not done: the stats are plain members of `SceneRenderer` now, which already separates them from `Scene`. **Breaking for external projects:** render calls move from `scene.X(...)` to `scene.GetRenderer().X(...)` (e.g. `Render`, `PrepareRenderFrame`, `GetLastRenderCpuTimings`, `SetRealtimeShadowSettings`, `BakeLighting`), and the nested types move from `Scene::` to `Renderer::SceneRenderer::`. **Verified:** all hosts build; Server output bit-identical on 9 scenes; `.local/tests` pass, including the six `GameStandalone_*` GPU captures (dx11/dx12/vulkan).
- [x] **4.5 Break up `Render`.** `SceneRenderer::Render` is now a 110-line driver that runs named passes over a per-view `RenderView` (`Core/Scene/Rendering/SceneRenderer/SceneRenderView.h`): `BeginView` → `PrepareViewCamera` → `StartCameraOcclusion` → `PrepareViewLighting` → `PrepareViewEnvironment` → `PrepareViewOcclusion` → `BuildVisibleSet` → `PrepareDraws` (per draw: `AcceptDrawCandidate`, `BeginDrawRecord`, `PrepareShadowDrawRecord` or `ResolveCameraDrawRecord`/`ResolveDrawSurface`/`ApplyCameraDrawLighting`/`SelectCameraDrawPipeline`/`RecordPerformanceHeat`, state in `DrawCandidate`) → `FinishPreparedDraws` → `UploadDrawRecords` → `SubmitDirectionalShadow` → `SubmitPortalShadows` → `FinishShadowView` or `SubmitCameraView` (`SubmitFoliageDepth`, `SubmitCameraDraws`, `SubmitSelectionOutline`). The 320-line `submitAutomaticGroup` lambda became `SubmitDrawGroup` → `SubmitInstancedGroup` (+ `CollectInstanceTiles`) / `SubmitBatchedGroup`; `resolvePortalTarget` became `ResolvePortalTarget`. Statements moved verbatim: each pass binds the `RenderView` fields it uses (`auto& x = v.x;`). The camera-visibility and occlusion-buffer jobs are fenced by one guard in `Render`. Largest function: 196 lines. **Verified:** Server output bit-identical; `.local/tests` pass (18); DX11/DX12/Vulkan captures pixel-identical to the pre-refactor build on all deterministic scenes.
- [x] **4.6 Spatial.** `Engine::Scene::SpatialSystem` (`Core/Scene/Spatial/SpatialSystem.{h,cpp}`, renamed from `SceneSpatialWarp.cpp`) owns warp-volume mapping (`SampleSpatialPoint`, `MapSpatialPoint/Matrix/Ray`, `TryUnmapSpatialPoint`, `CaptureSpatialWarpSnapshot`, `TraceSpatialRayPath`), portal tracing (`TracePortalRay`, `RaycastPortals`, `IsObjectInSpatialRegion`), the `WarpWorld*` compatibility names and the `SpatialUpdateScheduler` (`UpdateSpatialScripts`, `GetUpdateScheduler`). The query types (`SpatialQuery`, `SpatialQueryDomain`, `SpatialRay`, `PortalRaySegment`, ...) are namespace-level in `Engine::Scene`. **Placement (decision):** the plan said `Game`, but these queries run without a `Game` (editor picking, the renderer, audio and edit-mode previews), so `Scene` holds the system like its physics world: `scene.GetSpatial()`. **Breaking for external projects:** `scene->MapSpatialPoint(...)` → `scene->GetSpatial().MapSpatialPoint(...)` (same for the other queries); `Scene::SpatialQueryDomain` and the other query types → `Engine::Scene::SpatialQueryDomain`; `Scene::IsObjectInSpatialRegion` → `SpatialSystem::IsObjectInSpatialRegion`; `GetSpatialUpdateScheduler` → `GetSpatial().GetUpdateScheduler()`.
- [x] **4.7 Directories.** Pure moves (147 files, `git mv`; includes rewritten, relative includes in moved files made root-based; namespaces unchanged):
  - `Core/Renderers/{DX11,DX12,Vulkan}` → `Core/Graphics/Backends/{DX11,DX12,Vulkan}`, with the backend helpers `D3DShaderCache`, `GraphicsDebugLayer`, `GraphicsDeviceName`, `HlslFileInclude`.
  - Renderer interfaces and factory (`IRenderer`, `IGameRenderer`, `IEditorRenderer`, `IView`, `RendererFactory`) → `Core/Graphics/`. `Core/Renderers/` is gone.
  - `Core/Scene/Rendering/*` (SceneRenderer, its passes and helpers) → `Core/Rendering/SceneRenderer.h` + `Core/Rendering/SceneRenderer/`; `Scene/Lighting/SceneLighting.cpp` (the lighting bake, now a renderer method) → `Core/Rendering/SceneRenderer/SceneLightingBake.cpp`. `Scene/Rendering` and `Scene/Lighting` are gone, so lighting now lives in `Components/Lighting` (components) and `Rendering/Lighting` (pipelines) only.
  - `UIRenderer` → `Core/Rendering/UI/`; `PostProcess` → `Core/Rendering/`.
  - Rehomed: `ExportFrameSequence` → `Core/Runtime/` (it drives a Game); `AutoQuality`, `HardwareSimulation`, `DetailBudget`, `DetailCostModel` → `Core/Rendering/Quality/`.
  - **Breaking for external projects** including any of these paths (no forwarders, as with `Compoonents`). Namespaces (`Engine::Renderers`, `Engine::Scene::Renderer`) are unchanged.
- [x] **Done when:** `Scene.h` declares only object-memory and lifecycle members, and `Scene::` methods live only in `Core/Scene/` (hierarchy, lifecycle, persistence, snapshot). **Met, with documented exceptions:** every `Scene::` method is defined under `Core/Scene/` (`Init`, `SetMode2D` and `GetGraphicsProvider` moved to `Scene/Camera/SceneCamera.cpp`). Besides object memory and lifecycle, `Scene.h` still declares the services the scene holds by decision (physics world, `SceneRenderer`, `SpatialSystem`, via `GetPhysics/GetRenderer/GetSpatial`), `settings`, `editorCamera`, the 2D mode, the particle budget (used by edit-mode particle previews) and the embedded gameplay-input flags (read by components; the editor sets them per view). Moving the last two to `Game` would leave edit mode without them.

## Phase 5: Split remaining god classes

- [x] `TerrainManager`: keep a thin component; extract `TerrainStreamer`, `FarTerrainCache` (+ worker), `TerrainColliderBuilder`, `TerrainSunShadow`, `TerrainEditSession`, `TerrainPrefabPlacement`.
  - [x] `TerrainSunShadow` (`Components/Terrain/TerrainSunShadow.{h,cpp}`, from `TerrainManagerSunShadow.cpp`): owns the worker job, pending build, texture and request; `TerrainManager::GetSunShadowMap` passes it the far height grid. `TerrainManager::SunShadowMap` is now an alias of `TerrainSunShadow::Map`.
  - [x] `TerrainColliderBuilder` (`TerrainColliderBuilder.{h,cpp}`, from `TerrainManagerColliders.cpp`): cell physics setup, authored collision-cell discovery, viewer-based activation and its counters.
  - [x] `TerrainPrefabPlacement` (`TerrainPrefabPlacement.{h,cpp}`): the streamed prefab-cell grid and its pool (`streamedPrefabPath`). `TerrainManager::StreamPrefabCells` invalidates authored collision cells when the grid changes. No bundled scene uses this mode; check it manually if a project does.
  - [x] `FarTerrainCache` (`FarTerrainCache.h`, `FarTerrainCache{State,Update,Worker}.cpp`, from `TerrainManagerFarTerrain*.cpp`): far height grid, tile quadtree, selection/update, the prepare worker and its queues, timings and palette. Reached through `TerrainManager::GetFarTerrain()`; `FarTerrainPalette`, `FarLevelUsage`, `FarUpdateTiming` stay available as `TerrainManager::` aliases. The shared near/far upload budget stays on the manager. **Breaking** for scripts calling the far API on the manager (`manager->UpdateFarTerrain(...)` → `manager->GetFarTerrain().UpdateFarTerrain(...)`); the bundled scripts are updated. Landscape capture (dense far terrain, fixed camera) matches the pre-refactor build to 0.04% of pixels.
  - [x] `TerrainEditSession` (`TerrainEditSession.h`): brush and region edits (`ApplyBrush`, `EditRegion`, `PaintRegion`), loading cells for an edit, and the record of edited and unsaved procedural cells. The method definitions stay in `TerrainManagerEditing.cpp` / `TerrainRegionEdit.cpp` next to their file-local helpers. Reached through `TerrainManager::GetEditing()` (**breaking** for callers of `manager->ApplyBrush(...)`; the editor, bundled scripts and `TerrainEditingToolsTests` are updated). `TerrainEditingToolsTests` passes.
  - [x] `TerrainStreamer` (`TerrainStreamer.{h,cpp}`): the near-cell pipeline. It chooses cells and LOD strides, runs the prepare worker and its queues, commits prepared cells within the upload budget, and tracks the streaming center. The manager's update (`UpdateStreaming*`) drives it; cell creation and removal (`LoadCell`/`UnloadCell`) and the update-wide `StreamingTiming` stay on the manager. Held by `unique_ptr` because its task type uses the manager's nested types. Landscape near/far captures match the pre-refactor build within animation noise.
  - The manager keeps the cell data model (authored, procedural and baked cells, object layers), cell lifecycle, render batches, far objects, the runtime bake and serialization/inspector; its header and .cpp files went from about 7,800 to about 3,200 lines.
- [x] `Mesh`: separate `MeshGeometry` data, `MeshLod`, `MeshMorphs`, `MeshSlicer` (editor/tool), serialization.
  - [x] `MeshMorphs` (`Components/Mesh/MeshMorphs.h`): targets, weights, packed weights, GPU delta/weight buffers and the weight-observation revision (`ObserveWeights`, `SetWeights`, `GetWeightsRevision`). `Mesh` keeps its public morph API and builds/uploads the buffers from its vertices. MMD and animation-showcase Server runs and an MMD GPU capture are identical to the pre-refactor build.
  - [x] `MeshLod` → `MeshRuntimeLods` (`Components/Mesh/MeshRuntimeLods.h`): resident levels, active/previous level, cross-fade progress, visibility and fade revision, LOD envelope, authored-LOD signature; owns the transition, visibility and geometry-fade logic. Level storage and selection (`StoreRuntimeLod`, `SelectRuntimeLod`) stay on `Mesh` because they swap geometry with it. (Named `MeshRuntimeLods` because `Mesh/Lod/` already holds the authored-LOD headers.)
  - [x] `MeshSlicer` (`Components/Mesh/MeshSlicer.{h,cpp}`, from `MeshSlicing.cpp`): `SliceByPlane` is a standalone tool over vertex data; portal traversal calls `MeshSlicer::SliceByPlane` (**breaking** for callers of `Mesh::SliceByPlane` / `Mesh::SliceResult`). Server output (including `portal_split_on_destroy`) is identical.
  - [x] `MeshGeometry` and serialization: **decided against a further split.** With morphs, runtime LODs and slicing out, `Mesh`'s remaining state is its geometry (vertices, indices, GPU buffers, bounds, revisions), which is the component's purpose; wrapping it in a `MeshGeometry` struct would add indirection without separating a responsibility. Serialization implements `Component::Serialize/Deserialize` and already lives in `MeshSerialization*.cpp`.
- [x] `EditorState`: extract `AnimationEditor`, `MeshEditSession`, `SkeletonEditor`, `DocumentManager`, `EditorHistory`; `EditorState` becomes the coordinator.
  - [x] `EditorHistory` (`Editor/State/EditorHistory.h`): scene undo/redo entries, baseline, pending edit, background capture, history limit, edit/saved revisions and saved snapshot, with capture/track/flush/commit/apply/reset/trim and `StepScene`. `EditorState::Undo/Redo` stay as the coordinator across animation, texture paint, skeleton, mesh, asset documents and scene history; `TrackSceneChanges`, `MarkSceneEdited`, `SetHistoryLimit`, `GetSceneHistoryMemoryBytes` forward. Definitions stay in `EditorStateHistory.cpp`. Verified by build and an Editor launch smoke test only (no automated editor tests); see the manual-check list.
  - [x] `MeshEditSession` → `MeshEditor` (`Editor/State/MeshEditor.h`): owns the main and prefab `MeshEditSession` (the per-scene state struct, kept) and the per-asset session cache, and the mesh edit mode logic: selection sync, viewport input, vertex/texture paint strokes, mesh and texture-paint history steps, saving, and the mesh edit toolbar (`DrawMeshEditTools`). `EditorState::GetMeshEditor()`. `EditorHistory` gained `GetHistoryLimit()`. Build plus Editor launch smoke test.
  - [x] `SkeletonEditor` (`Editor/State/SkeletonEditor.h`): owns the main and prefab `SkeletonEditSession` and the skeleton edit mode logic: bone and skin-binding actions, weight paint strokes, bind-pose refresh, skeleton history (interleaved with scene history, so it is a friend of `EditorHistory`), viewport input and the toolbar. `EditorState::GetSkeletonEditor()`. Build plus Editor launch smoke test.
  - [x] `DocumentManager` (`Editor/State/DocumentManager.h`): asset documents, scene asset documents and the prefab stage (scene, view, paths, async read, unsaved/focus flags), with their queued opens, activation, snapshots, titles, saving, closing and stage visibility. `EditorState::GetDocumentManager()`; held where the prefab scene used to be declared so scene teardown order is unchanged. Stage import stays with the import code on `EditorState`. Build plus Editor launch smoke test.
  - [x] `AnimationEditor` (`Editor/State/AnimationEditor.h`): animation documents (open, close, retire, persistence), playback and sampling, timeline and key editing, animation graph, rig toolbar/viewport/constraints, pose library, properties, animation history — 55 methods across the nine `EditorStateAnimation*.cpp` files (definitions stay there). `EditorState::GetAnimationEditor()`; held where the animation documents were declared. Build, `.local/tests`, Editor launch smoke test.
  - `EditorState` is now the coordinator: window, panels, scenes, play mode, import, lighting bake, toolbar and the cross-editor `Undo`/`Redo`. Its header went from 1,074 to 886 lines and from about 204 to about 127 data members. The five collaborators are created in its constructor and reached through `GetHistory`-style accessors (`GetMeshEditor`, `GetSkeletonEditor`, `GetDocumentManager`, `GetAnimationEditor`; `EditorHistory` through `m_history` and forwarders).
- [x] Decide on a scene-service/system layer for `AnimationManager` / `SpriteAnimationManager`, or keep them as components that delegate to system classes. **Decision: keep them as components.** Each holds one character's playback state (clip, time, layers, fades, rest-pose and binding caches) for the `Model` it references, is serialized per object, and runs in edit-mode previews without a `Game`; the `Game`'s parallel update already schedules them. A scene system would only pay off for cross-character batching (e.g. GPU skinning in a compute pass); revisit then.
- [x] `Model/` split. Settings (`GraphicsQualitySettings`, `MaterialCacheSettings`, `ProjectLayers`, `ProjectSettings`, `SceneSettings`, `TextureLodSettings`, `VideoProfiles`) → `Core/Settings/`. Asset formats (`AnimationClipAsset`, `AnimationPackAsset`, `MeshPackAsset`, `PoseAsset`, `RigAsset`, `AssetData`, `ImportedModel`) → `Core/AssetFormats/`, **not** `Core/Assets/`: that is the content folder, and CMake excludes `Assets/` paths from engine sources. `MeshLodGenerator` → `Core/Components/Mesh/Lod/`. `Model/` keeps the plain data types (animation, mesh, lighting, renderer, sprite and UI data, `HumanoidRig`). Pure moves; namespace `Engine::Model` unchanged; **breaking** for external includes of the moved headers.

## Phase 6: Editor directory tidy

- [x] `Editor/Core/` → `Editor/Tools/` (136 files; the Editor include root `Engine/Editor/Core` is now `Engine/Editor/Tools`).
- [x] `GameBuildManager`, `BuildTreeLock`, `CodeEditorLink` → `Editor/Build/` (alongside `VSCodeBridge/`). The `Editor/` root now holds only `EditorApplication`, `ProjectLauncher` and `Main.cpp`.
- [x] Large views to split when touched: `PreferencesView` (2,024), `EditorGizmoSystem` (1,959), `ImGuiEditorUi` (1,861), `AssetsExplorerView` (1,798). **Standing rule, not a task:** none of these were changed beyond include paths in this refactor; split each when it is next edited for a feature.

---

## Rules for every phase

- Build `Engine`, `Editor` and `Game` (debug preset) after each step. Run the `.local/tests` regression targets that touch the moved code.
- One concern per commit; pure moves separate from behavior changes, so `git log --follow` works.
- Sources are found by `GLOB_RECURSE CONFIGURE_DEPENDS`, so moves need a re-configure but no CMake edits.
- Keep serialized type names and component field names unchanged: scenes and prefabs on disk must load without migration.
