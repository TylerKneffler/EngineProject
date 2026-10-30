#include "TerrainObjectLayer.h"
#include "Engine/Editor/UI/IEditorUi.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace Engine::Components
{
TerrainObjectLayer::TerrainObjectLayer()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainObjectLayer));
    RegisterField("viewDistance", viewDistance, "Object LOD");
    RegisterField("lod1Distance", lod1Distance, "Object LOD");
    RegisterField("lod2Distance", lod2Distance, "Object LOD");
    RegisterField("lodHysteresis", lodHysteresis, "Object LOD");
    RegisterField("shadowDistance", shadowDistance, "Object LOD");
}

glm::quat TerrainObjectLayer::SurfaceRotation(const glm::vec3& normal,
    float yawDegrees)
{
    const glm::vec3 up = glm::length(normal) > 1.e-5f
        ? glm::normalize(normal) : glm::vec3(0.f, 1.f, 0.f);
    return glm::normalize(glm::rotation(glm::vec3(0.f, 1.f, 0.f), up) *
        glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.f, 1.f, 0.f)));
}

TerrainObjectLayer::JsonValue TerrainObjectLayer::Serialize() const
{
    JsonValue out = Component::Serialize();
    out.Set("layerName", JsonValue(layerName));
    JsonValue entries = JsonValue::MakeArray();
    for (const Placement& placement : placements)
        entries.Push(JsonValue::MakeObject()
            .Set("asset", JsonValue(placement.asset))
            .Set("position", JsonValue::MakeArray()
                .Push(JsonValue(placement.position.x))
                .Push(JsonValue(placement.position.y))
                .Push(JsonValue(placement.position.z)))
            .Set("normal", JsonValue::MakeArray()
                .Push(JsonValue(placement.normal.x))
                .Push(JsonValue(placement.normal.y))
                .Push(JsonValue(placement.normal.z)))
            .Set("yawDegrees", JsonValue(placement.yawDegrees))
            .Set("scale", JsonValue(placement.scale)));
    return out.Set("placements", std::move(entries));
}

void TerrainObjectLayer::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    viewDistance = std::clamp(viewDistance, 1.f, 100000.f);
    lod1Distance = std::clamp(lod1Distance, 0.f, viewDistance);
    lod2Distance = std::clamp(lod2Distance, lod1Distance, viewDistance);
    lodHysteresis = std::clamp(lodHysteresis, 0.f, 10.f);
    shadowDistance = std::clamp(shadowDistance, 0.f, viewDistance);
    layerName = value.Has("layerName") ? value["layerName"].AsString() : "Objects";
    placements.clear();
    const auto& entries = value["placements"];
    placements.reserve(entries.ArraySize());
    for (size_t i = 0; i < entries.ArraySize(); ++i)
    {
        const auto& entry = entries.ArrayAt(i);
        const auto& position = entry["position"];
        if (position.ArraySize() != 3) continue;
        Placement placement;
        placement.asset = entry["asset"].AsString();
        placement.position = {position.ArrayAt(0).AsFloat(),
            position.ArrayAt(1).AsFloat(), position.ArrayAt(2).AsFloat()};
        const auto& normal = entry["normal"];
        if (normal.ArraySize() == 3)
            placement.normal = {normal.ArrayAt(0).AsFloat(),
                normal.ArrayAt(1).AsFloat(), normal.ArrayAt(2).AsFloat()};
        placement.yawDegrees = entry["yawDegrees"].AsFloat();
        placement.scale = entry.Has("scale") ?
            std::max(0.f, entry["scale"].AsFloat()) : 1.f;
        if (std::isfinite(placement.position.x) &&
            std::isfinite(placement.position.y) &&
            std::isfinite(placement.position.z) &&
            std::isfinite(placement.scale)) placements.push_back(std::move(placement));
    }
}

bool TerrainObjectLayer::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    ui.ValueLabel("Layer", layerName.c_str());
    changed |= ui.DragFloat("View distance (m)", &viewDistance,
        .25f, 1.f, 100000.f);
    changed |= ui.DragFloat("LOD 1 starts (m)", &lod1Distance,
        .25f, 0.f, viewDistance);
    changed |= ui.DragFloat("LOD 2 starts (m)", &lod2Distance,
        .25f, lod1Distance, viewDistance);
    changed |= ui.DragFloat("LOD hysteresis (m)", &lodHysteresis,
        .1f, 0.f, 10.f);
    changed |= ui.DragFloat("Shadow distance (m)", &shadowDistance,
        .25f, 0.f, viewDistance);
    viewDistance = std::clamp(viewDistance, 1.f, 100000.f);
    lod1Distance = std::clamp(lod1Distance, 0.f, viewDistance);
    lod2Distance = std::clamp(lod2Distance, lod1Distance, viewDistance);
    shadowDistance = std::clamp(shadowDistance, 0.f, viewDistance);
    const std::string count = "Stored placements: " +
        std::to_string(placements.size());
    ui.Label(count.c_str());
    if (changed) MarkConfigurationDirty();
    return changed;
}
}
