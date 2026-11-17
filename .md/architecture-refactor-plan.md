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
- [~] Move editor selection, preview object, mesh-edit pose, weight paint, bone visibility, 2D mode and camera follow off `Scene`.
  - [x] **Step A:** consolidated the 14 editor fields into `Engine::Scene::EditorViewState` (`Core/Scene/EditorViewState.{h,cpp}`), held by `Scene` as `m_editorView`, with the terrain-placement selection logic moved into `EditorViewState::SelectObject`. The renderer (11 files) reads `m_editorView.*`. `Scene`'s existing accessors forward to it, and `Scene::GetEditorView()` exposes it. `Scene.h` no longer includes `TerrainPlacementHandle.h`.
  - [x] **Step B:** deleted 10 forwarding accessors from `Scene` (`Set/GetSelectedObject`, `Set/GetSelectedPlacementId`, `Set/GetEditorSelectedMesh`, `SetEditorBonesVisible`, `AreEditorBonesVisible`, `SetPreviewObject`, `IsEditorMode2D`). The compiler identified each Scene call site, so same-named methods on `HierarchyView`/`PropertiesView` were left alone. 108 sites in 23 editor files plus `.local/tests/TerrainPlacementEditingTests.cpp` now use `GetEditorView()`. Four methods stay on `Scene` because they do real scene work: `SetEditorMeshEditPose`/`IsEditorMeshEditPose` and `SetEditorWeightPaint` mark the prepared render frame stale, and `SetEditorMode2D` reconfigures graphics.
  - [ ] **Step C:** superseded. Editor state leaves `Scene` entirely in Phase 4, step 4.1.
- [x] Move `editorOverlays` (and its flag enum) off `Object`. **Correction:** it went into `Scene`'s `EditorViewState`, which gave `Scene` new editor bookkeeping. Phase 4, step 4.1 moves it into the editor.
- [x] Keep `editorHidden` on `Object` (deliberate exception). `IsEnabledInHierarchy()` checks it on every ancestor in update, render and pick loops, so moving it to a map would put hash lookups in those hot paths.
- [ ] Extract prefab-instance bookkeeping from `Object` into `PrefabInstanceLink` (held by pointer, null for non-prefab objects).

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
- [~] **4.2 `Game` runtime** (`Engine::Runtime::Game`, `Core/Runtime/Game.{h,cpp}`), in three steps:
  - [x] **4.2a Lifecycle driver.** `Game(Scene&)` uses the scene, binds itself (`Scene::GetGame()`), owns the `SimulationClock`, and runs `Start`, `Update` (the 8-phase job graph, unchanged), `UpdateFixedFrame`, `AdvanceManualFrame`, fixed/variable step and update timings. `Scene` lost `Start/Update/UpdateFixedFrame/AdvanceManualFrame/SetFixedTimeStep/UseVariableTimeStep/GetLastUpdateTimings/AdvanceEditorPreviews` and the clock. It keeps a small, documented simulation-step support API (`BeginUpdatePass`/`EndUpdatePass`, `GetUpdateObjects`, `PrepareTransformHierarchy`, `ApplyPoseConstraints`, `EnforceParticleBudget`) plus read-throughs: `GetDeltaTime/GetClock/GetFrameIndex/GetElapsedTime` read the bound Game (zero/stopped without one), and `HasStarted` is set by `Game::Start` so objects added to a running scene still start on commit. Hosts: `GameApplication`, `ServerApplication`, `VideoExportApplication`, editor play mode (a `Game` per play session, destroyed before the edit scene is restored), `EditorPlayPipeline`, `ExportFrameSequence`, `SceneManager` (restarts loaded scenes through the bound Game; none in edit mode), the failed-load restore in `ScenePersistence`, profilers/benchmark, and tests. `AdvanceEditorPreviews` moved to `Engine/Editor/Runtime/EditorPreviews`. `SetUpdateProfilingEnabled` stays on `Scene` for now because the renderer uses the same flag for its timings (moves in 4.4).
  - [x] **4.2b Audio → `Game`.** `Game` owns `Engine::Audio::Audio`: it updates the listener and drains the mixer each frame, stops sources when the scene clears (it observes the scene as an `ISceneObserver`) and when the Game ends, and handles `SynchronizeToTime` for the exporter. `Scene` lost `m_audio`/`GetAudio`. Server output is still bit-identical.
  - [x] **Physics world stays with `Scene` (owner decision); `Game` steps it.** The world is the scene's collision representation and is used without a running simulation: `GroundedFootIK::FindGround`/`ChainIK::ApplyGroup` raycast during edit-mode pose evaluation, `ParticleEmitter::ResolveCollisions` runs in edit-mode previews, and `~RigidBody`/`~Cloth`/`Disabled`/`SpatialManipulator::ClearSpatialWarpState` tear down bodies when the editor deletes objects. `Game::Update` does prepare/simulate/finish. The step settings (`ConfigureFixedStep`/`ConfigureStepBudget`) also stay on the world: they are stored by the Bullet adapter with its accumulator and substep state, and the editor sets the budget once on the scene.
  - [x] **4.2c `Game` absorbs `SceneManager`.** Scene transitions are per-Game: `LoadScene`, `RequestSceneLoad`, `RequestDefaultSceneLoad`, `LoadSceneAsync`, `ProcessPendingSceneLoad`, `CancelPendingSceneLoad`, default scene path and the loaded callback. Loads replace the Game's scene in place and restart it. The async reader moved to `Core/Runtime/AsyncSceneReader.h` (one worker per Game, joined in `~Game`). Deleted `Core/Scene/SceneManager.{h,cpp}` and the `Core/SceneManager.h` forwarder. **Breaking for external projects** that call `Engine::Core::SceneManager`; use `Owner->GetScene()->GetGame()->RequestSceneLoad(path)`. `MainMenuGameManager` is updated. `Game` does not take *ownership* of its scene: loads happen in place on the same `Scene`, so hosts keep owning scenes and nothing needs the ownership change.
- [x] **4.3 Strict cascade** (generic lifecycle only). `Scene`/`Game` call lifecycle methods only on `Object`s; `Object` dispatches to its own components through new non-virtual methods: `CollectWorkerUpdates(nodes)` (replaces `Game` reading `object->Components` to build parallel update jobs), `FenceBackgroundWork()` and `DispatchComponentDestroy()` (replace `Scene` fencing and destroying components directly on removal and in `Scene::FenceBackgroundWork`). `~Object` reuses them. **Deliberately not routed through `Object`, for performance:** the flat update index (Scene walks only objects with updating components, not the whole tree), typed system passes (post-physics rig/portal/soft tissue, pose constraints, particle budget, physics teardown by type), and index/snapshot bookkeeping. Verified: server output bit-identical on 6 scenes; wall time unchanged within noise (−0.9% to −2.1% on fox_ragdoll, physics_stress and snake, 5 alternating runs × 900 frames).
- [ ] **4.4 `SceneRenderer`.** Move the ~190 render members and `Render`, `PrepareRenderFrame`, graphics preparation, native packets and retirement into `SceneRenderer`. The 19 `friend` helper classes take `SceneRenderer&` plus a read-only scene view. Render stats go into `RenderStats`.
- [ ] **4.5 Break up `Render`** (~2,850 lines) into passes: frame prep → visibility/LOD → shadows → opaque → particles → portals → post/UI. Target: no function over ~200 lines.
- [ ] **4.6 Spatial.** Portal/warp mapping and raycasts plus the spatial update scheduler → `SpatialSystem` in `World`.
- [ ] **4.7 Directories.** `Core/Graphics/` (RHI interfaces) with `Core/Graphics/Backends/{DX11,DX12,Vulkan}`. `Core/Rendering/` (SceneRenderer, passes, lighting, portal, sprites, culling, LOD). Move `Core/Scene/Rendering/*` there. Move `PostProcess.cpp` out of `Graphics/`. Rehome `ExportFrameSequence`, `HardwareSimulation`, `AutoQuality`.
- [ ] **Done when:** `Scene.h` declares only object-memory and lifecycle members, and `Scene::` methods live only in `Core/Scene/` (hierarchy, lifecycle, persistence, snapshot).

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
