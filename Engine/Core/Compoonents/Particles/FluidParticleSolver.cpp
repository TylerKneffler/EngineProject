#include "FluidParticleSolver.h"
#include <algorithm>
#include <cmath>

namespace Engine::Components
{
namespace
{
bool Finite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

glm::vec3 Limited(const glm::vec3& value, float maximum)
{
    if (!Finite(value)) return glm::vec3(0.f);
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared > maximum * maximum ?
        value * (maximum / std::sqrt(lengthSquared)) : value;
}

float ClampedFinite(float value, float minimum, float maximum,
    float fallback)
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}
}

void FluidParticleSolver::Reset()
{
    m_grid.Clear();
    m_density.clear();
    m_pressure.clear();
    m_acceleration.clear();
    m_neighbors.clear();
}

void FluidParticleSolver::Step(ParticleState* particles, size_t count,
    float seconds, const ParticleStepForces& forces)
{
    if (!particles || !count || !std::isfinite(seconds) || seconds <= 0.f)
        return;
    const float h = ClampedFinite(m_settings.smoothingRadius, 0.02f, 10.f, 0.3f);
    const float mass = ClampedFinite(m_settings.particleMass, 0.00001f, 100.f, 0.02f);
    const float rest = ClampedFinite(m_settings.restDensity, 0.001f, 100000.f, 8.f);
    const float stiffness = ClampedFinite(m_settings.pressureStiffness, 0.f, 10000.f, 8.f);
    const float viscosity = ClampedFinite(m_settings.viscosity, 0.f, 100.f, 0.05f);
    const float maxSpeed = ClampedFinite(m_settings.maxSpeed, 0.01f, 1000.f, 8.f);
    const float bounce = ClampedFinite(m_settings.boundaryBounce, 0.f, 1.f, 0.1f);
    const int substeps = std::clamp(m_settings.substeps, 1, 16);
    const float dt = seconds / static_cast<float>(substeps);
    const float drag = ClampedFinite(forces.drag, 0.f, 100.f, 0.f);
    const float damping = std::exp(-drag * dt);
    const glm::vec3 configuredMin = Finite(m_settings.boundsMin) ?
        m_settings.boundsMin : glm::vec3(-1.f);
    const glm::vec3 configuredMax = Finite(m_settings.boundsMax) ?
        m_settings.boundsMax : glm::vec3(1.f);
    const glm::vec3 boundsMin = glm::min(configuredMin, configuredMax);
    const glm::vec3 boundsMax = glm::max(configuredMin, configuredMax);
    const float h2 = h * h;
    constexpr float pi = 3.14159265358979323846f;
    const float poly6 = 315.f / (64.f * pi * std::pow(h, 9.f));
    const float spikyGradient = -45.f / (pi * std::pow(h, 6.f));
    const float viscosityLaplacian = 45.f / (pi * std::pow(h, 6.f));
    m_density.resize(count);
    m_pressure.resize(count);
    m_acceleration.resize(count);

    for (size_t i = 0; i < count; ++i)
    {
        if (!particles[i].active) continue;
        if (!Finite(particles[i].position) || !Finite(particles[i].velocity))
            particles[i].active = false;
    }

    for (int step = 0; step < substeps; ++step)
    {
        if (!m_grid.Build(particles, count, h)) return;
        // Include the self kernel. QueryRadius also includes coincident peers.
        for (size_t i = 0; i < count; ++i)
        {
            m_density[i] = 0.f;
            m_pressure[i] = 0.f;
            if (!particles[i].active) continue;
            m_grid.QueryRadius(particles[i].position, h, m_neighbors);
            for (const uint32_t j : m_neighbors)
            {
                const glm::vec3 delta = particles[i].position -
                    particles[j].position;
                const float kernel = std::max(0.f, h2 - glm::dot(delta, delta));
                m_density[i] += mass * poly6 * kernel * kernel * kernel;
            }
            m_density[i] = std::max(m_density[i], 0.001f);
            m_pressure[i] = stiffness * std::max(0.f, m_density[i] - rest);
        }

        for (size_t i = 0; i < count; ++i)
        {
            m_acceleration[i] = glm::vec3(0.f);
            if (!particles[i].active) continue;
            glm::vec3 acceleration = (Finite(forces.acceleration) ?
                forces.acceleration : glm::vec3(0.f)) + particles[i].force;
            m_grid.QueryRadius(particles[i].position, h, m_neighbors,
                static_cast<uint32_t>(i));
            for (const uint32_t j : m_neighbors)
            {
                const glm::vec3 displacement = particles[i].position -
                    particles[j].position;
                const float distance = glm::length(displacement);
                if (distance <= 1e-6f || distance >= h) continue;
                const float remainder = h - distance;
                const glm::vec3 gradient = spikyGradient * remainder *
                    remainder * displacement / distance;
                acceleration -= mass * (m_pressure[i] /
                    (m_density[i] * m_density[i]) + m_pressure[j] /
                    (m_density[j] * m_density[j])) * gradient;
                acceleration += viscosity * mass *
                    (particles[j].velocity - particles[i].velocity) /
                    m_density[j] * viscosityLaplacian * remainder;
            }
            m_acceleration[i] = Limited(acceleration, 1000.f);
        }

        for (size_t i = 0; i < count; ++i)
        {
            auto& particle = particles[i];
            if (!particle.active) continue;
            particle.velocity = Limited((particle.velocity +
                m_acceleration[i] * dt) * damping, maxSpeed);
            particle.position += particle.velocity * dt;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (particle.position[axis] < boundsMin[axis])
                {
                    particle.position[axis] = boundsMin[axis];
                    if (particle.velocity[axis] < 0.f)
                        particle.velocity[axis] *= -bounce;
                }
                else if (particle.position[axis] > boundsMax[axis])
                {
                    particle.position[axis] = boundsMax[axis];
                    if (particle.velocity[axis] > 0.f)
                        particle.velocity[axis] *= -bounce;
                }
            }
        }
    }
    for (size_t i = 0; i < count; ++i)
    {
        if (!particles[i].active) continue;
        particles[i].age += seconds;
        particles[i].force = glm::vec3(0.f);
    }
}
}
