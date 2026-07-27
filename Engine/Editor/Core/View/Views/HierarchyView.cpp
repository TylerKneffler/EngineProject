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
    if (ui.TreeNode(this, "World", false, false, true))
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
    const std::string label = (obj->name.empty() ? "(unnamed)" : obj->name) +
        (obj->IsPrefabInstance() ? " [Prefab]" : "");
    const bool open = ui.TreeNode(obj, label.c_str(), obj == m_selectedObject, !hasChildren);
    if (ui.IsItemClicked()) SetSelectedObject(obj);
    if (ui.IsItemDoubleClicked()) { SetSelectedObject(obj); if (OnFocusObject) OnFocusObject(obj); }
    if (ui.BeginDragDropSource())
    {
        Object* payload = obj;
        ui.SetDragDropPayload("ENGINE_SCENE_OBJECT", &payload, sizeof(payload));
        ui.Label(label.c_str());
        ui.EndDragDropSource();
    }
    if (open && hasChildren) { for (Object* child : obj->Children) DrawObjectNode(ui, child); ui.TreePop(); }
}
