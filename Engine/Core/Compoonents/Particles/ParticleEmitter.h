#pragma once

#include "Core/Component.h"
#include "ParticleSolver.h"
#include "FluidParticleSolver.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IPipelineState.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Graphics { class IGraphicsProvider; }
namespace Engine::Graphics { class IGraphicsContext; }
namespace Engine::Core { class Object; }

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
    enum class CollisionMode : int { None = 0, Kill = 1, Bounce = 2 };
    enum class FluidCouplingMode : int
    { Off = 0, VisualOnly = 1, GameplayImpulse = 2 };

    struct CollisionHit
    {
        uint64_t particleId = 0;
        Engine::Core::Object* object = nullptr; // Non-owning; valid while the scene object lives.
        glm::vec3 point{0.f};
        glm::vec3 normal{0.f, 1.f, 0.f};
        glm::vec3 incomingVelocity{0.f}; // World space.
        glm::vec3 outgoingVelocity{0.f}; // World space; zero when killed.
        uint64_t simulationTick = 0;
        bool killed = false;
    };

    using Particle = ParticleState;

    struct Bounds
    {
        glm::vec3 minimum{0.f};
        glm::vec3 maximum{0.f};
        bool valid = false;
    };

    struct GpuSnapshot
    {
        uint32_t liveCount = 0;
        Bounds bounds;
        float minimumAge = 0.f;
        float maximumAge = 0.f;
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
    int collisionMode = static_cast<int>(CollisionMode::None);
    float collisionBounce = 0.5f;
    float collisionFriction = 0.2f;
    int collisionMask = -1;
    bool useGpuSimulation = false;
    bool fluidEnabled = false;
    FluidParticleSolver::Settings fluid;
    bool fluidVisual = true;
    float fluidVisualRadiusScale = 1.35f;
    int fluidCouplingMode = static_cast<int>(FluidCouplingMode::Off);
    float fluidRigidBounce = 0.2f;
    float fluidRigidFriction = 0.1f;
    float fluidImpulseScale = 1.f;
    int fluidCollisionMask = -1;

    void Start() override;
    void Update() override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

    void Reset();
    void Restart();
    // Play resumes a paused emitter without clearing live particles. After a
    // finite emission window, it begins a new emission cycle. Start() is the
    // scene-load reset and honors playOnStart; Restart() always clears/plays.
    void Play();
    void Pause() { m_playing = false; }
    void Burst(int count);
    // Adds acceleration to one live particle for consumption on its next step.
    bool AddParticleForce(uint64_t id, const glm::vec3& acceleration);
    // Retains the oldest live particles when a capacity or scene budget shrinks.
    // Removed IDs are reported to the solver before dense storage is recycled.
    void TrimToCapacity(size_t capacity);
    // Exposed for deterministic tools and solvers; Update passes the scene's
    // simulation delta and the emitter world transform.
    void Advance(float seconds, const glm::mat4& emitterWorld);
    // Rebuild state from scene start for editor scrubbing and deterministic
    // replay. Moving world-space emitters should supply a transform at each
    // tick; the fixed-transform overload is useful for static previews.
    // Invalid targets and targets beyond the replay budget leave state intact.
    bool Seek(double seconds, const glm::mat4& emitterWorld,
        uint64_t maxReplaySteps = 36000);
    bool Seek(double seconds,
        const std::function<glm::mat4(double)>& worldAtTime,
        uint64_t maxReplaySteps = 36000);
    bool IsPlaying() const { return m_playing; }
    bool IsEditorPreviewRunning() const
    {
        return m_editorPreviewRunning && (m_playing || !m_particles.empty() ||
            (IsGpuSimulationActive() && m_simulationTicks < m_gpuLatestExpiryTick));
    }
    uint64_t GetSimulationTicks() const { return m_simulationTicks; }
    const std::vector<Particle>& GetParticles() const { return m_particles; }
    // Hits from the most recent Advance call, in fixed-step and particle order.
    // Copy them during the same scene update; the next Advance clears the batch.
    const std::vector<CollisionHit>& GetCollisionHits() const { return m_collisionHits; }
    size_t GetDroppedCollisionHitCount() const { return m_droppedCollisionHits; }
    bool IsGpuSimulationActive() const;
    uint32_t GetGpuDrawCapacity() const;
    uint32_t GetGpuReservedCapacity() const;
    const Engine::Graphics::IGraphicsBuffer* GetGpuDrawBuffer() const;
    void RecordGpuSimulation(Engine::Graphics::IGraphicsContext* context);
    // Explicit inspection path. Request after recording simulation, submit the
    // command buffer, wait for its fence, then resolve before requesting again.
    bool RequestGpuSnapshot(Engine::Graphics::IGraphicsContext* context);
    bool ResolveGpuSnapshot(GpuSnapshot& snapshot);
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
    void ResolveCollisions(const glm::mat4& emitterWorld, float seconds);
    bool SyncConfiguredSolver();
    bool InitializeGpuSimulation();
    void ScheduleGpuStep(float seconds, const glm::mat4& emitterWorld);

    struct PendingGpuStep
    {
        glm::mat4 emitterWorld{1.f};
        uint32_t spawnCount = 0;
        uint32_t firstId = 0;
        float seconds = 0.f;
    };
    struct GpuRuntime
    {
        std::unique_ptr<Engine::Graphics::IGraphicsBuffer> buffers[2];
        std::unique_ptr<Engine::Graphics::IGraphicsBuffer> readback;
        std::unique_ptr<Engine::Graphics::IPipelineState> clearPipeline;
        std::unique_ptr<Engine::Graphics::IPipelineState> updatePipeline;
        std::unique_ptr<Engine::Graphics::IPipelineState> spawnPipeline;
        std::vector<PendingGpuStep> pending;
        uint32_t capacity = 0;
        uint32_t active = 0;
        bool initialized = false;
        bool snapshotPending = false;
    };

    std::vector<Particle> m_particles;
    std::vector<glm::vec3> m_previousPositions;
    std::vector<CollisionHit> m_collisionHits;
    std::unique_ptr<GpuRuntime> m_gpu;
    std::vector<std::unique_ptr<GpuRuntime>> m_retiredGpu;
    bool m_gpuInitializationAttempted = false;
    bool m_gpuPreviewCpu = false;
    uint64_t m_gpuLatestExpiryTick = 0;
    size_t m_droppedCollisionHits = 0;
    uint32_t m_randomState = 1;
    uint64_t m_nextId = 1;
    double m_emitRemainder = 0.0;
    double m_stepRemainder = 0.0;
    uint64_t m_emissionTicks = 0;
    uint64_t m_simulationTicks = 0;
    int m_pendingBurst = 0;
    bool m_playing = false;
    bool m_editorPreviewRunning = false;
    bool m_emissionStarted = false;
    std::unique_ptr<IParticleSolver> m_solver;
    bool m_customSolver = false;
    std::shared_ptr<Texture> m_texture;
    std::string m_loadedTexturePath;
};
}
