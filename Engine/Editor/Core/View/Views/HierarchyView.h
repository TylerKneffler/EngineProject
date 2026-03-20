#pragma once
#include "pch.h"

#include "Core/Scene/Scene.h"
#include "Core/Object.h"

// ---------------------------------------------------------------------------
// HierarchyView
//
// Draws the "HierarchyView" ImGui panel showing all scene objects in a
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
class HierarchyView
{
public:
    HierarchyView()  = default;
    ~HierarchyView() = default;

    void Init(Scene* scene) { m_scene = scene; }

    void DrawPanel();

    Object* GetSelectedObject() const  { return m_selectedObject; }
    void    SetSelectedObject(Object* obj);

    // Fires whenever the selected object changes (including deselect → nullptr).
    std::function<void(Object*)> OnSelectionChanged;

private:
    void DrawObjectNode(Object* obj);

    Scene*  m_scene          = nullptr;
    Object* m_selectedObject = nullptr;
};