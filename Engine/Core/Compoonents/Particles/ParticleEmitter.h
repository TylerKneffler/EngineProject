#pragma once

#include "Core/Component.h"
#include "ParticleSolver.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Graphics { class IGraphicsProvider; }

namespace Engine::Components
{
class Texture;
// Reusable source of simulated particles. Rendering and fluid solvers can
// consume the same state without owning emission or particle lifetimes.
class ParticleEmitter final : public Engine::Core::Component
{
public:
    enum class Shape : int { Point = 0, Box = 1, Sphere = 2 };
    enum class BlendMode : int { Additive = 0, Alpha = 1 };

    using Particle = ParticleState;

    struct Bounds
    {
        glm::vec3 minimum{0.f};
        glm::vec3 maximum{0.f};
        bool valid = false;
    };

    ParticleEmitter();

    float rate = 10.f;
    int burstOnStart = 0;
    int maxParticles = 1024;
    float lifetime = 2.f;
    float speed = 1.f;
    float spreadDegrees = 15.f;
    glm::vec3 direction{0.f, 1.f, 0.f};
    glm::vec3 gravity{0.f, -9.81f, 0.f};
    float drag = 0.f;
    glm::vec3 color{1.f};
    float size = 0.1f;
    int shape = static_cast<int>(Shape::Point);
    glm::vec3 boxExtents{0.5f};
    float sphereRadius = 0.5f;
    bool worldSpace = true;
    bool playOnStart = true;
    float duration = 0.f; // Zero emits indefinitely; positive values use 60 Hz ticks.
    bool loop = false;
    int seed = 1;
    std::string texturePath;
    float opacity = 1.f;
    int blendMode = static_cast<int>(BlendMode::Additive);

    void Start() override;
    void Update() override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

    void Reset();
    void Play();
    void Pause() { m_playing = false; }
    void Burst(int count);
    // Exposed for deterministic tools and solvers; Update passes the scene's
    // simulation delta and the emitter world transform.
    void Advance(float seconds, const glm::mat4& emitterWorld);
    bool IsPlaying() const { return m_playing; }
    const std::vector<Particle>& GetParticles() const { return m_particles; }
    Bounds GetWorldBounds(const glm::mat4& emitterWorld) const;
    // Replacing a solver resets the emitter and its particle state.
    void SetSolver(std::unique_ptr<IParticleSolver> solver);
    std::shared_ptr<Texture> PrepareTexture(Engine::Graphics::IGraphicsProvider* provider);

private:
    uint32_t NextRandom();
    float RandomUnit();
    glm::vec3 RandomInUnitSphere();
    void SpawnOne(const glm::mat4& emitterWorld);
    void Step(float seconds, const glm::mat4& emitterWorld);

    std::vector<Particle> m_particles;
    uint32_t m_randomState = 1;
    uint64_t m_nextId = 1;
    double m_emitRemainder = 0.0;
    double m_stepRemainder = 0.0;
    uint64_t m_emissionTicks = 0;
    int m_pendingBurst = 0;
    bool m_playing = false;
    std::unique_ptr<IParticleSolver> m_solver;
    std::shared_ptr<Texture> m_texture;
    std::string m_loadedTexturePath;
};
}
