#pragma once
#include "pch.h"

#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "View/IEditorPanel.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// HierarchyView
//
// Defines the Hierarchy panel showing all scene objects in a
// parent → child tree, with a "World" root node, matching the style of
// Unity / Unreal / Godot editors.
//
// Usage:
//   hierarchy.Init(&scene);
//   // each frame:
//   hierarchy.DrawPanel();
//
// Call GetSelectedObject() to retrieve the currently selected object for
// use in the Properties panel.
// ---------------------------------------------------------------------------
class HierarchyView : public IEditorPanel
{
public:
    HierarchyView();
    ~HierarchyView() = default;

    void Init(Engine::Scene::Scene* scene) { m_scene = scene; }

    void DrawPanel(IEditorUi& ui) override;

    Engine::Core::Object* GetSelectedObject() const  { return m_selectedObject; }
    const std::vector<Engine::Core::Object*>& GetSelectedObjects() const { return m_selectedObjects; }
    void    SetSelectedObject(Engine::Core::Object* obj);
    void RequestDeleteSelectedObject();
    void SetDebugInteractionLogging(bool enabled);
    void SetObjectFilter(std::function<bool(const Engine::Core::Object*)> filter)
    { m_objectFilter = std::move(filter); }
    void SetAllowDelete(bool allow) { m_allowDelete = allow; }
    void SetFilteredObjectContextActions(bool allow, Engine::Core::Object* root)
    { m_allowFilteredContextActions = allow; m_filteredContextRoot = root; }
    void SetSkeletonContextActions(bool allow, Engine::Core::Object* root)
    { m_allowSkeletonContextActions = allow; m_filteredContextRoot = root; }

    // Fires whenever the selected object changes (including deselect → nullptr).
    std::function<void(Engine::Core::Object*)> OnSelectionChanged;
    std::function<void(const std::vector<Engine::Core::Object*>&)> OnSelectionSetChanged;

    // Fires when the user double-clicks an object — editor should frame it in the scene view.
    std::function<void(Engine::Core::Object*)> OnFocusObject;
    std::function<void(const std::string&)> OnPrefabRequested;
    std::function<void()> OnHierarchyChanged;
    std::function<void(const std::string&)> OnInteractionLog;

private:
    enum class PendingAddType { Empty, Primitive3D, Sprite, LightProbe,
        LightProbeGroup };
    enum class PendingPrefabAction { None, Apply, ApplyAll, Revert, Unpack };
    void DrawObjectNode(IEditorUi& ui, Engine::Core::Object* obj, int depth,
        bool lastSibling, uint64_t ancestorGuideMask = 0);
    void SelectSceneRoot();
    void LogInteraction(const std::string& message) const;
    void CopySelection();
    void PasteClipboard();
    void NotifySelectionChanged();

    Engine::Scene::Scene*  m_scene          = nullptr;
    Engine::Core::Object* m_selectedObject = nullptr;
    Engine::Core::Object* m_selectionAnchor = nullptr;
    std::vector<Engine::Core::Object*> m_selectedObjects;
    Engine::Core::Object* m_pendingDragged = nullptr;
    Engine::Core::Object* m_pendingTarget = nullptr;
    Engine::Core::Object* m_pendingAddParent = nullptr;
    Engine::Core::Object* m_pendingDelete = nullptr;
    Engine::Core::Object* m_pendingPrefabRoot = nullptr;
    PendingPrefabAction m_pendingPrefabAction = PendingPrefabAction::None;
    PendingAddType m_pendingAddType = PendingAddType::Empty;
    std::string m_pendingPrimitive3D;
    bool m_hasPendingAdd = false;
    ::Engine::Scene::Scene::ObjectPlacement m_pendingPlacement = ::Engine::Scene::Scene::ObjectPlacement::AsChild;
    bool m_hasPendingMove = false;
    bool m_debugInteractionLogging = true;
    bool m_dragObservedThisFrame = false;
    bool m_dropObservedThisFrame = false;
    Engine::Core::Object* m_debugDragSource = nullptr;
    Engine::Core::Object* m_debugHoverTarget = nullptr;
    EditorUiHierarchyDropPosition m_debugHoverPosition = EditorUiHierarchyDropPosition::None;
    int m_debugHoverTargetDepth = 0;
    int m_debugDropDepth = -1;
    std::string m_objectClipboard;
    ::Engine::Scene::Scene::ObjectPath m_clipboardSourcePath;
    std::function<bool(const Engine::Core::Object*)> m_objectFilter;
    bool m_allowDelete = true;
    bool m_allowFilteredContextActions = false;
    bool m_allowFilteredReparent = false;
    bool m_allowSkeletonContextActions = false;
    Engine::Core::Object* m_filteredContextRoot = nullptr;
};
}
