#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine::Scene
{
namespace
{
struct OrderedVolume
{
    const Engine::Components::SpatialManipulator* manipulator = nullptr;
    Scene::ObjectPath path;
};

std::vector<OrderedVolume> GatherOrderedVolumes(const Scene& scene,
    const Scene::SpatialQuery& query)
{
    std::vector<OrderedVolume> volumes;
    if (!query.includeWarpVolumes)
        return volumes;

    // Scene owns every object in one flat list; Children is a non-owning
    // hierarchy view over the same entries. Recursing from every list entry
    // revisits descendants once per ancestor and can apply a nested volume
    // multiple times.
    for (const auto& owned : scene.GetObjects())
    {
        const Engine::Core::Object* object = owned.get();
        if (!object || object == query.excludedOwner ||
            !object->IsEnabledInHierarchy())
            continue;
        const auto* manipulator =
            object->GetComponent<Engine::Components::SpatialManipulator>();
        if (!manipulator || !manipulator->enabled ||
            !manipulator->definesWarpVolume)
            continue;
        Scene::ObjectPath path;
        scene.TryGetObjectPath(object, path);
        volumes.push_back({ manipulator, std::move(path) });
    }
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
    return volumes;
}

glm::vec3 MapPoint(const glm::vec3& worldPoint,
    const std::vector<OrderedVolume>& volumes, bool& affectedByWarpVolume)
{
    glm::vec3 mapped = worldPoint;
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
    const std::vector<OrderedVolume> volumes = GatherOrderedVolumes(*this, query);
    sample.point = MapPoint(worldPoint, volumes,
        sample.affectedByWarpVolume);
    // Preserve the exact identity transform when no volume can affect this
    // query. Numerically differentiating identity with a 0.001 step loses
    // precision at large coordinates (for example 0.9765625 instead of 1),
    // which scales each separately rendered chunk around its own origin and
    // opens multi-unit gaps between otherwise matching meshes.
    if (!query.includeWarpVolumes || volumes.empty())
        return sample;

    // Differentiate the fully composed mapping, not individual volume
    // transforms. This preserves nonlinear behavior when volumes overlap.
    constexpr float kDerivativeStep = 0.001f;
    for (int column = 0; column < 3; ++column)
    {
        bool endpointAffected = false;
        const glm::vec3 endpoint = MapPoint(
            worldPoint + glm::vec3(column == 0, column == 1, column == 2) *
                kDerivativeStep,
            volumes, endpointAffected);
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
    bool affectedByWarpVolume = false;
    return MapPoint(worldPoint, GatherOrderedVolumes(*this, query),
        affectedByWarpVolume);
}

bool Scene::TryUnmapSpatialPoint(const glm::vec3& mappedPoint,
    glm::vec3& worldPoint, const SpatialQuery& query) const
{
    if (!std::isfinite(mappedPoint.x) || !std::isfinite(mappedPoint.y) ||
        !std::isfinite(mappedPoint.z))
        return false;

    constexpr int kMaximumIterations = 24;
    constexpr float kPositionTolerance = 0.0001f;
    constexpr float kMinimumDeterminant = 1e-7f;
    glm::vec3 estimate = mappedPoint;
    float previousError = std::numeric_limits<float>::infinity();

    for (int iteration = 0; iteration < kMaximumIterations; ++iteration)
    {
        const SpatialQuerySample sample = SampleSpatialPoint(estimate, query);
        const glm::vec3 residual = sample.point - mappedPoint;
        const float error = glm::length(residual);
        if (std::isfinite(error) && error <= kPositionTolerance)
        {
            worldPoint = estimate;
            return true;
        }
        const float determinant = glm::determinant(sample.jacobian);
        if (!std::isfinite(error) || !std::isfinite(determinant) ||
            std::abs(determinant) <= kMinimumDeterminant)
            return false;

        glm::vec3 step = glm::inverse(sample.jacobian) * residual;
        const float stepLength = glm::length(step);
        if (!std::isfinite(stepLength))
            return false;
        if (stepLength > 5.f)
            step *= 5.f / stepLength;

        // Backtracking keeps the solve stable near blended finite-volume
        // boundaries, where the active mapping can change during an update.
        float damping = error > previousError ? 0.5f : 1.f;
        glm::vec3 bestEstimate = estimate;
        float bestError = error;
        for (int attempt = 0; attempt < 7; ++attempt)
        {
            const glm::vec3 candidate = estimate - step * damping;
            const float candidateError = glm::length(
                MapSpatialPoint(candidate, query) - mappedPoint);
            if (std::isfinite(candidateError) && candidateError < bestError)
            {
                bestEstimate = candidate;
                bestError = candidateError;
                break;
            }
            damping *= 0.5f;
        }
        if (bestError >= error)
            return false;
        estimate = bestEstimate;
        previousError = bestError;
    }

    const float finalError = glm::length(
        MapSpatialPoint(estimate, query) - mappedPoint);
    if (!std::isfinite(finalError) || finalError > kPositionTolerance)
        return false;
    worldPoint = estimate;
    return true;
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
        // Scene owns every hierarchy node in one flat list. Walking Children
        // from every entry would revisit a descendant once per ancestor.
        for (const auto& owned : GetObjects())
        {
            const Engine::Core::Object* object = owned.get();
            if (!object || object == source->Owner || reciprocal)
                continue;
            auto* candidate = object->GetComponent<
                Engine::Components::SpatialManipulator>();
            if (candidate && candidate->ResolveTarget() == source)
                reciprocal = candidate;
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
            for (const auto& owned : GetObjects())
            {
                const Engine::Core::Object* object = owned.get();
                if (!object || !object->IsEnabledInHierarchy())
                    continue;
                auto* source = object->GetComponent<
                    Engine::Components::SpatialManipulator>();
                if (!source || !source->enabled)
                    continue;
                    const auto mode = static_cast<Engine::Components::
                        SpatialManipulator::ConnectionMode>(source->connectionMode);
                    if (mode != Engine::Components::SpatialManipulator::ConnectionMode::Portal &&
                        mode != Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
                        continue;
                    auto* target = resolveTarget(source);
                    if (!target || !target->enabled || !target->Owner ||
                        !source->HasCompatiblePortalShapeWith(*target))
                    {
                        continue;
                    }
                    const PortalEdge edge { source, target };
                    if (std::find_if(portalPath.begin(), portalPath.end(),
                        [&](const PortalEdge& prior)
                        {
                            return prior.source == edge.source &&
                                prior.target == edge.target;
                        }) != portalPath.end())
                    {
                        continue;
                    }

                    const glm::mat4 sourceFrame = source->GetPortalWorldFrame();
                    const glm::vec3 planePoint(sourceFrame[3]);
                    const glm::vec3 planeNormal = glm::normalize(
                        glm::vec3(sourceFrame[2]));
                    const float denominator = glm::dot(current.direction, planeNormal);
                    if (std::abs(denominator) <= 1e-6f)
                        continue;
                    const float distance = glm::dot(planePoint - current.origin,
                        planeNormal) / denominator;
                    if (!std::isfinite(distance) || distance <= kRayEpsilon ||
                        distance >= nearestDistance)
                    {
                        continue;
                    }
                    const glm::vec3 hit = current.origin + current.direction * distance;
                    if (!source->IsWorldPointInsidePortalAperture(hit, 0.001f))
                        continue;
                    nearestSource = source;
                    nearestTarget = target;
                    nearestDistance = distance;
            }
        }

        segments.push_back({ current, nearestDistance,
            nearestSource ? nearestSource->Owner : nullptr });
        if (!nearestSource || !nearestTarget)
            break;

        const glm::vec3 sourceHit = current.origin +
            current.direction * nearestDistance;
        const glm::vec3 mappedDirection =
            nearestSource->MapWorldDirectionThroughPortalShape(sourceHit,
                current.direction, *nearestTarget);
        const float mappedLength = glm::length(mappedDirection);
        if (!std::isfinite(mappedLength) || mappedLength <= 1e-6f)
            break;
        current.direction = mappedDirection / mappedLength;
        current.origin = nearestSource->MapWorldPointThroughPortalShape(
            sourceHit, *nearestTarget) + current.direction * kRayEpsilon;
        remainingDistance -= nearestDistance;
        portalPath.push_back({ nearestSource, nearestTarget });
        if (remainingDistance <= kRayEpsilon)
            break;
    }
    return segments;
}
}
