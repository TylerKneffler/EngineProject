#include "ParticleSolver.h"
#include <algorithm>
#include <cmath>

namespace Engine::Components
{
void BallisticParticleSolver::Step(ParticleState* particles, size_t count,
    float seconds, const ParticleStepForces& forces)
{
    const float damp = std::exp(-std::max(0.f, forces.drag) * seconds);
    for (size_t index = 0; index < count; ++index)
    {
        auto& particle = particles[index];
        if (!particle.active) continue;
        particle.velocity = (particle.velocity +
            (forces.acceleration + particle.force) * seconds) * damp;
        particle.position += particle.velocity * seconds;
        particle.age += seconds;
        particle.force = glm::vec3(0.f);
    }
}
}
