#pragma once
#include <glm/glm.hpp>
#include <array>
#include <vector>
namespace Engine::Scene { class Scene; }
namespace Engine::Rendering
{
struct GrassInteractionCapsule { glm::vec4 startRadius{}, endReach{}; };
struct GrassInteractionSelection
{
    std::array<glm::vec4, 8> data{};
    unsigned count = 0;
};
GrassInteractionCapsule MakeGrassInteractionCapsule(const glm::mat4& world,
    const glm::vec3& center, float radius, float height, float reach = .65f);
std::vector<GrassInteractionCapsule> CaptureGrassInteractionCapsules(const Engine::Scene::Scene& scene);
GrassInteractionSelection SelectGrassInteractionCapsules(
    const std::vector<GrassInteractionCapsule>& capsules, const glm::vec3& camera);
}
