#include "Core/Scene/Scene.h"
#include "Core/Compoonents/SpatialManipulator.h"
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
            const glm::vec3 before = mapped;
            mapped = manipulator->MapWorldPointThroughVolume(mapped);
            const glm::vec3 delta = mapped - before;
            affectedByWarpVolume = affectedByWarpVolume ||
                glm::dot(delta, delta) > 1e-12f;
        });
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
}
