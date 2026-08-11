#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/Core/View/Templates/Properties/PropertiesViewTemplateHelpers.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Component.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <memory>

using namespace Editor::ViewTemplates;

bool PropertiesView::AddRegisteredComponent(const std::string& typeName, std::string& message)
{
    if (!m_selectedObject)
    {
        message = "[Properties] Select an object before adding a component.";
        return false;
    }
    try
    {
        std::unique_ptr<Component> component(
            SceneSerializer::CreateRegisteredComponent(typeName));
        if (!component)
        {
            message = "[Properties] Component '" + typeName +
                "' is not compiled and registered; rebuild/register it before adding it.";
            return false;
        }
        if (component->singlecomponent)
        {
            const std::string serializedType = component->GetTypeName();
            const auto duplicate = std::find_if(m_selectedObject->Components.begin(),
                m_selectedObject->Components.end(), [&serializedType](const Component* existing)
                {
                    return existing && existing->GetTypeName() == serializedType;
                });
            if (duplicate != m_selectedObject->Components.end())
            {
                message = "[Properties] Object already has the single-instance component '" +
                    serializedType + "'.";
                return false;
            }
        }
        component->Owner = m_selectedObject;
        component->OnAfterDeserialize(m_scene ? m_scene->GetGraphicsProvider() : nullptr);
        m_selectedObject->Components.push_back(component.release());
        message = "[Properties] Added component '" + typeName + "'";
        return true;
    }
    catch (const std::exception& error)
    {
        message = "[Properties] Could not add component '" + typeName + "': " + error.what();
        return false;
    }
}

void PropertiesView::DrawComponentPicker(IEditorUi& ui)
{
    if (!m_componentPickerOpen || !m_selectedObject)
        return;
    if (m_positionComponentPicker)
    {
        ui.SetNextWindowRect(420.f, 220.f, 360.f, 420.f);
        m_positionComponentPicker = false;
    }
    if (!ui.BeginWindow("Add Component", &m_componentPickerOpen))
    {
        ui.EndWindow();
        return;
    }

    ui.InputText("Search", m_componentSearch, sizeof(m_componentSearch));
    ui.Separator();
    const std::vector<std::string> types = SceneSerializer::GetRegisteredComponentTypes();
    bool foundMatch = false;
    for (const std::string& type : types)
    {
        if (!ContainsIgnoringCase(type, m_componentSearch))
            continue;
        foundMatch = true;
        if (ui.Selectable(type.c_str()))
        {
            std::string message;
            const bool added = AddRegisteredComponent(type, message);
            LogAssetDrop(message, !added);
            if (added)
            {
                if (OnComponentsChanged)
                    OnComponentsChanged();
                m_componentPickerOpen = false;
            }
        }
    }
    if (!foundMatch)
        ui.DisabledLabel("No matching components");
    ui.EndWindow();
}

void PropertiesView::LogAssetDrop(const std::string& message, bool error) const
{
    if (OnAssetDropLog)
        OnAssetDropLog(message, error);
    else
        OutputDebugStringA((message + "\n").c_str());
}

