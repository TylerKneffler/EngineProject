#pragma once
#include "pch.h"
#include "Views/SceneView.h"
#include "Views/GameView.h"
#include "Views/HierarchyView.h"
#include "Views/PropertiesView.h"
#include "Views/AssetsExplorerView.h"
#include "Views/ConsoleView.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Scene/Scene.h"
#include "Core/ProjectLoader.h"
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// ViewFactory — creates and names editor panels.
//
// 3-D views (SceneView, GameView) require an SRV slot from the renderer's
// shader-visible heap.  The factory owns a free-list of slots; callers must
// call FreeSrvSlot() before destroying a 3-D view so the slot can be reused.
//
// All panel types are numbered sequentially per-type ("Scene 1", "Scene 2", …).
//
// Usage:
//   ViewFactory factory(renderer.get(), &scene, settings);
//   factory.OnSelectionChanged = [&](Object* o){ ... };
//   factory.OnSceneRequested   = [&](const std::string& p){ ... };
//
//   auto panel = factory.Create("Scene");   // returns unique_ptr<IEditorPanel>
//   bool ok    = factory.CanCreate3DView(); // false when SRV slots exhausted
// ---------------------------------------------------------------------------
class ViewFactory
{
public:
    // Slot 0 is always reserved for the ImGui font atlas.
    static constexpr uint32_t MAX_SRV_SLOTS = 32; // must match renderer heap size

    ViewFactory(IEditorRenderer*    renderer,
                Scene*              scene,
                const ProjectSettings& settings);

    // Create a new panel by type name.
    // Supported names: "Scene", "Game", "Hierarchy", "Properties", "Assets", "Console"
    // Returns nullptr if the type name is unknown or if no SRV slot is available for a 3-D view.
    std::unique_ptr<IEditorPanel> Create(const std::string& typeName);

    // Returns false when no SRV slots remain for 3-D views (Scene / Game).
    bool CanCreate3DView() const { return !m_freeSrvSlots.empty(); }

    // Return a previously-used SRV slot back to the free list.
    void FreeSrvSlot(uint32_t slot);

    // Returns true if typeName is a singleton panel type.
    // Singleton panels focus the existing window instead of spawning a second one.
    static bool IsSingleton(const std::string& typeName);

    // Must be called before a panel is destroyed so the factory can clear its
    // singleton tracking entry (preventing a dangling-pointer lookup).
    void NotifyPanelRemoved(IEditorPanel* panel);

    // ---- Callbacks wired into newly created panels ----
    std::function<void(Object*)>           OnSelectionChanged;  // HierarchyView
    std::function<void(const std::string&)> OnSceneRequested;   // AssetsExplorerView

private:
    uint32_t AllocSrvSlot(); // takes from free list; asserts CanCreate3DView()

    IEditorRenderer* m_renderer = nullptr;
    Scene*           m_scene    = nullptr;
    ProjectSettings     m_settings;

    std::vector<uint32_t> m_freeSrvSlots;

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
};
