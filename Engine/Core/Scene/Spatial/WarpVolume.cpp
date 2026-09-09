#include "WarpVolume.h"

#include "Core/Math/FormulaExpression.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Scene::Spatial
{
namespace
{
float Clamp01(float value)
{
    return std::max(0.f, std::min(1.f, value));
}

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared <= 1e-8f ? fallback : value / std::sqrt(lengthSquared);
}
}

glm::mat4 WarpVolume::BuildOverlayMatrix(
    const WarpVolumeDefinition& definition)
{
    const glm::mat4 translation = glm::translate(glm::mat4(1.f),
        definition.translation);
    const glm::mat4 rotationX = glm::rotate(glm::mat4(1.f),
        definition.rotation.x, { 1.f, 0.f, 0.f });
    const glm::mat4 rotationY = glm::rotate(glm::mat4(1.f),
        definition.rotation.y, { 0.f, 1.f, 0.f });
    const glm::mat4 rotationZ = glm::rotate(glm::mat4(1.f),
        definition.rotation.z, { 0.f, 0.f, 1.f });
    return translation * rotationZ * rotationY * rotationX *
        glm::scale(glm::mat4(1.f), definition.scale);
}

bool WarpVolume::Contains(const WarpVolumeDefinition& definition,
    const glm::mat4& volumeWorld, const glm::vec3& worldPoint)
{
    if (!definition.enabled || !definition.definesVolume)
        return false;

    const glm::vec3 localPoint = glm::vec3(glm::inverse(volumeWorld) *
        glm::vec4(worldPoint, 1.f));
    switch (definition.shape)
    {
    case WarpVolumeShape::Box:
    {
        const glm::vec3 halfSize = glm::max(glm::abs(definition.size) * 0.5f,
            glm::vec3(0.0001f));
        return std::abs(localPoint.x) <= halfSize.x &&
            std::abs(localPoint.y) <= halfSize.y &&
            std::abs(localPoint.z) <= halfSize.z;
    }
    case WarpVolumeShape::Sphere:
    {
        const float radius = std::max(0.001f, std::abs(definition.radius));
        return glm::dot(localPoint, localPoint) <= radius * radius;
    }
    case WarpVolumeShape::Infinite:
    default:
        return true;
    }
}

glm::vec3 WarpVolume::MapPoint(const WarpVolumeDefinition& definition,
    const glm::mat4& volumeWorld, const glm::vec3& worldPoint)
{
    if (!Contains(definition, volumeWorld, worldPoint))
        return worldPoint;

    const glm::vec3 localPoint = glm::vec3(glm::inverse(volumeWorld) *
        glm::vec4(worldPoint, 1.f));
    glm::vec3 mappedLocal = localPoint;
    if (definition.warpType == SpaceWarpType::Formula)
    {
        const Engine::Math::FormulaVariables variables {
            localPoint.x, localPoint.y, localPoint.z, definition.formulaA,
            definition.formulaB, definition.formulaC, definition.formulaD
        };
        double mappedX = 0.0;
        double mappedY = 0.0;
        double mappedZ = 0.0;
        if (!Engine::Math::EvaluateFormula(definition.formulaX, variables,
                mappedX) ||
            !Engine::Math::EvaluateFormula(definition.formulaY, variables,
                mappedY) ||
            !Engine::Math::EvaluateFormula(definition.formulaZ, variables,
                mappedZ))
        {
            return worldPoint;
        }
        mappedLocal = glm::vec3(BuildOverlayMatrix(definition) * glm::vec4(
            static_cast<float>(mappedX), static_cast<float>(mappedY),
            static_cast<float>(mappedZ), 1.f));
        if (!std::isfinite(mappedLocal.x) || !std::isfinite(mappedLocal.y) ||
            !std::isfinite(mappedLocal.z))
        {
            return worldPoint;
        }
    }
    else
    {
        mappedLocal = glm::vec3(BuildOverlayMatrix(definition) *
            glm::vec4(localPoint, 1.f));
    }

    if (definition.warpType == SpaceWarpType::Spiral)
    {
        const glm::vec3 axis = SafeNormalize(definition.spiralAxis,
            glm::vec3(0.f, 1.f, 0.f));
        const float angle = glm::dot(localPoint, axis) *
            definition.spiralRadiansPerUnit;
        mappedLocal = glm::vec3(glm::rotate(glm::mat4(1.f), angle, axis) *
            glm::vec4(mappedLocal, 1.f));
    }

    const float falloff = std::max(0.f, definition.boundaryFalloff);
    if (falloff > 0.f)
    {
        float boundaryDistance = falloff;
        switch (definition.shape)
        {
        case WarpVolumeShape::Box:
        {
            const glm::vec3 halfSize = glm::max(glm::abs(definition.size) *
                0.5f, glm::vec3(0.0001f));
            const glm::vec3 remaining = halfSize - glm::abs(localPoint);
            boundaryDistance = std::min(remaining.x,
                std::min(remaining.y, remaining.z));
            break;
        }
        case WarpVolumeShape::Sphere:
            boundaryDistance = std::max(0.f, std::abs(definition.radius) -
                glm::length(localPoint));
            break;
        case WarpVolumeShape::Infinite:
        default:
            break;
        }
        mappedLocal = glm::mix(localPoint, mappedLocal,
            Clamp01(boundaryDistance / falloff));
    }

    return glm::vec3(volumeWorld * glm::vec4(mappedLocal, 1.f));
}
}
