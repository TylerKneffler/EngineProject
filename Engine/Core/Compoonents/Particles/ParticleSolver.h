#pragma once

#include "ParticleState.h"
#include <cstddef>
#include <cstdint>

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
    // Called before a particle leaves the dense live array, including expiry,
    // solver death, and budget truncation. Release solver-owned data keyed by id.
    virtual void OnParticleRemoved(uint64_t id) { (void)id; }
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
