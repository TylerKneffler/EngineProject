#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace Engine::Scene::Spatial
{
// Geometry-only view of a portal aperture. It deliberately has no component
// or physics ownership, which keeps aperture validation reusable by rendering,
// ray queries, traversal, and editor diagnostics.
class PortalAperture final
{
public:
    static glm::mat4 BuildFrame(const glm::mat4& ownerWorld,
        const glm::vec3& localAnchor, const glm::vec3& localNormal);
    static std::vector<glm::vec3> BuildWorldPoints(
        const std::vector<glm::vec3>& localShape, const glm::mat4& ownerWorld,
        const glm::vec3& localAnchor, const glm::vec3& localNormal);
    static float Area(const std::vector<glm::vec3>& worldPoints);
    static bool IsValid(const std::vector<glm::vec3>& worldPoints,
        const glm::mat4& frame, float tolerance = 0.0005f);
    static bool Contains(const glm::vec3& worldPoint,
        const std::vector<glm::vec3>& worldPoints, const glm::mat4& frame,
        float margin = 0.f);
};
}
