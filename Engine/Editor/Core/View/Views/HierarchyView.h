#pragma once
#include "pch.h"

#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "View/IEditorPanel.h"
#include "Engine/Editor/UI/IEditorUi.h"

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

    void Init(Scene* scene) { m_scene = scene; }

    void DrawPanel(IEditorUi& ui) override;

    Object* GetSelectedObject() const  { return m_selectedObject; }
    void    SetSelectedObject(Object* obj);
    void SetDebugInteractionLogging(bool enabled);

    // Fires whenever the selected object changes (including deselect → nullptr).
    std::function<void(Object*)> OnSelectionChanged;

    // Fires when the user double-clicks an object — editor should frame it in the scene view.
    std::function<void(Object*)> OnFocusObject;
    std::function<void()> OnHierarchyChanged;
    std::function<void(const std::string&)> OnInteractionLog;

private:
    enum class PendingAddType { Empty, Cube };
    void DrawObjectNode(IEditorUi& ui, Object* obj, int depth,
        bool lastSibling, uint64_t ancestorGuideMask = 0);
    void LogInteraction(const std::string& message) const;
    void CopySelection();
    void PasteClipboard();

    Scene*  m_scene          = nullptr;
    Object* m_selectedObject = nullptr;
    Object* m_pendingDragged = nullptr;
    Object* m_pendingTarget = nullptr;
    Object* m_pendingAddParent = nullptr;
    Object* m_pendingDelete = nullptr;
    PendingAddType m_pendingAddType = PendingAddType::Empty;
    bool m_hasPendingAdd = false;
    Scene::ObjectPlacement m_pendingPlacement = Scene::ObjectPlacement::AsChild;
    bool m_hasPendingMove = false;
    bool m_debugInteractionLogging = true;
    bool m_dragObservedThisFrame = false;
    bool m_dropObservedThisFrame = false;
    Object* m_debugDragSource = nullptr;
    Object* m_debugHoverTarget = nullptr;
    EditorUiHierarchyDropPosition m_debugHoverPosition = EditorUiHierarchyDropPosition::None;
    int m_debugHoverTargetDepth = 0;
    int m_debugDropDepth = -1;
    std::string m_objectClipboard;
    Scene::ObjectPath m_clipboardSourcePath;
};
