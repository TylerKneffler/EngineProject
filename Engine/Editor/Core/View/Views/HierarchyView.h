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
    HierarchyView()  = default;
    ~HierarchyView() = default;

    void Init(Engine::Scene::Scene* scene) { m_scene = scene; }

    void DrawPanel(IEditorUi& ui) override;

    Engine::Core::Object* GetSelectedObject() const  { return m_selectedObject; }
    void    SetSelectedObject(Engine::Core::Object* obj);
    void SetDebugInteractionLogging(bool enabled);

    // Fires whenever the selected object changes (including deselect → nullptr).
    std::function<void(Engine::Core::Object*)> OnSelectionChanged;

    // Fires when the user double-clicks an object — editor should frame it in the scene view.
    std::function<void(Engine::Core::Object*)> OnFocusObject;
    std::function<void()> OnHierarchyChanged;
    std::function<void(const std::string&)> OnInteractionLog;

private:
    enum class PendingAddType { Empty, Cube, Sprite };
    enum class PendingPrefabAction { None, Apply, ApplyAll, Revert, Unpack };
    void DrawObjectNode(IEditorUi& ui, Engine::Core::Object* obj, int depth,
        bool lastSibling, uint64_t ancestorGuideMask = 0);
    void LogInteraction(const std::string& message) const;
    void CopySelection();
    void PasteClipboard();

    Engine::Scene::Scene*  m_scene          = nullptr;
    Engine::Core::Object* m_selectedObject = nullptr;
    Engine::Core::Object* m_pendingDragged = nullptr;
    Engine::Core::Object* m_pendingTarget = nullptr;
    Engine::Core::Object* m_pendingAddParent = nullptr;
    Engine::Core::Object* m_pendingDelete = nullptr;
    Engine::Core::Object* m_pendingPrefabRoot = nullptr;
    PendingPrefabAction m_pendingPrefabAction = PendingPrefabAction::None;
    PendingAddType m_pendingAddType = PendingAddType::Empty;
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
};
}
