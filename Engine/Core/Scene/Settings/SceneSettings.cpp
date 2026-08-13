#include "Core/Scene/Settings/SceneSettingsSerialization.h"

namespace
{
    JsonValue Vec3ToJson(const glm::vec3& value)
    {
        return JsonValue::MakeArray()
            .Push(JsonValue(value.x))
            .Push(JsonValue(value.y))
            .Push(JsonValue(value.z));
    }

    glm::vec3 Vec3FromJson(const JsonValue& value, const glm::vec3& fallback)
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

JsonValue SerializeSceneSettings(const SceneSettings& settings)
{
    JsonValue value = JsonValue::MakeObject();
    value.Set("showGrid", JsonValue(settings.showGrid));
    value.Set("gridHalfSize", JsonValue(settings.gridHalfSize));
    value.Set("gridCellSize", JsonValue(settings.gridCellSize));
    value.Set("gridOpacity", JsonValue(settings.gridOpacity));
    value.Set("gridFadeDistance", JsonValue(settings.gridFadeDistance));
    value.Set("gridColor", Vec3ToJson(settings.gridColor));
    value.Set("gridOriginColor", Vec3ToJson(settings.gridOriginColor));
    value.Set("ambientColor", Vec3ToJson(settings.ambientColor));
    value.Set("skyboxTexture", JsonValue(settings.skyboxTexture));
    return value;
}

void DeserializeSceneSettings(SceneSettings& settings, const JsonValue& value)
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
