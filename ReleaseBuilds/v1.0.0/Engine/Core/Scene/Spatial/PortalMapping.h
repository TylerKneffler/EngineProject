#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace Engine::Scene::Spatial
{
// Maps positions and differential vectors between two compatible apertures.
// Similar apertures use one affine transform; mismatched shapes use a stable
// piecewise map while retaining the affine transform as their fallback.
class PortalMapping final
{
public:
    static float ComputeScaleRatio(
        const std::vector<glm::vec3>& sourceWorldPoints,
        const std::vector<glm::vec3>& targetWorldPoints);
    static glm::mat4 BuildAffineTransform(
        const std::vector<glm::vec3>& sourceWorldPoints,
        const std::vector<glm::vec3>& targetWorldPoints,
        const glm::mat4& sourceFrame, const glm::mat4& targetFrame);

    PortalMapping(const std::vector<glm::vec3>& sourceWorldPoints,
        const std::vector<glm::vec3>& targetWorldPoints,
        const glm::mat4& sourceFrame, const glm::mat4& targetFrame);

    bool IsValid() const;
    bool IsPiecewise() const;
    float ScaleRatio() const;
    const glm::mat4& AffineTransform() const;
    glm::vec3 MapPoint(const glm::vec3& worldPoint) const;
    glm::vec3 MapDirection(const glm::vec3& origin,
        const glm::vec3& direction) const;
    glm::vec3 MapNormal(const glm::vec3& origin,
        const glm::vec3& normal) const;

private:
    std::vector<glm::vec2> m_source;
    std::vector<glm::vec2> m_target;
    glm::vec2 m_sourceCenter { 0.f };
    glm::vec2 m_targetCenter { 0.f };
    glm::mat4 m_sourceFrame { 1.f };
    glm::mat4 m_targetFrame { 1.f };
    glm::mat4 m_affineTransform { 1.f };
    float m_scaleRatio = 1.f;
    bool m_valid = false;
    bool m_piecewise = false;
};
}
