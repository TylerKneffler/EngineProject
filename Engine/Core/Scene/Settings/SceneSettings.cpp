#include "Core/Scene/Scene.h"

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

JsonValue SceneSettings::Serialize() const
{
    JsonValue value = JsonValue::MakeObject();
    value.Set("showGrid", JsonValue(showGrid));
    value.Set("gridHalfSize", JsonValue(gridHalfSize));
    value.Set("gridCellSize", JsonValue(gridCellSize));
    value.Set("gridOpacity", JsonValue(gridOpacity));
    value.Set("gridFadeDistance", JsonValue(gridFadeDistance));
    value.Set("gridColor", Vec3ToJson(gridColor));
    value.Set("gridOriginColor", Vec3ToJson(gridOriginColor));
    value.Set("ambientColor", Vec3ToJson(ambientColor));
    value.Set("skyboxTexture", JsonValue(skyboxTexture));
    return value;
}

void SceneSettings::Deserialize(const JsonValue& value)
{
    if (!value.IsObject())
        return;
    if (value.Has("showGrid")) showGrid = value["showGrid"].AsBool();
    if (value.Has("gridHalfSize")) gridHalfSize = value["gridHalfSize"].AsInt();
    if (value.Has("gridCellSize")) gridCellSize = value["gridCellSize"].AsFloat();
    if (value.Has("gridOpacity")) gridOpacity = value["gridOpacity"].AsFloat();
    if (value.Has("gridFadeDistance")) gridFadeDistance = value["gridFadeDistance"].AsFloat();
    if (value.Has("gridColor"))
        gridColor = Vec3FromJson(value["gridColor"], gridColor);
    if (value.Has("gridOriginColor"))
        gridOriginColor = Vec3FromJson(value["gridOriginColor"], gridOriginColor);
    if (value.Has("ambientColor"))
        ambientColor = Vec3FromJson(value["ambientColor"], ambientColor);
    skyboxTexture = value.Has("skyboxTexture")
        ? value["skyboxTexture"].AsString()
        : std::string{};
}
