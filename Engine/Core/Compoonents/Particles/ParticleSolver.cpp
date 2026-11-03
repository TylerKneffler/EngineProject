#include "ParticleSolver.h"
#include "Core/Threading/JobScheduler.h"
#include <algorithm>
#include <cmath>

namespace Engine::Components
{
void BallisticParticleSolver::Step(ParticleState* particles, size_t count,
    float seconds, const ParticleStepForces& forces)
{
    const float damp = std::exp(-std::max(0.f, forces.drag) * seconds);
    Engine::Threading::JobScheduler::Get().For(count, 1024, [&](size_t index)
    {
        auto& particle = particles[index];
        if (!particle.active) return;
        particle.velocity = (particle.velocity +
            (forces.acceleration + particle.force) * seconds) * damp;
        particle.position += particle.velocity * seconds;
        particle.age += seconds;
        particle.force = glm::vec3(0.f);
    });
}
}
