#include "Core/Scene/Settings/SceneSettingsSerialization.h"

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
    return value;
}

void DeserializeSceneSettings(Engine::Model::SceneSettings& settings, const Engine::Serialization::JsonValue& value)
{
    if (!value.IsObject())
        return;
    if (value.Has("showGrid")) settings.showGrid = value["showGrid"].AsBool();
    if (value.Has("gridHalfSize")) settings.gridHalfSize = value["gridHalfSize"].AsInt();
    if (value.Has("gridCellSize")) settings.gridCellSize = value["gridCellSize"].AsFloat();
    if (value.Has("gridOpacity")) settings.gridOpacity = value["gridOpacity"].AsFloat();
    if (value.Has("gridFadeDistance")) settings.gridFadeDistance = value["gridFadeDistance"].AsFloat();
    if (value.Has("gridColor"))
        settings.gridColor = Vec3FromJson(value["gridColor"], settings.gridColor);
    if (value.Has("gridOriginColor"))
        settings.gridOriginColor = Vec3FromJson(value["gridOriginColor"], settings.gridOriginColor);
    if (value.Has("ambientColor"))
        settings.ambientColor = Vec3FromJson(value["ambientColor"], settings.ambientColor);
    settings.skyboxTexture = value.Has("skyboxTexture")
        ? value["skyboxTexture"].AsString()
        : std::string{};
}
}
