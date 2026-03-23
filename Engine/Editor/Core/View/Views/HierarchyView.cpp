#include "HierarchyView.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void HierarchyView::DrawPanel()
{
    ImGui::Begin("HierarchyView");

    if (!m_scene)
    {
        ImGui::TextDisabled("No scene loaded");
        ImGui::End();
        return;
    }

    // "World" root node — always open by default.
    ImGuiTreeNodeFlags worldFlags =
        ImGuiTreeNodeFlags_DefaultOpen |
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth;

    if (ImGui::TreeNodeEx("World", worldFlags))
    {
        for (const auto& obj : m_scene->GetObjects())
        {
            // Only draw root-level objects here; children are drawn recursively.
            if (obj->Parent == nullptr)
                DrawObjectNode(obj.get());
        }
        ImGui::TreePop();
    }

    // Clicking on empty space (no item under cursor) deselects.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered())
        SetSelectedObject(nullptr);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// DrawObjectNode  (recursive)
// ---------------------------------------------------------------------------
void HierarchyView::SetSelectedObject(Object* obj)
{
    if (m_selectedObject == obj) return;
    m_selectedObject = obj;
    if (OnSelectionChanged) OnSelectionChanged(obj);
}

void HierarchyView::DrawObjectNode(Object* obj)
{
    bool hasChildren = !obj->Children.empty();

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth;

    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    if (obj == m_selectedObject)
        flags |= ImGuiTreeNodeFlags_Selected;

    const char* label = obj->name.empty() ? "(unnamed)" : obj->name.c_str();

    // Use the object pointer as the unique ID so sibling names can collide safely.
    bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(obj), flags, "%s", label);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        SetSelectedObject(obj);

    if (open && hasChildren)
    {
        for (Object* child : obj->Children)
            DrawObjectNode(child);
        ImGui::TreePop();
    }
}
