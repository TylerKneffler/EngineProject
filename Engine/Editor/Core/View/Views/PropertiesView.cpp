#include "PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Component.h"

void PropertiesView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    if (!m_selectedObject) { ui.DisabledLabel("No object selected"); ui.EndWindow(); return; }
    Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();
    const bool linked = prefabRoot != nullptr;
    char name[256]; strncpy_s(name, m_selectedObject->name.c_str(), sizeof(name));
    ui.BeginDisabled(linked);
    if (ui.InputText("##name", name, sizeof(name))) m_selectedObject->name = name;
    ui.EndDisabled();
    if (prefabRoot && prefabRoot->Prefab)
    {
        ui.ValueLabel("Prefab", prefabRoot->Prefab->GetPath().c_str());
        ui.DisabledLabel(
            m_selectedObject == prefabRoot
                ? "Properties come from the prefab; root transform is instance placement."
                : "Properties and transform come from the prefab asset.");
    }
    else
        ui.DisabledLabel("Scene-only object");
    
    ui.Separator();
    
    // Draw Transform (always present, not in Components list)
    DrawTransform(ui);
    
    // Draw all components with accordion views
    for (Component* component : m_selectedObject->Components)
    {
        if (!component) continue;
        
        std::string componentType = component->GetTypeName();
        if (componentType.empty()) componentType = "Component";
        
        if (ui.CollapsingHeader(componentType.c_str()))
        {
            const bool locked = m_selectedObject->IsPartOfPrefabInstance();
            ui.BeginDisabled(locked);
            component->DrawProperties(ui);
            ui.EndDisabled();
        }
    }
    
    ui.EndWindow();
}

void PropertiesView::DrawTransform(IEditorUi& ui)
{
    Transform& t = m_selectedObject->transform;
    const Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();
    const bool locked = prefabRoot && prefabRoot != m_selectedObject;
    
    if (ui.CollapsingHeader("Transform"))
    {
        ui.BeginDisabled(locked);
        t.DrawProperties(ui);
        ui.EndDisabled();
    }
}
