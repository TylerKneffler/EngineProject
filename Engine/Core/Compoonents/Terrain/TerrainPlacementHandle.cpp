#include "TerrainPlacementHandle.h"
#include "TerrainObjectLayer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Engine::Components
{
namespace
{
constexpr float kMinimumScale = 1.e-4f;

// Transform composes rotation as Rz * Ry * Rx (see ComposeLocalMatrix).
glm::mat3 EulerToMatrix(const glm::vec3& radians)
{
    return glm::mat3(glm::eulerAngleZYX(radians.z, radians.y, radians.x));
}

glm::vec3 MatrixToEuler(const glm::mat3& rotation)
{
    float z = 0.f, y = 0.f, x = 0.f;
    glm::extractEulerAngleZYX(glm::mat4(rotation), z, y, x);
    return { x, y, z };
}

float TwistAboutY(const glm::quat& rotation)
{
    // Swing-twist decomposition: the twist about +Y is the normalized
    // quaternion built from w and the Y component alone.
    const float length = std::sqrt(rotation.w * rotation.w + rotation.y * rotation.y);
    if (length < 1.e-6f) return 0.f;
    return glm::degrees(2.f * std::atan2(rotation.y / length, rotation.w / length));
}
}

TerrainPlacementHandle::TerrainPlacementHandle()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainPlacementHandle));
    singlecomponent = true;
}

TerrainObjectLayer* TerrainPlacementHandle::GetLayer() const
{
    return Owner && Owner->Parent
        ? Owner->Parent->GetComponent<TerrainObjectLayer>() : nullptr;
}

size_t TerrainPlacementHandle::FindPlacementIndex() const
{
    const auto* layer = GetLayer();
    if (!layer || !placementId) return SIZE_MAX;
    // Placement IDs are assigned lazily; make sure new records have one.
    layer->GetPlacementSignature();
    const auto& placements = layer->placements;
    if (m_indexHint < placements.size() && placements[m_indexHint].id == placementId)
        return m_indexHint;
    for (size_t index = 0; index < placements.size(); ++index)
        if (placements[index].id == placementId)
            return m_indexHint = index;
    return SIZE_MAX;
}

void TerrainPlacementHandle::RotationToPlacement(const glm::mat3& rotation,
    bool conformToGround, glm::vec3& normal, float& yawDegrees)
{
    const glm::quat orientation = glm::normalize(glm::quat_cast(rotation));
    if (conformToGround)
    {
        yawDegrees = TwistAboutY(orientation);
        return;
    }
    // SurfaceRotation(normal, yaw) = rotation(Y -> normal) * yaw about Y, so
    // the normal is the rotated up axis and the yaw is what remains.
    normal = glm::normalize(orientation * glm::vec3(0.f, 1.f, 0.f));
    const glm::quat swing = glm::rotation(glm::vec3(0.f, 1.f, 0.f), normal);
    yawDegrees = TwistAboutY(glm::normalize(glm::inverse(swing) * orientation));
}

bool TerrainPlacementHandle::PullFromPlacement()
{
    auto* layer = GetLayer();
    const size_t index = FindPlacementIndex();
    if (!Owner || !layer || index == SIZE_MAX) return false;
    const auto& placement = layer->placements[index];
    const glm::quat rotation = layer->conformToGround
        ? glm::angleAxis(glm::radians(placement.yawDegrees), glm::vec3(0.f, 1.f, 0.f))
        : TerrainObjectLayer::SurfaceRotation(placement.normal, placement.yawDegrees);
    Owner->transform.position = glm::dvec3(placement.position);
    Owner->transform.rotation = MatrixToEuler(glm::mat3_cast(rotation));
    Owner->transform.scale = glm::vec3(placement.scale);
    m_proxyRevision = Owner->transform.GetLocalRevision();
    m_layerRevision = layer->GetConfigurationRevision();
    return true;
}

TerrainPlacementHandle::SyncResult TerrainPlacementHandle::Synchronize()
{
    auto* layer = GetLayer();
    const size_t index = FindPlacementIndex();
    if (!Owner || !layer || index == SIZE_MAX) return SyncResult::PlacementMissing;

    const uint64_t proxyRevision = Owner->transform.GetLocalRevision();
    if (proxyRevision != m_proxyRevision)
    {
        auto& placement = layer->placements[index];
        const auto& transform = Owner->transform;
        placement.position = glm::vec3(transform.position);
        RotationToPlacement(EulerToMatrix(transform.rotation),
            layer->conformToGround, placement.normal, placement.yawDegrees);
        // Placements are uniformly scaled. A single-axis gizmo drag changes one
        // component; take whichever moved furthest from the stored value.
        float scale = transform.scale.x;
        for (int axis = 1; axis < 3; ++axis)
            if (std::abs(transform.scale[axis] - placement.scale) >
                std::abs(scale - placement.scale))
                scale = transform.scale[axis];
        placement.scale = std::isfinite(scale)
            ? std::copysign(std::max(std::abs(scale), kMinimumScale), scale)
            : placement.scale;
        layer->MarkConfigurationDirty();
        m_proxyRevision = proxyRevision;
        m_layerRevision = layer->GetConfigurationRevision();
        return SyncResult::PlacementUpdated;
    }
    if (layer->GetConfigurationRevision() != m_layerRevision)
        return PullFromPlacement() ? SyncResult::ProxyUpdated : SyncResult::PlacementMissing;
    return SyncResult::Unchanged;
}

bool TerrainPlacementHandle::RemovePlacement()
{
    auto* layer = GetLayer();
    const size_t index = FindPlacementIndex();
    if (!layer || index == SIZE_MAX) return false;
    layer->placements.erase(layer->placements.begin() + static_cast<std::ptrdiff_t>(index));
    layer->MarkConfigurationDirty();
    m_layerRevision = layer->GetConfigurationRevision();
    return true;
}

Engine::Core::Object* TerrainPlacementHandle::Acquire(Engine::Scene::Scene& scene,
    Engine::Core::Object* layerObject, uint64_t placementId)
{
    auto* layer = layerObject ? layerObject->GetComponent<TerrainObjectLayer>() : nullptr;
    if (!layer || !placementId) return nullptr;
    for (auto* child : layerObject->Children)
        if (auto* handle = child ? child->GetComponent<TerrainPlacementHandle>() : nullptr;
            handle && handle->placementId == placementId)
            return handle->PullFromPlacement() ? child : nullptr;

    layer->GetPlacementSignature();
    const auto found = std::find_if(layer->placements.begin(), layer->placements.end(),
        [&](const auto& placement) { return placement.id == placementId; });
    if (found == layer->placements.end()) return nullptr;

    auto* proxy = scene.AddObject("Placement: " +
        std::filesystem::path(found->asset).stem().string());
    proxy->runtimeOnly = true;
    if (!scene.SetParentLocal(proxy, layerObject))
    {
        scene.RemoveObject(proxy);
        return nullptr;
    }
    auto* handle = proxy->AddComponent<TerrainPlacementHandle>();
    handle->placementId = placementId;
    handle->m_indexHint = static_cast<size_t>(found - layer->placements.begin());
    if (!handle->PullFromPlacement())
    {
        scene.RemoveObject(proxy);
        return nullptr;
    }
    return proxy;
}
}
