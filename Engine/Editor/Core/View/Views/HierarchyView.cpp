#include "HierarchyView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"

void HierarchyView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    if (!m_scene) { ui.DisabledLabel("No scene loaded"); ui.EndWindow(); return; }
    const bool worldOpen = ui.TreeNode(
        this, "World", m_selectedObject == nullptr, false, true);
    if (ui.IsItemClicked())
        SetSelectedObject(nullptr);
    if (worldOpen)
    {
        for (const auto& obj : m_scene->GetObjects()) if (!obj->Parent) DrawObjectNode(ui, obj.get());
        ui.TreePop();
    }
    if (ui.IsWindowBackgroundClicked()) SetSelectedObject(nullptr);
    ui.EndWindow();
}

void HierarchyView::SetSelectedObject(Object* obj)
{
    if (m_selectedObject == obj) return;
    m_selectedObject = obj;
    if (OnSelectionChanged) OnSelectionChanged(obj);
}

void HierarchyView::DrawObjectNode(IEditorUi& ui, Object* obj)
{
    const bool hasChildren = !obj->Children.empty();
    char name[256]; strncpy_s(name, obj->name.c_str(), sizeof(name));
    bool enabled = obj->enabled;
    const EditorUiObjectRowResult row = ui.ObjectTreeRow(
        obj, name, sizeof(name), &enabled, obj == m_selectedObject,
        !hasChildren, obj->IsPartOfPrefabInstance());
    if (row.nameChanged) obj->name = name;
    if (row.enabledChanged)
        enabled ? obj->Enabled() : obj->Disabled();
    if (row.clicked) SetSelectedObject(obj);
    if (row.doubleClicked) { SetSelectedObject(obj); if (OnFocusObject) OnFocusObject(obj); }
    if (ui.BeginDragDropSource())
    {
        Object* payload = obj;
        ui.SetDragDropPayload("ENGINE_SCENE_OBJECT", &payload, sizeof(payload));
        ui.Label(obj->name.empty() ? "(unnamed)" : obj->name.c_str());
        ui.EndDragDropSource();
    }
    if (row.open && hasChildren) { for (Object* child : obj->Children) DrawObjectNode(ui, child); ui.ObjectTreePop(); }
}
