#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/Core/View/Templates/Properties/PropertiesViewTemplateHelpers.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Component.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <array>
#include <memory>
#include <unordered_set>

namespace Engine::Editor
{
namespace
{
size_t ComponentCategory(const std::string& type,
    const std::unordered_set<std::string>& scripts)
{
    if (scripts.count(type)) return 6;
    if (type == "Mesh" || type == "Material" ||
        type == "MaterialPropertyMorph" || type == "Camera" ||
        type == "Light" || type == "LightProbe" || type == "LightProbeGroup" ||
        type == "Sprite" || type == "BakedLightingData") return 0;
    if (type == "RigidBody" || type == "PrimitiveObjectCollider" ||
        type == "MeshObjectCollider" || type == "Cloth" ||
        type == "RigPhysicsBinding") return 1;
    if (type == "Model" || type == "AnimationManager" ||
        type == "RigController" || type == "RigConstraints" ||
        type == "RigMorphControls" ||
        type == "SceneCameraLightTrack" ||
        type == "Skeleton" ||
        type == "AnimationBone" || type == "SkinnedMesh" ||
        type == "SoftTissueSurface" || type == "IKBone" ||
        type == "TwoBoneIK" || type == "ChainIK" ||
        type == "AimConstraint" || type == "ParentConstraint" ||
        type == "PositionConstraint" || type == "RotationConstraint" ||
        type == "SpriteAnimationManager" || type == "CameraTrack") return 2;
    if (type == "Canvas" || type == "UIButton" || type == "UIImage" ||
        type == "UIObject" || type == "UIText") return 3;
    if (type == "AudioSource") return 4;
    if (type == "SpatialManipulator" || type == "SplinePath" ||
        type == "SplineFollower") return 5;
    return 7;
}
}

bool PropertiesView::AddRegisteredComponent(const std::string& typeName, std::string& message)
{
    if (!m_selectedObject)
    {
        message = "[Properties] Select an object before adding a component.";
        return false;
    }
    if (m_selectedObject->IsPartOfPrefabInstance())
    {
        message = "[Properties] Prefab instance content is read-only. Open the prefab asset or unpack the instance first.";
        return false;
    }
    try
    {
        std::unique_ptr<Engine::Core::Component> component(
            Engine::Serialization::SceneSerializer::CreateRegisteredComponent(typeName));
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
                m_selectedObject->Components.end(), [&serializedType](const Engine::Core::Component* existing)
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

    ui.SearchInput("##componentSearch", m_componentSearch,
        sizeof(m_componentSearch), "Find component...");
    ui.Separator();
    const std::vector<std::string>& types =
        Engine::Serialization::SceneSerializer::GetRegisteredComponentTypes();
    const std::vector<std::string> registeredScripts =
        Engine::Serialization::SceneSerializer::GetRegisteredScriptTypes();
    const std::unordered_set<std::string> scripts(
        registeredScripts.begin(), registeredScripts.end());
    constexpr std::array<const char*, 8> categoryNames = {
        "Rendering", "Physics", "Animation", "UI", "Audio",
        "Spatial", "Scripts", "Other"
    };
    std::array<std::vector<const std::string*>, categoryNames.size()> categories;
    bool foundMatch = false;
    for (const std::string& type : types)
    {
        if (!ContainsIgnoringCase(type, m_componentSearch))
            continue;
        foundMatch = true;
        categories[ComponentCategory(type, scripts)].push_back(&type);
    }
    for (size_t category = 0; category < categories.size(); ++category)
    {
        const auto& entries = categories[category];
        if (entries.empty()) continue;
        if (m_componentSearch[0] != '\0')
            ui.SetNextComponentHeaderOpen(true);
        const std::string label = std::string(categoryNames[category]) +
            " (" + std::to_string(entries.size()) + ")";
        if (!ui.CollapsingHeader(label.c_str(), false)) continue;
        ui.Indent(8.f);
        for (const std::string* type : entries)
        {
            if (ui.Selectable(type->c_str()))
            {
                std::string message;
                const bool added = AddRegisteredComponent(*type, message);
                LogAssetDrop(message, !added);
                if (added)
                {
                    if (OnComponentsChanged)
                        OnComponentsChanged();
                    m_componentPickerOpen = false;
                }
            }
        }
        ui.Unindent(8.f);
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
}
