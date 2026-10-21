#pragma once

#include "ParticleSolver.h"
#include "ParticleNeighborGrid.h"
#include <vector>

namespace Engine::Components
{
// Small, CPU reference WCSPH solver. Positions and bounds use the emitter's
// simulation space; world-space emitters are best for a stationary container.
class FluidParticleSolver final : public IParticleSolver
{
public:
    struct Settings
    {
        float smoothingRadius = 0.3f;
        float particleMass = 0.02f;
        float restDensity = 8.f;
        float pressureStiffness = 8.f;
        float viscosity = 0.05f;
        int substeps = 4;
        glm::vec3 boundsMin{-1.f};
        glm::vec3 boundsMax{1.f};
        float boundaryBounce = 0.1f;
        float maxSpeed = 8.f;
    };

    FluidParticleSolver() = default;
    explicit FluidParticleSolver(const Settings& settings) : m_settings(settings) {}
    void SetSettings(const Settings& settings) { m_settings = settings; }
    const Settings& GetSettings() const { return m_settings; }
    void Reset() override;
    void Step(ParticleState* particles, size_t count, float seconds,
        const ParticleStepForces& forces) override;
    const std::vector<float>& GetDensities() const { return m_density; }

private:
    Settings m_settings;
    ParticleNeighborGrid m_grid;
    std::vector<float> m_density;
    std::vector<float> m_pressure;
    std::vector<glm::vec3> m_acceleration;
    std::vector<uint32_t> m_neighbors;
};
}
