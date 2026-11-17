#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace Engine::Components
{
// State shared by emitters, solvers, and renderers. Solver-specific arrays can
// be kept separately and keyed by id, without adding fluid fields here.
struct ParticleState
{
    glm::vec3 position{0.f};
    glm::vec3 velocity{0.f};
    glm::vec3 color{1.f};
    glm::vec3 force{0.f}; // Additional acceleration consumed on the next step.
    float age = 0.f;
    float lifetime = 1.f;
    float size = 0.1f;
    float rotation = 0.f;
    uint64_t id = 0;
    bool active = true;
};
}
