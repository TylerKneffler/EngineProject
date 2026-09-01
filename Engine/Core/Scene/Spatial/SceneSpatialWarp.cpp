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

glm::vec3 Scene::WarpWorldPoint(const glm::vec3& worldPoint,
    const Object* excludedOwner) const
{
    glm::vec3 mapped = worldPoint;
    for (const auto& root : m_objects)
        VisitObjectTree(root.get(), [&](const Engine::Core::Object* object)
        {
            if (!object || object == excludedOwner || !object->IsEnabledInHierarchy())
                return;
            const auto* manipulator =
                object->GetComponent<Engine::Components::SpatialManipulator>();
            if (!manipulator || !manipulator->enabled ||
                !manipulator->definesWarpVolume)
                return;
            mapped = manipulator->MapWorldPointThroughVolume(mapped);
        });
    return mapped;
}

glm::mat4 Scene::WarpWorldMatrix(const glm::mat4& worldMatrix,
    const Object* excludedOwner) const
{
    const glm::vec3 origin(worldMatrix[3]);
    const glm::vec3 mappedOrigin = WarpWorldPoint(origin, excludedOwner);

    glm::mat4 mapped(1.f);
    constexpr float kDerivativeStep = 0.001f;
    for (int column = 0; column < 3; ++column)
    {
        const glm::vec3 basis(worldMatrix[column]);
        const float length = glm::length(basis);
        if (length <= 1e-8f)
        {
            mapped[column] = glm::vec4(0.f);
            continue;
        }
        const glm::vec3 direction = basis / length;
        const glm::vec3 mappedEndpoint = WarpWorldPoint(
            origin + direction * kDerivativeStep, excludedOwner);
        glm::vec3 mappedBasis = (mappedEndpoint - mappedOrigin) *
            (length / kDerivativeStep);
        if (!std::isfinite(mappedBasis.x) || !std::isfinite(mappedBasis.y) ||
            !std::isfinite(mappedBasis.z))
            mappedBasis = basis;
        mapped[column] = glm::vec4(mappedBasis, 0.f);
    }
    mapped[3] = glm::vec4(mappedOrigin, 1.f);
    return mapped;
}
}
