#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Particles/ParticleSolver.h"
#include "Core/Serialization/SceneSerializer.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <stdexcept>
#include <memory>

using Engine::Components::ParticleEmitter;

namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

bool Near(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(a - b) < 1e-5f;
}

class KillParticleSolver final : public Engine::Components::IParticleSolver
{
public:
    explicit KillParticleSolver(int& resetCount) : m_resetCount(resetCount) {}
    void Reset() override { ++m_resetCount; }
    void Step(Engine::Components::ParticleState* particles, size_t count,
        float, const Engine::Components::ParticleStepForces&) override
    {
        for (size_t index = 0; index < count; ++index)
            particles[index].active = false;
    }
private:
    int& m_resetCount;
};
}

int main()
{
    ParticleEmitter emitter;
    emitter.rate = 0.f;
    emitter.burstOnStart = 3;
    emitter.maxParticles = 4;
    emitter.lifetime = 0.05f;
    emitter.speed = 0.f;
    emitter.gravity = {0.f, 0.f, 0.f};
    emitter.shape = static_cast<int>(ParticleEmitter::Shape::Box);
    emitter.boxExtents = {1.f, 2.f, 3.f};
    emitter.seed = 42;
    emitter.texturePath = "Textures/particle-test.png";
    emitter.opacity = 0.35f;
    emitter.blendMode = static_cast<int>(ParticleEmitter::BlendMode::Alpha);
    emitter.duration = 0.5f;
    emitter.loop = true;
    const glm::mat4 translated = glm::translate(glm::mat4(1.f), {10.f, 0.f, 0.f});

    emitter.Start();
    emitter.Advance(1.f / 60.f, translated);
    Require(emitter.GetParticles().size() == 3, "start burst count");
    auto first = emitter.GetParticles();
    for (const auto& particle : first)
    {
        Require(particle.position.x >= 9.f && particle.position.x <= 11.f,
            "world-space box emission");
        Require(particle.id >= 1 && particle.id <= 3, "stable IDs");
    }
    emitter.Burst(100);
    emitter.Advance(1.f / 60.f, translated);
    Require(emitter.GetParticles().size() == 4, "capacity bound");
    Require(emitter.GetParticles().back().id == 4, "next stable ID");
    emitter.Advance(3.f / 60.f, translated);
    Require(emitter.GetParticles().empty(), "expiry");

    emitter.Start();
    emitter.Advance(1.f / 60.f, translated);
    const auto& replay = emitter.GetParticles();
    Require(replay.size() == first.size(), "replay count");
    for (size_t i = 0; i < replay.size(); ++i)
        Require(replay[i].id == first[i].id &&
            Near(replay[i].position, first[i].position), "deterministic reset");

    const auto saved = emitter.Serialize();
    ParticleEmitter restored;
    restored.Deserialize(saved);
    Require(restored.rate == emitter.rate &&
        restored.burstOnStart == emitter.burstOnStart &&
        restored.shape == emitter.shape &&
        Near(restored.boxExtents, emitter.boxExtents) &&
        restored.seed == emitter.seed &&
        restored.texturePath == emitter.texturePath &&
        restored.opacity == emitter.opacity &&
        restored.blendMode == emitter.blendMode &&
        restored.duration == emitter.duration &&
        restored.loop == emitter.loop, "serialized settings");
    Require(restored.GetParticles().empty(), "runtime state not serialized");

    emitter.worldSpace = false;
    emitter.Start();
    emitter.Advance(1.f / 60.f, translated);
    for (const auto& particle : emitter.GetParticles())
        Require(particle.position.x >= -1.f && particle.position.x <= 1.f,
            "local-space box emission");
    const auto bounds = emitter.GetWorldBounds(translated);
    Require(bounds.valid, "particle bounds available");
    for (const auto& particle : emitter.GetParticles())
    {
        const glm::vec3 worldPosition = particle.position + glm::vec3(10.f, 0.f, 0.f);
        Require(glm::all(glm::lessThanEqual(bounds.minimum, worldPosition)) &&
            glm::all(glm::greaterThanEqual(bounds.maximum, worldPosition)),
            "world bounds contain local-space particles");
    }

    std::vector<Engine::Components::ParticleState> solverParticles(1);
    solverParticles[0].force = {0.f, 6.f, 0.f};
    Engine::Components::BallisticParticleSolver ballistic;
    ballistic.Step(solverParticles.data(), solverParticles.size(), 1.f / 60.f,
        { {0.f, -3.f, 0.f}, 0.f });
    Require(std::abs(solverParticles[0].velocity.y - 0.05f) < 1e-5f &&
        Near(solverParticles[0].force, {0.f, 0.f, 0.f}) &&
        solverParticles[0].age > 0.f, "solver force and age update");

    int solverResets = 0;
    emitter.SetSolver(std::make_unique<KillParticleSolver>(solverResets));
    emitter.Start();
    emitter.Advance(1.f / 60.f, translated);
    Require(emitter.GetParticles().size() == 3, "custom solver spawn");
    emitter.Advance(1.f / 60.f, translated);
    Require(emitter.GetParticles().empty() && solverResets >= 2,
        "custom solver expiry and reset");

    ParticleEmitter finite;
    finite.rate = 0.f;
    finite.burstOnStart = 1;
    finite.lifetime = 10.f;
    finite.speed = 0.f;
    finite.gravity = {0.f, 0.f, 0.f};
    finite.worldSpace = false;
    finite.duration = 2.f / 60.f;
    finite.Start();
    finite.Advance(2.f / 60.f, translated);
    Require(finite.GetParticles().size() == 1 && finite.IsPlaying(),
        "finite emission window");
    finite.Advance(1.f / 60.f, translated);
    Require(finite.GetParticles().size() == 1 && !finite.IsPlaying(),
        "finite emission stops while particles remain alive");
    finite.Play();
    finite.Advance(1.f / 60.f, translated);
    Require(finite.GetParticles().size() == 2 &&
        finite.GetParticles().back().id == 2, "play starts another emission cycle");

    finite.loop = true;
    finite.Start();
    finite.Advance(3.f / 60.f, translated);
    Require(finite.GetParticles().size() == 2, "loop repeats start burst");
    finite.Pause();
    finite.Advance(2.f / 60.f, translated);
    Require(finite.GetParticles().size() == 2, "pause holds emission clock");
    finite.Play();
    finite.Advance(2.f / 60.f, translated);
    Require(finite.GetParticles().size() == 3, "loop resumes from paused clock");
    finite.Reset();
    Require(!finite.GetWorldBounds(translated).valid, "empty emitter has no bounds");
}
