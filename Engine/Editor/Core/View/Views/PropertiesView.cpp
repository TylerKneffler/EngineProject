#include "PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Component.h"
#include "Core/Scene/Scene.h"

void PropertiesView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    if (!m_selectedObject)
    {
        ui.Label("Scene");
        ui.Separator();
        if (!m_scene)
            ui.DisabledLabel("No scene loaded");
        else
        {
            char skyboxPath[512];
            strncpy_s(skyboxPath, m_scene->settings.skyboxTexture.c_str(), sizeof(skyboxPath));
            if (ui.InputText("Skybox Texture Override", skyboxPath, sizeof(skyboxPath)))
                m_scene->settings.skyboxTexture = skyboxPath;
            if (m_scene->settings.skyboxTexture.empty())
                ui.DisabledLabel("Using the editor default skybox texture.");
            ui.Separator();
            ui.Label("Baked Lighting");
            if (ui.Button("Bake Lighting", 140.f, 30.f))
                m_scene->BakeLighting();
            ui.SameLine();
            if (ui.Button("Clear Bake", 110.f, 30.f))
                m_scene->ClearBakedLighting();
            ui.DisabledLabel(m_scene->GetLightingBakeStatus().c_str());
            ui.DisabledLabel(
                "Bakes point-light irradiance into persistent object records.");
        }
        ui.EndWindow();
        return;
    }
    Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();
    const bool linked = prefabRoot != nullptr;
    char name[256]; strncpy_s(name, m_selectedObject->name.c_str(), sizeof(name));
    bool enabled = m_selectedObject->enabled;
    const EditorUiObjectRowResult header = ui.ObjectHeader(
        m_selectedObject, name, sizeof(name), &enabled, linked);
    if (header.nameChanged) m_selectedObject->name = name;
    if (header.enabledChanged)
        enabled ? m_selectedObject->Enabled() : m_selectedObject->Disabled();
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
