#include "Core/Compoonents/Particles/ParticleNeighborGrid.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

using Engine::Components::ParticleNeighborGrid;
using Engine::Components::ParticleState;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<uint32_t> BruteForce(const std::vector<ParticleState>& particles,
    const glm::vec3& center, float radius, uint32_t exclude)
{
    std::vector<uint32_t> result;
    const double squared = static_cast<double>(radius) * radius;
    for (uint32_t index = 0; index < particles.size(); ++index)
    {
        const auto& particle = particles[index];
        if (!particle.active || index == exclude ||
            !std::isfinite(particle.position.x) ||
            !std::isfinite(particle.position.y) ||
            !std::isfinite(particle.position.z)) continue;
        const glm::dvec3 difference = glm::dvec3(particle.position) -
            glm::dvec3(center);
        if (glm::dot(difference, difference) <= squared)
            result.push_back(index);
    }
    return result;
}
}

int main()
{
    std::vector<ParticleState> particles;
    for (int z = -3; z <= 3; ++z)
        for (int y = -3; y <= 3; ++y)
            for (int x = -3; x <= 3; ++x)
            {
                ParticleState particle;
                particle.position = glm::vec3(x * .29f + .01f,
                    y * .31f - .02f, z * .27f + .03f);
                particle.active = particles.size() % 11 != 0;
                particles.push_back(particle);
            }
    particles.push_back(ParticleState{});
    particles.push_back(ParticleState{});
    particles.push_back(ParticleState{});
    particles.back().position.x = std::numeric_limits<float>::infinity();

    ParticleNeighborGrid grid;
    Require(grid.Build(particles.data(), particles.size(), .5f),
        "build neighbor grid");
    Require(grid.GetCellCount() > 1 &&
        grid.GetParticleCount() < particles.size(),
        "grid indexes active finite particles");

    std::vector<uint32_t> found;
    const glm::vec3 centers[] = {
        {-1.f, -.5f, .25f}, {0.f, 0.f, 0.f},
        {.5f, -.5f, .5f}, {1.2f, 1.2f, 1.2f}
    };
    const float radii[] = {0.f, .1f, .49f, .5f, .8f, 2.f, 10.f};
    for (const auto& center : centers)
        for (float radius : radii)
        {
            grid.QueryRadius(center, radius, found);
            Require(found == BruteForce(particles, center, radius,
                std::numeric_limits<uint32_t>::max()),
                "grid matches brute-force neighbors across cells");
            grid.QueryRadius(center, radius, found, 171u);
            Require(found == BruteForce(particles, center, radius, 171u),
                "excluded source index and stable order");
        }

    grid.QueryRadius({0.f, 0.f, 0.f}, 0.f, found);
    Require(found.size() == 2 && found[0] + 1u == found[1],
        "coincident particles at zero radius");
    particles[found[0]].position = {100.f, 100.f, 100.f};
    grid.QueryRadius({0.f, 0.f, 0.f}, 0.f, found);
    Require(found.size() == 2, "build snapshots positions");
    Require(grid.Build(particles.data(), particles.size(), .5f),
        "rebuild after movement");
    grid.QueryRadius({0.f, 0.f, 0.f}, 0.f, found);
    Require(found.size() == 1, "rebuild updates neighbor set");

    Require(!grid.Build(particles.data(), particles.size(), 0.f) &&
        grid.GetParticleCount() == 0, "invalid cell size clears grid");
    Require(grid.Build(nullptr, 0, .5f), "empty grid is valid");
    grid.QueryRadius({0.f, 0.f, 0.f}, -1.f, found);
    Require(found.empty(), "invalid query radius returns no neighbors");
}
