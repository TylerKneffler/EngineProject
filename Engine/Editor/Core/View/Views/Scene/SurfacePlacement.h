#pragma once
#include <glm/glm.hpp>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Editor
{
inline glm::quat SurfacePlacementRotation(const glm::vec3& normal,
    const glm::quat& authoredRotation)
{
    const auto up = glm::normalize(normal);
    const auto axis = glm::cross(glm::vec3(0.f, 1.f, 0.f), up);
    const auto align = up.y < -.999999f ? glm::quat(0.f, 1.f, 0.f, 0.f) :
        glm::normalize(glm::quat(1.f + up.y, axis.x, axis.y, axis.z));
    return glm::normalize(align * authoredRotation);
}

// The ray direction need not be normalized; distance retains its parameterization.
inline bool IntersectPlacementTriangle(const glm::dvec3& origin,
    const glm::dvec3& direction, const glm::dvec3& a,
    const glm::dvec3& b, const glm::dvec3& c, double& distance,
    glm::dvec3& normal)
{
    const auto edge1 = b - a, edge2 = c - a;
    const auto p = glm::cross(direction, edge2);
    const double determinant = glm::dot(edge1, p);
    if (std::abs(determinant) < 1.e-12) return false;
    const auto offset = origin - a;
    const double u = glm::dot(offset, p) / determinant;
    if (u < 0.0 || u > 1.0) return false;
    const auto q = glm::cross(offset, edge1);
    const double v = glm::dot(direction, q) / determinant;
    if (v < 0.0 || u + v > 1.0) return false;
    const double hit = glm::dot(edge2, q) / determinant;
    if (hit < 0.0 || hit >= distance) return false;
    distance = hit;
    normal = glm::normalize(glm::cross(edge1, edge2));
    return true;
}
}
