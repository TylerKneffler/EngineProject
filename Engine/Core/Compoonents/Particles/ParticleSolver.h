#pragma once

#include "ParticleState.h"
#include <cstddef>

namespace Engine::Components
{
struct ParticleStepForces
{
    glm::vec3 acceleration{0.f};
    float drag = 0.f;
};

// An emitter owns the fixed clock, spawn order, capacity, and lifetime cull.
// Solvers update live particles once per fixed step and own any extra state.
// They advance age, consume accumulated force, and may mark particles inactive.
// The pointer/count view prevents a solver from changing spawn order or capacity.
class IParticleSolver
{
public:
    virtual ~IParticleSolver() = default;
    virtual void Reset() {}
    virtual void Step(ParticleState* particles, size_t count, float seconds,
        const ParticleStepForces& forces) = 0;
};

class BallisticParticleSolver final : public IParticleSolver
{
public:
    void Step(ParticleState* particles, size_t count, float seconds,
        const ParticleStepForces& forces) override;
};
}
