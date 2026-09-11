#include "PortalAperture.h"

#include <algorithm>
#include <cmath>

namespace Engine::Scene::Spatial
{
namespace
{
glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared <= 1e-8f ? fallback : value / std::sqrt(lengthSquared);
}

void BuildBasis(const glm::vec3& normal, glm::vec3& tangent,
    glm::vec3& bitangent, glm::vec3& normalized)
{
    normalized = SafeNormalize(normal, glm::vec3(0.f, 0.f, 1.f));
    const glm::vec3 reference = std::abs(normalized.z) < 0.999f
        ? glm::vec3(0.f, 0.f, 1.f)
        : glm::vec3(0.f, 1.f, 0.f);
    tangent = SafeNormalize(glm::cross(reference, normalized),
        glm::vec3(1.f, 0.f, 0.f));
    bitangent = SafeNormalize(glm::cross(normalized, tangent),
        glm::vec3(0.f, 1.f, 0.f));
}
}

glm::mat4 PortalAperture::BuildFrame(const glm::mat4& ownerWorld,
    const glm::vec3& localAnchor, const glm::vec3& localNormal)
{
    glm::vec3 localTangent(1.f, 0.f, 0.f);
    glm::vec3 localBitangent(0.f, 1.f, 0.f);
    glm::vec3 normalizedLocalNormal(0.f, 0.f, 1.f);
    BuildBasis(localNormal, localTangent, localBitangent,
        normalizedLocalNormal);

    const glm::mat3 ownerLinear(ownerWorld);
    const glm::vec3 normal = SafeNormalize(
        glm::transpose(glm::inverse(ownerLinear)) * normalizedLocalNormal,
        glm::vec3(0.f, 0.f, 1.f));
    glm::vec3 tangentCandidate = ownerLinear * localTangent;
    tangentCandidate -= normal * glm::dot(tangentCandidate, normal);
    glm::vec3 tangent = SafeNormalize(tangentCandidate, glm::vec3(0.f));
    if (glm::dot(tangent, tangent) <= 1e-8f)
    {
        glm::vec3 fallbackBitangent(0.f, 1.f, 0.f);
        glm::vec3 fallbackNormal(0.f, 0.f, 1.f);
        BuildBasis(normal, tangent, fallbackBitangent, fallbackNormal);
    }

    glm::mat4 frame(1.f);
    frame[0] = glm::vec4(tangent, 0.f);
    frame[1] = glm::vec4(SafeNormalize(glm::cross(normal, tangent),
        glm::vec3(0.f, 1.f, 0.f)), 0.f);
    frame[2] = glm::vec4(normal, 0.f);
    frame[3] = glm::vec4(glm::vec3(ownerWorld *
        glm::vec4(localAnchor, 1.f)), 1.f);
    return frame;
}

std::vector<glm::vec3> PortalAperture::BuildWorldPoints(
    const std::vector<glm::vec3>& localShape, const glm::mat4& ownerWorld,
    const glm::vec3& localAnchor, const glm::vec3& localNormal)
{
    glm::vec3 tangent(1.f, 0.f, 0.f);
    glm::vec3 bitangent(0.f, 1.f, 0.f);
    glm::vec3 normal(0.f, 0.f, 1.f);
    BuildBasis(localNormal, tangent, bitangent, normal);
    std::vector<glm::vec3> result;
    result.reserve(localShape.size());
    for (const glm::vec3& shapePoint : localShape)
    {
        const glm::vec3 localPoint = localAnchor + tangent * shapePoint.x +
            bitangent * shapePoint.y + normal * shapePoint.z;
        result.push_back(glm::vec3(ownerWorld * glm::vec4(localPoint, 1.f)));
    }
    return result;
}

float PortalAperture::Area(const std::vector<glm::vec3>& worldPoints)
{
    if (worldPoints.size() < 3u)
        return 0.f;
    glm::vec3 center(0.f);
    for (const glm::vec3& point : worldPoints)
        center += point;
    center /= static_cast<float>(worldPoints.size());
    glm::vec3 twiceArea(0.f);
    for (size_t index = 0; index < worldPoints.size(); ++index)
    {
        twiceArea += glm::cross(worldPoints[index] - center,
            worldPoints[(index + 1u) % worldPoints.size()] - center);
    }
    return 0.5f * glm::length(twiceArea);
}

bool PortalAperture::IsValid(const std::vector<glm::vec3>& worldPoints,
    const glm::mat4& frame, float tolerance)
{
    if (worldPoints.size() < 3u)
        return false;
    const glm::vec3 tangent(frame[0]);
    const glm::vec3 bitangent(frame[1]);
    const glm::vec3 normal(frame[2]);
    const glm::vec3 anchor(frame[3]);
    const float epsilon = std::max(1e-6f, tolerance);
    std::vector<glm::vec2> projected;
    projected.reserve(worldPoints.size());
    for (const glm::vec3& point : worldPoints)
    {
        const glm::vec3 relative = point - anchor;
        if (std::abs(glm::dot(relative, normal)) > epsilon)
            return false;
        projected.emplace_back(glm::dot(relative, tangent),
            glm::dot(relative, bitangent));
    }

    float winding = 0.f;
    for (size_t index = 0; index < projected.size(); ++index)
    {
        const glm::vec2& a = projected[index];
        const glm::vec2& b = projected[(index + 1u) % projected.size()];
        const glm::vec2& c = projected[(index + 2u) % projected.size()];
        const glm::vec2 edge = b - a;
        if (glm::dot(edge, edge) <= epsilon * epsilon)
            return false;
        const float turn = edge.x * (c.y - b.y) - edge.y * (c.x - b.x);
        if (std::abs(turn) <= epsilon)
            return false;
        if (winding == 0.f)
            winding = turn;
        else if (turn * winding <= 0.f)
            return false;
    }
    return true;
}

bool PortalAperture::Contains(const glm::vec3& worldPoint,
    const std::vector<glm::vec3>& worldPoints, const glm::mat4& frame,
    float margin)
{
    if (!IsValid(worldPoints, frame))
        return false;
    const glm::vec3 tangent(frame[0]);
    const glm::vec3 bitangent(frame[1]);
    const glm::vec3 normal(frame[2]);
    const glm::vec3 anchor(frame[3]);
    const glm::vec3 relative = worldPoint - anchor;
    if (std::abs(glm::dot(relative, normal)) > std::max(0.0005f, margin))
        return false;
    const glm::vec2 point(glm::dot(relative, tangent),
        glm::dot(relative, bitangent));
    float winding = 0.f;
    for (size_t index = 0; index < worldPoints.size(); ++index)
    {
        const glm::vec3 aWorld = worldPoints[index] - anchor;
        const glm::vec3 bWorld = worldPoints[(index + 1u) % worldPoints.size()] -
            anchor;
        const glm::vec2 a(glm::dot(aWorld, tangent),
            glm::dot(aWorld, bitangent));
        const glm::vec2 b(glm::dot(bWorld, tangent),
            glm::dot(bWorld, bitangent));
        const glm::vec2 edge = b - a;
        const glm::vec2 offset = point - a;
        const float cross = edge.x * offset.y - edge.y * offset.x;
        if (index == 0u)
            winding = cross >= 0.f ? 1.f : -1.f;
        if (winding * cross < -std::max(0.f, margin) * glm::length(edge))
            return false;
    }
    return true;
}
}
