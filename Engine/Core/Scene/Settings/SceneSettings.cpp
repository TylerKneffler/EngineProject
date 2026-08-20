#include "Core/Scene/Settings/SceneSettingsSerialization.h"
#include <algorithm>
#include <cctype>

namespace Engine::Scene
{
namespace
{
    Engine::Serialization::JsonValue Vec3ToJson(const glm::vec3& value)
    {
        return Engine::Serialization::JsonValue::MakeArray()
            .Push(Engine::Serialization::JsonValue(value.x))
            .Push(Engine::Serialization::JsonValue(value.y))
            .Push(Engine::Serialization::JsonValue(value.z));
    }

    glm::vec3 Vec3FromJson(const Engine::Serialization::JsonValue& value, const glm::vec3& fallback)
    {
        if (!value.IsArray() || value.ArraySize() < 3)
            return fallback;
        return {
            value.ArrayAt(0).AsFloat(),
            value.ArrayAt(1).AsFloat(),
            value.ArrayAt(2).AsFloat()
        };
    }

    const Engine::Serialization::JsonValue* FindField(
        const Engine::Serialization::JsonValue& object,
        const char* key)
    {
        if (!object.IsObject() || !key)
            return nullptr;
        if (object.Has(key))
            return &object[key];

        std::string loweredKey(key);
        std::transform(loweredKey.begin(), loweredKey.end(), loweredKey.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });

        for (std::size_t index = 0; index < object.ObjectSize(); ++index)
        {
            std::string candidate = object.ObjectKey(index);
            std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            if (candidate == loweredKey)
                return &object.ObjectValue(index);
        }
        return nullptr;
    }
}

Engine::Serialization::JsonValue SerializeSceneSettings(const Engine::Model::SceneSettings& settings)
{
    Engine::Serialization::JsonValue value = Engine::Serialization::JsonValue::MakeObject();
    value.Set("showGrid", Engine::Serialization::JsonValue(settings.showGrid));
    value.Set("gridHalfSize", Engine::Serialization::JsonValue(settings.gridHalfSize));
    value.Set("gridCellSize", Engine::Serialization::JsonValue(settings.gridCellSize));
    value.Set("gridOpacity", Engine::Serialization::JsonValue(settings.gridOpacity));
    value.Set("gridFadeDistance", Engine::Serialization::JsonValue(settings.gridFadeDistance));
    value.Set("gridColor", Vec3ToJson(settings.gridColor));
    value.Set("gridOriginColor", Vec3ToJson(settings.gridOriginColor));
    value.Set("ambientColor", Vec3ToJson(settings.ambientColor));
    value.Set("skyboxTexture", Engine::Serialization::JsonValue(settings.skyboxTexture));
    value.Set("renderMode",
        Engine::Serialization::JsonValue(static_cast<int>(settings.renderMode)));
    value.Set("sceneViewUiOverlay",
        Engine::Serialization::JsonValue(settings.sceneViewUiOverlay));
    return value;
}

void DeserializeSceneSettings(Engine::Model::SceneSettings& settings, const Engine::Serialization::JsonValue& value)
{
    if (!value.IsObject())
        return;
    if (const auto* showGrid = FindField(value, "showGrid"))
        settings.showGrid = showGrid->AsBool();
    if (const auto* gridHalfSize = FindField(value, "gridHalfSize"))
        settings.gridHalfSize = gridHalfSize->AsInt();
    if (const auto* gridCellSize = FindField(value, "gridCellSize"))
        settings.gridCellSize = gridCellSize->AsFloat();
    if (const auto* gridOpacity = FindField(value, "gridOpacity"))
        settings.gridOpacity = gridOpacity->AsFloat();
    if (const auto* gridFadeDistance = FindField(value, "gridFadeDistance"))
        settings.gridFadeDistance = gridFadeDistance->AsFloat();
    if (const auto* gridColor = FindField(value, "gridColor"))
        settings.gridColor = Vec3FromJson(*gridColor, settings.gridColor);
    if (const auto* gridOriginColor = FindField(value, "gridOriginColor"))
        settings.gridOriginColor = Vec3FromJson(*gridOriginColor, settings.gridOriginColor);
    if (const auto* ambientColor = FindField(value, "ambientColor"))
        settings.ambientColor = Vec3FromJson(*ambientColor, settings.ambientColor);
    if (const auto* skyboxTexture = FindField(value, "skyboxTexture"))
        settings.skyboxTexture = skyboxTexture->AsString();
    else
        settings.skyboxTexture.clear();
    if (const auto* renderMode = FindField(value, "renderMode"))
    {
        const int mode = renderMode->AsInt();
        switch (mode)
        {
        case 1:
            settings.renderMode = Engine::Model::SceneRenderMode::Unlit;
            break;
        case 2:
            settings.renderMode = Engine::Model::SceneRenderMode::Wireframe;
            break;
        default:
            settings.renderMode = Engine::Model::SceneRenderMode::Lit;
            break;
        }
    }
    if (const auto* sceneViewUiOverlay = FindField(value, "sceneViewUiOverlay"))
        settings.sceneViewUiOverlay = sceneViewUiOverlay->AsBool();
}
}
