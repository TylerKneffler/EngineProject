#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }
namespace Engine::Components { class Terrain; class TerrainManager; }

namespace Engine::Editor
{
// Terrain editing helpers for the scene view's terrain tools: surface
// sampling under the brush, the projected brush ring, and object scatter /
// erase strokes. Brush positions and radii are in the target root's local
// space (the manager's object, or the selected Terrain cell), matching
// Terrain::ApplyBrush and TerrainManager::ApplyBrush.
class SceneTerrainTools
{
public:
    struct Target
    {
        Engine::Components::Terrain* terrain = nullptr;      // selected cell (no manager)
        Engine::Components::TerrainManager* manager = nullptr;
        Engine::Core::Object* root = nullptr;                 // manager owner or terrain owner
    };

    // Surface point under root-local X/Z, in world space, with the cell that
    // holds it. False outside loaded terrain.
    static bool SampleSurface(const Target& target, double localX, double localZ,
        glm::dvec3& world, glm::vec3& worldNormal,
        Engine::Components::Terrain** cell = nullptr);

    // Root-local position of a world point.
    static glm::dvec3 ToLocal(const Target& target, const glm::dvec3& world);

    // Ring of the brush draped over the surface; an inner ring marks where the
    // falloff starts. Points behind the camera break the outline.
    static void DrawBrushRing(const Target& target, const glm::dvec3& localCenter,
        float radius, float hardness, const glm::dmat4& viewProjection,
        const glm::vec2& screenMin, const glm::vec2& screenSize, uint32_t color);

    struct ScatterSettings
    {
        std::string prefab;
        int count = 1;              // objects per dab
        bool alignToSurface = true;
        bool randomYaw = true;
        float minScale = 1.f, maxScale = 1.f;
    };
    // Places up to settings.count objects at random points inside the brush
    // (the first dab of a single-object brush lands at the center). Returns
    // the number placed.
    static int Scatter(Engine::Scene::Scene& scene, const Target& target,
        const glm::dvec3& localCenter, float radius,
        const ScatterSettings& settings, uint32_t& random);

    // Removes placements inside the brush. With a prefab, only that asset;
    // without one, any placement except ground-cover layers (snapToGround or
    // conformToGround, such as generated grass). Returns the count removed.
    static int Erase(const Target& target, const glm::dvec3& localCenter,
        float radius, const std::string& prefab);
};
}
