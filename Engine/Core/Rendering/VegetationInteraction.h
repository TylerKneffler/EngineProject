#pragma once
#include <glm/glm.hpp>
#include <array>
#include <vector>
namespace Engine::Scene { class Scene; }
// Vegetation bending: capsules from moving colliders flagged bendGrass
// (PrimitiveObjectCollider), captured once per frame and narrowed to the four
// nearest the camera for ObjectGPUData::grassCapsules. Any material built with
// GRASS_CAPSULE_INTERACTION (e.g. Assets/Shaders/GrassFootBend.hlsl) bends
// around them, whether or not a GrassField placed it.
namespace Engine::Rendering
{
struct VegetationBendCapsule { glm::vec4 startRadius{}, endReach{}; };
struct VegetationBendSelection
{
    std::array<glm::vec4, 8> data{};
    unsigned count = 0;
};
VegetationBendCapsule MakeVegetationBendCapsule(const glm::mat4& world,
    const glm::vec3& center, float radius, float height, float reach = .65f);
std::vector<VegetationBendCapsule> CaptureVegetationBendCapsules(const Engine::Scene::Scene& scene);
VegetationBendSelection SelectVegetationBendCapsules(
    const std::vector<VegetationBendCapsule>& capsules, const glm::vec3& camera);
}
