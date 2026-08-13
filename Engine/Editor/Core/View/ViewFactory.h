#pragma once
#include "pch.h"
#include "Views/SceneView.h"
#include "Views/GameView.h"
#include "Views/HierarchyView.h"
#include "Views/PropertiesView.h"
#include "Views/AssetsExplorerView.h"
#include "Views/ConsoleView.h"
#include "Views/TerminalView.h"
#include "Views/ProblemsView.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Scene/Scene.h"
#include "Core/ProjectLoader.h"
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// ViewFactory — creates and names editor panels.
//
// 3-D views (SceneView, GameView) require an SRV slot from the renderer's
// shader-visible heap. Callers must call FreeSrvSlot() before destroying a
// 3-D view so the slot can be reused.
//
// All panel types are numbered sequentially per-type ("Scene 1", "Scene 2", …).
//
// Usage:
//   ViewFactory factory(renderer.get(), &scene, settings);
//   factory.OnSelectionChanged = [&](Engine::Core::Object* o){ ... };
//   factory.OnSceneRequested   = [&](const std::string& p){ ... };
//
//   auto panel = factory.Create("Scene");   // returns unique_ptr<IEditorPanel>
//   bool ok    = factory.CanCreate3DView(); // false when SRV slots exhausted
// ---------------------------------------------------------------------------
class ViewFactory
{
public:
    // Slot 0 is reserved by the active UI graphics bridge.
    static constexpr uint32_t MAX_SRV_SLOTS = 32; // must match renderer heap size

    ViewFactory(::Engine::Renderers::IEditorRenderer*    renderer,
                Engine::Scene::Scene*              scene,
                const Engine::Model::ProjectSettings& settings);

    // Create a new panel by type name.
    // Supported names: "Scene", "Game", "Hierarchy", "Properties", "Assets", "Console", "Terminal", "Problems"
    // Returns nullptr if the type name is unknown or if no SRV slot is available for a 3-D view.
    std::unique_ptr<IEditorPanel> Create(const std::string& typeName);

    // Creates an additional scene viewport bound to an independent scene.
    // Used by prefab editing so the project Scene/Game views keep rendering
    // the active project scene.
    std::unique_ptr<SceneView> CreateSceneView(
        Engine::Scene::Scene* scene, const std::string& title);

    // Returns false when no SRV slots remain for 3-D views (Scene / Game).
    bool CanCreate3DView() const { return m_renderer && m_renderer->CanAllocateSrvSlot(); }

    // Return a previously-used SRV slot back to the free list.
    void FreeSrvSlot(uint32_t slot);

    // Returns true if typeName is a singleton panel type.
    // Singleton panels focus the existing window instead of spawning a second one.
    static bool IsSingleton(const std::string& typeName);

    // Must be called before a panel is destroyed so the factory can clear its
    // singleton tracking entry (preventing a dangling-pointer lookup).
    void NotifyPanelRemoved(IEditorPanel* panel);

    // ---- Callbacks wired into newly created panels ----
    std::function<void(Engine::Core::Object*)>           OnSelectionChanged;  // HierarchyView
    std::function<void()>                  OnMainDocumentFocused;
    std::function<void(Engine::Core::Object*)>           OnObjectSelected;    // SceneView click selection
    std::function<void(Engine::Core::Object*)>           OnObjectCreated;     // SceneView context creation
    std::function<void(bool)>              OnGizmoInteraction;  // SceneView transform drag
    std::function<void(Engine::Core::Object*)>           OnFocusObject;       // HierarchyView double-click
    std::function<void()>                  OnHierarchyChanged;
    std::function<void(const std::string&)> OnHierarchyInteraction;
    std::function<void(const std::string&)> OnSceneRequested;   // AssetsExplorerView
    std::function<void(const std::string&)> OnPrefabRequested;  // AssetsExplorerView
    std::function<void(const std::string&)> OnAssetSelected;    // AssetsExplorerView
    std::function<void(const std::string&)> OnAssetDropped;     // SceneView
    std::function<Engine::Core::Object*(const std::string&)> OnAssetPreviewRequested;
    std::function<void(Engine::Core::Object*)> OnAssetPreviewCancelled;
    std::function<void(Engine::Core::Object*, const std::string&)> OnAssetPreviewCommitted;
    std::function<void()>                  OnPropertiesChanged;
    std::function<void(const std::string&, bool)> OnPropertiesAssetDropLog;
    std::function<void(const std::string&, const std::string&)> OnAssetRenamed;
    std::function<void(const std::string&)> OnAssetContentsChanged;
    std::function<void(Engine::Core::Object*, const std::string&)> OnPrefabCreated;

private:
    ::Engine::Renderers::IEditorRenderer* m_renderer = nullptr;
    Engine::Scene::Scene*           m_scene    = nullptr;
    Engine::Model::ProjectSettings     m_settings;
    std::shared_ptr<EditorProblemStore> m_problemStore =
        std::make_shared<EditorProblemStore>();

    // Tracks live singleton panel instances (raw, non-owning).
    // Cleared via NotifyPanelRemoved when a panel is erased from the panels vector.
    std::unordered_map<std::string, IEditorPanel*> m_singletonInstances;

    // Set of type names that are restricted to one live instance at a time.
    static const std::unordered_set<std::string> kSingletonTypes;

    // Per-type instance counters for title generation.
    int m_sceneCount      = 0;
    int m_gameCount       = 0;
    int m_hierarchyCount  = 0;
    int m_propertiesCount = 0;
    int m_assetsCount     = 0;
    int m_consoleCount    = 0;
    int m_terminalCount   = 0;
    int m_problemsCount   = 0;
};
}
