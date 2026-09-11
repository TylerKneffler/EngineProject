#pragma once

#include <glm/glm.hpp>
#include <string>

namespace Engine::Scene::Spatial
{
enum class WarpVolumeShape : int
{
    Infinite = 0,
    Box = 1,
    Sphere = 2
};

enum class SpaceWarpType : int
{
    Affine = 0,
    Spiral = 1,
    Formula = 2
};

struct WarpVolumeDefinition
{
    bool enabled = true;
    bool definesVolume = false;
    WarpVolumeShape shape = WarpVolumeShape::Infinite;
    glm::vec3 size { 10.f };
    float radius = 5.f;
    float boundaryFalloff = 0.f;
    SpaceWarpType warpType = SpaceWarpType::Affine;
    glm::vec3 translation { 0.f };
    glm::vec3 rotation { 0.f };
    glm::vec3 scale { 1.f };
    glm::vec3 spiralAxis { 0.f, 1.f, 0.f };
    float spiralRadiansPerUnit = 0.5f;
    std::string formulaX { "x" };
    std::string formulaY { "y" };
    std::string formulaZ { "z" };
    float formulaA = 1.f;
    float formulaB = 1.f;
    float formulaC = 1.f;
    float formulaD = 0.f;
};

// Stateless evaluator for one bounded spatial chart. Component lifecycle and
// serialization stay outside this class so rendering, physics, and tests can
// share the same mapping implementation.
class WarpVolume final
{
public:
    static glm::mat4 BuildOverlayMatrix(const WarpVolumeDefinition& definition);
    static bool Contains(const WarpVolumeDefinition& definition,
        const glm::mat4& volumeWorld, const glm::vec3& worldPoint);
    static glm::vec3 MapPoint(const WarpVolumeDefinition& definition,
        const glm::mat4& volumeWorld, const glm::vec3& worldPoint);
};
}
