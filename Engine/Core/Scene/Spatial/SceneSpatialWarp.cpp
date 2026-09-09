#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace Engine::Scene
{
namespace
{
void VisitObjectTree(const Engine::Core::Object* object,
    const std::function<void(const Engine::Core::Object*)>& visitor)
{
    if (!object)
        return;
    visitor(object);
    for (const Engine::Core::Object* child : object->Children)
        VisitObjectTree(child, visitor);
}
}

namespace
{
glm::vec3 MapPoint(const Scene& scene, const glm::vec3& worldPoint,
    const Scene::SpatialQuery& query, bool& affectedByWarpVolume)
{
    glm::vec3 mapped = worldPoint;
    if (!query.includeWarpVolumes)
        return mapped;

    struct OrderedVolume
    {
        const Engine::Components::SpatialManipulator* manipulator = nullptr;
        Scene::ObjectPath path;
    };
    std::vector<OrderedVolume> volumes;
    for (const auto& root : scene.GetObjects())
        VisitObjectTree(root.get(), [&](const Engine::Core::Object* object)
        {
            if (!object || object == query.excludedOwner ||
                !object->IsEnabledInHierarchy())
                return;
            const auto* manipulator =
                object->GetComponent<Engine::Components::SpatialManipulator>();
            if (!manipulator || !manipulator->enabled ||
                !manipulator->definesWarpVolume)
                return;
            Scene::ObjectPath path;
            scene.TryGetObjectPath(object, path);
            volumes.push_back({ manipulator, std::move(path) });
        });
    // Composition is explicit: lower priorities map first, higher priorities
    // map last. Equal priorities use the persisted hierarchy path, so moving
    // unrelated scene objects cannot silently change nonlinear results.
    std::sort(volumes.begin(), volumes.end(),
        [](const OrderedVolume& first, const OrderedVolume& second)
        {
            if (first.manipulator->warpPriority !=
                second.manipulator->warpPriority)
            {
                return first.manipulator->warpPriority <
                    second.manipulator->warpPriority;
            }
            return first.path < second.path;
        });
    for (const OrderedVolume& volume : volumes)
    {
            const glm::vec3 before = mapped;
            mapped = volume.manipulator->MapWorldPointThroughVolume(mapped);
            const glm::vec3 delta = mapped - before;
            affectedByWarpVolume = affectedByWarpVolume ||
                glm::dot(delta, delta) > 1e-12f;
    }
    return mapped;
}
}

Scene::SpatialQuerySample Scene::SampleSpatialPoint(
    const glm::vec3& worldPoint, const SpatialQuery& query) const
{
    SpatialQuerySample sample{};
    sample.point = MapPoint(*this, worldPoint, query,
        sample.affectedByWarpVolume);
    if (!query.includeWarpVolumes)
        return sample;

    // Differentiate the fully composed mapping, not individual volume
    // transforms. This preserves nonlinear behavior when volumes overlap.
    constexpr float kDerivativeStep = 0.001f;
    for (int column = 0; column < 3; ++column)
    {
        bool endpointAffected = false;
        const glm::vec3 endpoint = MapPoint(*this,
            worldPoint + glm::vec3(column == 0, column == 1, column == 2) *
                kDerivativeStep,
            query, endpointAffected);
        glm::vec3 derivative = (endpoint - sample.point) / kDerivativeStep;
        if (!std::isfinite(derivative.x) || !std::isfinite(derivative.y) ||
            !std::isfinite(derivative.z))
        {
            derivative = glm::vec3(column == 0, column == 1, column == 2);
        }
        sample.jacobian[column] = derivative;
        sample.affectedByWarpVolume = sample.affectedByWarpVolume ||
            endpointAffected;
    }
    return sample;
}

glm::vec3 Scene::MapSpatialPoint(const glm::vec3& worldPoint,
    const SpatialQuery& query) const
{
    return SampleSpatialPoint(worldPoint, query).point;
}

glm::vec3 Scene::WarpWorldPoint(const glm::vec3& worldPoint,
    const Object* excludedOwner) const
{
    return MapSpatialPoint(worldPoint,
        { SpatialQueryDomain::Gameplay, excludedOwner });
}

glm::mat4 Scene::MapSpatialMatrix(const glm::mat4& worldMatrix,
    const SpatialQuery& query) const
{
    const glm::vec3 origin(worldMatrix[3]);
    const SpatialQuerySample sample = SampleSpatialPoint(origin, query);

    glm::mat4 mapped(1.f);
    for (int column = 0; column < 3; ++column)
    {
        const glm::vec3 basis(worldMatrix[column]);
        glm::vec3 mappedBasis = sample.jacobian * basis;
        if (!std::isfinite(mappedBasis.x) || !std::isfinite(mappedBasis.y) ||
            !std::isfinite(mappedBasis.z))
            mappedBasis = basis;
        mapped[column] = glm::vec4(mappedBasis, 0.f);
    }
    mapped[3] = glm::vec4(sample.point, 1.f);
    return mapped;
}

glm::mat4 Scene::WarpWorldMatrix(const glm::mat4& worldMatrix,
    const Object* excludedOwner) const
{
    return MapSpatialMatrix(worldMatrix,
        { SpatialQueryDomain::Gameplay, excludedOwner });
}

Scene::SpatialRay Scene::MapSpatialRay(const SpatialRay& ray,
    const SpatialQuery& query) const
{
    const SpatialQuerySample sample = SampleSpatialPoint(ray.origin, query);
    SpatialRay mapped{};
    mapped.origin = sample.point;
    const glm::vec3 mappedDirection = sample.jacobian * ray.direction;
    const float length = glm::length(mappedDirection);
    mapped.direction = length > 1e-6f ? mappedDirection / length :
        glm::vec3(0.f, 0.f, 1.f);
    return mapped;
}

std::vector<Scene::PortalRaySegment> Scene::TracePortalRay(
    const SpatialRay& ray, float maxDistance, uint32_t maxPortalHops) const
{
    std::vector<PortalRaySegment> segments;
    if (!std::isfinite(maxDistance) || maxDistance <= 0.f)
        return segments;

    const float directionLength = glm::length(ray.direction);
    if (!std::isfinite(directionLength) || directionLength <= 1e-6f)
        return segments;

    struct PortalEdge
    {
        const Engine::Components::SpatialManipulator* source = nullptr;
        const Engine::Components::SpatialManipulator* target = nullptr;
    };
    std::vector<PortalEdge> portalPath;
    SpatialRay current { ray.origin, ray.direction / directionLength };
    float remainingDistance = maxDistance;
    constexpr float kRayEpsilon = 0.001f;

    const auto resolveTarget = [this](
        const Engine::Components::SpatialManipulator* source)
        -> const Engine::Components::SpatialManipulator*
    {
        if (!source)
            return nullptr;
        if (auto* target = source->ResolveTarget())
            return target;

        const Engine::Components::SpatialManipulator* reciprocal = nullptr;
        for (const auto& root : GetObjects())
        {
            VisitObjectTree(root.get(), [&](const Engine::Core::Object* object)
            {
                if (!object || object == source->Owner || reciprocal)
                    return;
                auto* candidate = object->GetComponent<
                    Engine::Components::SpatialManipulator>();
                if (candidate && candidate->ResolveTarget() == source)
                    reciprocal = candidate;
            });
        }
        return reciprocal;
    };

    // Always emit the current ordinary-space segment.  Reaching the portal
    // hop budget merely stops further remapping; it must not make the tail of
    // a ray disappear (and a zero budget is still a valid ordinary ray).
    for (uint32_t hop = 0u;; ++hop)
    {
        const Engine::Components::SpatialManipulator* nearestSource = nullptr;
        const Engine::Components::SpatialManipulator* nearestTarget = nullptr;
        float nearestDistance = remainingDistance;

        if (hop < maxPortalHops)
        {
            for (const auto& root : GetObjects())
            {
                VisitObjectTree(root.get(), [&](const Engine::Core::Object* object)
                {
                    if (!object || !object->IsEnabledInHierarchy())
                        return;
                    auto* source = object->GetComponent<
                        Engine::Components::SpatialManipulator>();
                    if (!source || !source->enabled)
                        return;
                    const auto mode = static_cast<Engine::Components::
                        SpatialManipulator::ConnectionMode>(source->connectionMode);
                    if (mode != Engine::Components::SpatialManipulator::ConnectionMode::Portal &&
                        mode != Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
                        return;
                    auto* target = resolveTarget(source);
                    if (!target || !target->enabled || !target->Owner ||
                        !source->HasCompatiblePortalShapeWith(*target))
                    {
                        return;
                    }
                    const PortalEdge edge { source, target };
                    if (std::find_if(portalPath.begin(), portalPath.end(),
                        [&](const PortalEdge& prior)
                        {
                            return prior.source == edge.source &&
                                prior.target == edge.target;
                        }) != portalPath.end())
                    {
                        return;
                    }

                    const glm::mat4 sourceFrame = source->GetPortalWorldFrame();
                    const glm::vec3 planePoint(sourceFrame[3]);
                    const glm::vec3 planeNormal = glm::normalize(
                        glm::vec3(sourceFrame[2]));
                    const float denominator = glm::dot(current.direction, planeNormal);
                    if (std::abs(denominator) <= 1e-6f)
                        return;
                    const float distance = glm::dot(planePoint - current.origin,
                        planeNormal) / denominator;
                    if (!std::isfinite(distance) || distance <= kRayEpsilon ||
                        distance >= nearestDistance)
                    {
                        return;
                    }
                    const glm::vec3 hit = current.origin + current.direction * distance;
                    if (!source->IsWorldPointInsidePortalAperture(hit, 0.001f))
                        return;
                    nearestSource = source;
                    nearestTarget = target;
                    nearestDistance = distance;
                });
            }
        }

        segments.push_back({ current, nearestDistance,
            nearestSource ? nearestSource->Owner : nullptr });
        if (!nearestSource || !nearestTarget)
            break;

        const glm::mat4 sourceToTarget =
            nearestSource->GetPortalWorldTransformTo(*nearestTarget);
        const glm::vec3 sourceHit = current.origin +
            current.direction * nearestDistance;
        const glm::vec3 mappedDirection = glm::vec3(sourceToTarget *
            glm::vec4(current.direction, 0.f));
        const float mappedLength = glm::length(mappedDirection);
        if (!std::isfinite(mappedLength) || mappedLength <= 1e-6f)
            break;
        current.direction = mappedDirection / mappedLength;
        current.origin = glm::vec3(sourceToTarget * glm::vec4(sourceHit, 1.f)) +
            current.direction * kRayEpsilon;
        remainingDistance -= nearestDistance;
        portalPath.push_back({ nearestSource, nearestTarget });
        if (remainingDistance <= kRayEpsilon)
            break;
    }
    return segments;
}
}
