#include "ParticleEmitter.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Physics/Physics.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif
#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

namespace Engine::Components
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr int ParticleLimit = 1000000;
constexpr double FixedStep = 1.0 / 60.0;
constexpr size_t MaxCollisionHitsPerAdvance = 4096;

struct alignas(16) GpuParticleConstants
{
    uint32_t u0[4]{};
    glm::vec4 c[7]{};
};
static_assert(sizeof(GpuParticleConstants) == 128);
constexpr uint64_t GpuParticleStride = 64;

std::string ParticleGpuShaderPath()
{
    const auto bundled = std::filesystem::path("Engine/Shaders/ParticleGpu/ParticleGpu.hlsl");
    if (std::filesystem::is_regular_file(bundled)) return bundled.string();
    return (std::filesystem::path(ENGINE_SHADERS_PATH) /
        "ParticleGpu/ParticleGpu.hlsl").string();
}

uint64_t EmissionDurationTicks(float duration)
{
    if (!std::isfinite(duration) || duration <= 0.f) return 0;
    return static_cast<uint64_t>(std::max(1.0,
        std::ceil(std::min(static_cast<double>(duration), 1e12) /
            FixedStep - 1e-6)));
}

glm::vec3 SafeDirection(glm::vec3 value)
{
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared > 1e-12f && std::isfinite(lengthSquared)
        ? value / std::sqrt(lengthSquared) : glm::vec3(0.f, 1.f, 0.f);
}

std::string ResolveParticleTexturePath(const std::string& requested)
{
    const std::filesystem::path path(requested);
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error))
        return path.lexically_normal().generic_string();
    const auto relative = path.lexically_relative(std::filesystem::path("Assets"));
    if (!relative.empty() && *relative.begin() != "..")
    {
        const auto bundled = std::filesystem::path(ENGINE_ASSETS_PATH) / relative;
        if (std::filesystem::is_regular_file(bundled, error))
            return bundled.lexically_normal().generic_string();
    }
    const auto bundled = std::filesystem::path(ENGINE_ASSETS_PATH) / path;
    if (std::filesystem::is_regular_file(bundled, error))
        return bundled.lexically_normal().generic_string();
    return path.lexically_normal().generic_string();
}
}

ParticleEmitter::ParticleEmitter()
    : m_solver(std::make_unique<BallisticParticleSolver>())
{
    SetTypeName(COMPONENT_TYPE_NAME(ParticleEmitter));
    singlecomponent = true;
    RegisterField("rate", rate);
    RegisterField("burstOnStart", burstOnStart);
    RegisterField("maxParticles", maxParticles);
    RegisterField("lifetime", lifetime);
    RegisterField("speed", speed);
    RegisterField("spreadDegrees", spreadDegrees);
    RegisterField("direction", direction);
    RegisterField("gravity", gravity);
    RegisterField("drag", drag);
    RegisterField("color", color);
    RegisterField("size", size);
    RegisterField("shape", shape);
    RegisterField("boxExtents", boxExtents);
    RegisterField("sphereRadius", sphereRadius);
    RegisterField("worldSpace", worldSpace);
    RegisterField("playOnStart", playOnStart);
    RegisterField("duration", duration);
    RegisterField("loop", loop);
    RegisterField("seed", seed);
    RegisterField("texturePath", texturePath);
    RegisterField("opacity", opacity);
    RegisterField("blendMode", blendMode);
    RegisterField("collisionMode", collisionMode);
    RegisterField("collisionBounce", collisionBounce);
    RegisterField("collisionFriction", collisionFriction);
    RegisterField("collisionMask", collisionMask);
    RegisterField("useGpuSimulation", useGpuSimulation);
    RegisterField("fluidEnabled", fluidEnabled);
    RegisterField("fluidSmoothingRadius", fluid.smoothingRadius);
    RegisterField("fluidParticleMass", fluid.particleMass);
    RegisterField("fluidRestDensity", fluid.restDensity);
    RegisterField("fluidPressureStiffness", fluid.pressureStiffness);
    RegisterField("fluidViscosity", fluid.viscosity);
    RegisterField("fluidSubsteps", fluid.substeps);
    RegisterField("fluidBoundsMin", fluid.boundsMin);
    RegisterField("fluidBoundsMax", fluid.boundsMax);
    RegisterField("fluidBoundaryBounce", fluid.boundaryBounce);
    RegisterField("fluidMaxSpeed", fluid.maxSpeed);
    RegisterField("fluidVisual", fluidVisual);
    RegisterField("fluidVisualRadiusScale", fluidVisualRadiusScale);
    RegisterField("fluidCouplingMode", fluidCouplingMode);
    RegisterField("fluidRigidBounce", fluidRigidBounce);
    RegisterField("fluidRigidFriction", fluidRigidFriction);
    RegisterField("fluidImpulseScale", fluidImpulseScale);
    RegisterField("fluidCollisionMask", fluidCollisionMask);
}

void ParticleEmitter::Reset()
{
    SyncConfiguredSolver();
    m_solver->Reset();
    m_particles.clear();
    m_previousPositions.clear();
    m_collisionHits.clear();
    m_droppedCollisionHits = 0;
    if (m_gpu)
    {
        m_gpu->pending.clear();
        m_gpu->initialized = false;
        m_gpu->active = 0;
        m_gpu->snapshotPending = false;
    }
    m_gpuInitializationAttempted = false;
    m_gpuPreviewCpu = false;
    m_gpuLatestExpiryTick = 0;
    m_randomState = seed > 0 ? static_cast<uint32_t>(seed) : 1u;
    m_nextId = 1;
    m_emitRemainder = 0.0;
    m_stepRemainder = 0.0;
    m_emissionTicks = 0;
    m_simulationTicks = 0;
    m_pendingBurst = 0;
    m_playing = false;
    m_editorPreviewRunning = false;
    m_emissionStarted = false;
}

void ParticleEmitter::SetSolver(std::unique_ptr<IParticleSolver> solver)
{
    m_customSolver = static_cast<bool>(solver);
    m_solver = solver ? std::move(solver) :
        std::make_unique<BallisticParticleSolver>();
    Reset();
}

bool ParticleEmitter::SyncConfiguredSolver()
{
    if (m_customSolver) return false;
    if (fluidEnabled)
    {
        if (auto* solver = dynamic_cast<FluidParticleSolver*>(m_solver.get()))
        {
            solver->SetSettings(fluid);
            return false;
        }
        m_solver = std::make_unique<FluidParticleSolver>(fluid);
        return true;
    }
    if (dynamic_cast<BallisticParticleSolver*>(m_solver.get())) return false;
    m_solver = std::make_unique<BallisticParticleSolver>();
    return true;
}

bool ParticleEmitter::InitializeGpuSimulation()
{
    if (!useGpuSimulation || m_gpuPreviewCpu || !worldSpace ||
        collisionMode != static_cast<int>(CollisionMode::None) ||
        !dynamic_cast<BallisticParticleSolver*>(m_solver.get()) ||
        !Owner || !Owner->GetScene()) return false;
    const uint32_t capacity = static_cast<uint32_t>(std::min<size_t>(
        std::clamp(maxParticles, 0, 100000),
        Owner->GetScene()->GetParticleCapacityFor(this)));
    if (!capacity) return false;
    if (m_gpu && m_gpu->capacity == capacity) return true;
    if (m_gpuInitializationAttempted) return false;
    m_gpuInitializationAttempted = true;
    auto* provider = Owner->GetScene()->GetGraphicsProvider();
    if (!provider || !provider->GetShaderCompiler() ||
        !provider->GetPipelineStateFactory() || !provider->GetBufferFactory())
        return false;
    auto* compiler = provider->GetShaderCompiler();
    auto* pipelines = provider->GetPipelineStateFactory();
    auto* buffers = provider->GetBufferFactory();
    const std::string path = ParticleGpuShaderPath();
    const auto compile = [&](const char* entry)
    {
        auto shader = compiler->CompileFromFile(path, entry,
            Engine::Graphics::IShaderCompiler::CompileProfile::CS_5_0);
        return shader ? pipelines->CreateComputePipeline(shader.get()) : nullptr;
    };
    auto runtime = std::make_unique<GpuRuntime>();
    runtime->clearPipeline = compile("CSClear");
    runtime->updatePipeline = compile("CSUpdate");
    runtime->spawnPipeline = compile("CSSpawn");
    if (!runtime->clearPipeline || !runtime->updatePipeline ||
        !runtime->spawnPipeline) return false;
    runtime->capacity = capacity;
    try
    {
        for (auto& buffer : runtime->buffers)
            buffer = buffers->CreateBuffer(
                Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer,
                Engine::Graphics::IGraphicsBuffer::AccessMode::Default,
                (static_cast<uint64_t>(capacity) + 1u) * GpuParticleStride,
                nullptr, static_cast<uint32_t>(GpuParticleStride));
    }
    catch (const std::exception&)
    {
        return false;
    }
    if (!runtime->buffers[0] || !runtime->buffers[1]) return false;
    if (m_gpu) m_retiredGpu.push_back(std::move(m_gpu));
    m_gpu = std::move(runtime);
    return true;
}

bool ParticleEmitter::IsGpuSimulationActive() const
{
    return useGpuSimulation && !m_gpuPreviewCpu && worldSpace &&
        collisionMode == static_cast<int>(CollisionMode::None) &&
        dynamic_cast<BallisticParticleSolver*>(m_solver.get()) &&
        m_gpu && m_gpu->initialized;
}

uint32_t ParticleEmitter::GetGpuDrawCapacity() const
{
    return IsGpuSimulationActive() ? m_gpu->capacity : 0;
}

uint32_t ParticleEmitter::GetGpuReservedCapacity() const
{
    return useGpuSimulation && !m_gpuPreviewCpu && m_gpu ? m_gpu->capacity : 0;
}

const Engine::Graphics::IGraphicsBuffer* ParticleEmitter::GetGpuDrawBuffer() const
{
    return IsGpuSimulationActive() ?
        m_gpu->buffers[m_gpu->active].get() : nullptr;
}

void ParticleEmitter::ScheduleGpuStep(float seconds,
    const glm::mat4& emitterWorld)
{
    const uint64_t durationTicks = EmissionDurationTicks(duration);
    if (m_playing && durationTicks && m_emissionTicks >= durationTicks)
    {
        if (loop)
        {
            m_emissionTicks = 0;
            Burst(burstOnStart);
        }
        else
            m_playing = false;
    }
    int requested = std::exchange(m_pendingBurst, 0);
    if (m_playing && std::isfinite(rate) && rate > 0.f)
    {
        m_emitRemainder += static_cast<double>(rate) * seconds;
        const int continuous = static_cast<int>(std::min(
            std::floor(m_emitRemainder),
            static_cast<double>(m_gpu->capacity)));
        m_emitRemainder -= std::floor(m_emitRemainder);
        requested = std::min(ParticleLimit, requested + continuous);
    }
    if (m_playing) ++m_emissionTicks;
    requested = std::clamp(requested, 0, static_cast<int>(m_gpu->capacity));
    const uint32_t firstId = static_cast<uint32_t>(m_nextId);
    m_nextId += static_cast<uint64_t>(requested);
    if (requested)
    {
        m_gpuLatestExpiryTick = std::max(m_gpuLatestExpiryTick,
            m_simulationTicks + static_cast<uint64_t>(std::ceil(
                std::max(0.001f, lifetime) / FixedStep)));
    }
    m_gpu->pending.push_back({ emitterWorld,
        static_cast<uint32_t>(requested), firstId, seconds });
    ++m_simulationTicks;
}

void ParticleEmitter::RecordGpuSimulation(
    Engine::Graphics::IGraphicsContext* context)
{
    if (!context || !m_gpu || m_gpu->pending.empty()) return;
    for (const PendingGpuStep& step : m_gpu->pending)
    {
        const uint32_t next = 1u - m_gpu->active;
        auto* output = m_gpu->buffers[next].get();
        if (!context->SetComputePipeline(m_gpu->clearPipeline.get())) return;
        context->SetComputeWriteBuffer(output);
        context->DispatchCompute(1);
        context->StorageBarrier(output);
        if (m_gpu->initialized)
        {
            GpuParticleConstants update{};
            update.u0[0] = m_gpu->capacity;
            update.c[0] = glm::vec4(step.seconds, std::max(0.f, drag), 0.f, 0.f);
            update.c[1] = glm::vec4(gravity, 0.f);
            context->SetComputePipeline(m_gpu->updatePipeline.get());
            context->SetComputeConstants(&update, sizeof(update));
            context->SetComputeReadBuffer(m_gpu->buffers[m_gpu->active].get());
            context->SetComputeWriteBuffer(output);
            context->DispatchCompute((m_gpu->capacity + 63u) / 64u);
            context->StorageBarrier(output);
        }
        if (step.spawnCount)
        {
            GpuParticleConstants spawn{};
            spawn.u0[0] = step.spawnCount;
            spawn.u0[1] = m_gpu->capacity;
            spawn.u0[2] = step.firstId;
            spawn.u0[3] = static_cast<uint32_t>(std::max(1, seed));
            spawn.c[0] = glm::vec4(glm::vec3(step.emitterWorld[0]),
                std::max(0.f, speed));
            spawn.c[1] = glm::vec4(glm::vec3(step.emitterWorld[1]),
                std::max(0.001f, lifetime));
            spawn.c[2] = glm::vec4(glm::vec3(step.emitterWorld[2]),
                spreadDegrees);
            spawn.c[3] = glm::vec4(glm::vec3(step.emitterWorld[3]),
                static_cast<float>(shape));
            spawn.c[4] = glm::vec4(direction, sphereRadius);
            spawn.c[5] = glm::vec4(boxExtents, size);
            spawn.c[6] = glm::vec4(color, 0.f);
            context->SetComputePipeline(m_gpu->spawnPipeline.get());
            context->SetComputeConstants(&spawn, sizeof(spawn));
            context->SetComputeWriteBuffer(output);
            context->DispatchCompute((step.spawnCount + 63u) / 64u);
            context->StorageBarrier(output);
        }
        m_gpu->active = next;
        m_gpu->initialized = true;
    }
    m_gpu->pending.clear();
}

bool ParticleEmitter::RequestGpuSnapshot(
    Engine::Graphics::IGraphicsContext* context)
{
    if (!context || !IsGpuSimulationActive() || !m_gpu->pending.empty() ||
        m_gpu->snapshotPending || !Owner || !Owner->GetScene()) return false;
    if (!m_gpu->readback)
    {
        auto* provider = Owner->GetScene()->GetGraphicsProvider();
        if (!provider || !provider->GetBufferFactory()) return false;
        const uint64_t bytes =
            (static_cast<uint64_t>(m_gpu->capacity) + 1u) * GpuParticleStride;
        m_gpu->readback = provider->GetBufferFactory()->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer,
            Engine::Graphics::IGraphicsBuffer::AccessMode::Readback,
            bytes, nullptr, static_cast<uint32_t>(GpuParticleStride));
        if (!m_gpu->readback) return false;
    }
    if (!context->CopyBuffer(m_gpu->buffers[m_gpu->active].get(),
        m_gpu->readback.get())) return false;
    m_gpu->snapshotPending = true;
    return true;
}

bool ParticleEmitter::ResolveGpuSnapshot(GpuSnapshot& snapshot)
{
    if (!m_gpu || !m_gpu->snapshotPending || !m_gpu->readback)
        return false;
    struct alignas(16) GpuParticleData
    {
        float positionAge[4];
        float velocityLife[4];
        float colorSize[4];
        uint32_t meta[4];
    };
    static_assert(sizeof(GpuParticleData) == GpuParticleStride);
    auto* particles = static_cast<const GpuParticleData*>(m_gpu->readback->Map());
    if (!particles) return false;
    GpuSnapshot result{};
    result.liveCount = std::min(particles[0].meta[0], m_gpu->capacity);
    constexpr float CornerRadius = 1.41421356237f;
    for (uint32_t index = 1; index <= result.liveCount; ++index)
    {
        const auto& particle = particles[index];
        const glm::vec3 position(particle.positionAge[0],
            particle.positionAge[1], particle.positionAge[2]);
        const glm::vec3 radius(std::max(0.f, particle.colorSize[3]) *
            CornerRadius);
        if (!result.bounds.valid)
        {
            result.bounds.minimum = position - radius;
            result.bounds.maximum = position + radius;
            result.minimumAge = particle.positionAge[3];
            result.maximumAge = particle.positionAge[3];
            result.bounds.valid = true;
        }
        else
        {
            result.bounds.minimum = glm::min(result.bounds.minimum,
                position - radius);
            result.bounds.maximum = glm::max(result.bounds.maximum,
                position + radius);
            result.minimumAge = std::min(result.minimumAge,
                particle.positionAge[3]);
            result.maximumAge = std::max(result.maximumAge,
                particle.positionAge[3]);
        }
    }
    m_gpu->readback->Unmap();
    m_gpu->snapshotPending = false;
    snapshot = result;
    return true;
}

void ParticleEmitter::Play()
{
    const uint64_t durationTicks = EmissionDurationTicks(duration);
    if (!m_playing)
    {
        if (durationTicks && m_emissionTicks >= durationTicks)
        {
            m_emissionTicks = 0;
            Burst(burstOnStart);
        }
        else if (!m_emissionStarted)
            Burst(burstOnStart);
        m_emissionStarted = true;
    }
    m_playing = true;
}

void ParticleEmitter::Restart()
{
    Reset();
    Play();
}

ParticleEmitter::Bounds ParticleEmitter::GetWorldBounds(
    const glm::mat4& emitterWorld) const
{
    Bounds bounds;
    constexpr float CornerRadius = 1.41421356237f;
    for (const Particle& particle : m_particles)
    {
        if (!particle.active) continue;
        const glm::vec3 position = worldSpace ? particle.position :
            glm::vec3(emitterWorld * glm::vec4(particle.position, 1.f));
        const float visualScale = fluidEnabled && fluidVisual ?
            std::clamp(fluidVisualRadiusScale, 0.5f, 3.f) : 1.f;
        const glm::vec3 radius(std::max(0.f, particle.size) *
            visualScale * CornerRadius);
        if (!bounds.valid)
        {
            bounds.minimum = position - radius;
            bounds.maximum = position + radius;
            bounds.valid = true;
        }
        else
        {
            bounds.minimum = glm::min(bounds.minimum, position - radius);
            bounds.maximum = glm::max(bounds.maximum, position + radius);
        }
    }
    return bounds;
}

void ParticleEmitter::Start()
{
    Reset();
    if (playOnStart) Play();
}

void ParticleEmitter::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    m_texture.reset();
    m_loadedTexturePath.clear();
    Reset();
}

std::shared_ptr<Texture> ParticleEmitter::PrepareTexture(
    Engine::Graphics::IGraphicsProvider* provider)
{
    if (texturePath != m_loadedTexturePath)
    {
        m_loadedTexturePath = texturePath;
        m_texture = texturePath.empty() ? nullptr :
            Texture::Acquire(ResolveParticleTexturePath(texturePath));
    }
    if (!m_texture || !m_texture->Prepare(provider)) return nullptr;
    return m_texture;
}

void ParticleEmitter::Update()
{
    if (!Owner || !Owner->GetScene()) return;
    Advance(Owner->GetScene()->GetDeltaTime(),
        Owner->transform.GetWorldMatrix());
}

void ParticleEmitter::Burst(int count)
{
    if (count <= 0) return;
    m_pendingBurst = std::min(ParticleLimit,
        m_pendingBurst + std::min(count, ParticleLimit - m_pendingBurst));
}

bool ParticleEmitter::AddParticleForce(uint64_t id,
    const glm::vec3& acceleration)
{
    if (!std::isfinite(acceleration.x) || !std::isfinite(acceleration.y) ||
        !std::isfinite(acceleration.z)) return false;
    for (Particle& particle : m_particles)
        if (particle.active && particle.id == id)
        {
            const glm::vec3 accumulated = particle.force + acceleration;
            if (!std::isfinite(accumulated.x) ||
                !std::isfinite(accumulated.y) ||
                !std::isfinite(accumulated.z)) return false;
            particle.force = accumulated;
            return true;
        }
    return false;
}

void ParticleEmitter::TrimToCapacity(size_t capacity)
{
    if (m_particles.size() <= capacity) return;
    for (size_t index = capacity; index < m_particles.size(); ++index)
        m_solver->OnParticleRemoved(m_particles[index].id);
    m_particles.resize(capacity);
}

uint32_t ParticleEmitter::NextRandom()
{
    uint32_t x = m_randomState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_randomState = x;
    return x;
}

float ParticleEmitter::RandomUnit()
{
    return static_cast<float>(NextRandom() >> 8) * (1.f / 16777216.f);
}

glm::vec3 ParticleEmitter::RandomInUnitSphere()
{
    const float z = 2.f * RandomUnit() - 1.f;
    const float angle = 2.f * Pi * RandomUnit();
    const float radius = std::cbrt(RandomUnit());
    const float xy = std::sqrt(std::max(0.f, 1.f - z * z));
    return radius * glm::vec3(xy * std::cos(angle), z, xy * std::sin(angle));
}

void ParticleEmitter::SpawnOne(const glm::mat4& emitterWorld)
{
    if (m_particles.size() >= static_cast<size_t>(std::clamp(maxParticles, 0, ParticleLimit)))
        return;

    Particle particle;
    particle.id = m_nextId++;
    particle.lifetime = std::max(0.001f, lifetime);
    particle.color = color;
    particle.size = std::max(0.f, size);

    glm::vec3 localPosition(0.f);
    if (shape == static_cast<int>(Shape::Box))
    {
        const glm::vec3 extents = glm::max(glm::abs(boxExtents), glm::vec3(0.f));
        localPosition = extents * glm::vec3(
            2.f * RandomUnit() - 1.f,
            2.f * RandomUnit() - 1.f,
            2.f * RandomUnit() - 1.f);
    }
    else if (shape == static_cast<int>(Shape::Sphere))
        localPosition = RandomInUnitSphere() * std::max(0.f, sphereRadius);

    const glm::vec3 axis = SafeDirection(direction);
    const float cone = std::clamp(spreadDegrees, 0.f, 180.f) * Pi / 180.f;
    const float cosTheta = 1.f - RandomUnit() * (1.f - std::cos(cone));
    const float sinTheta = std::sqrt(std::max(0.f, 1.f - cosTheta * cosTheta));
    const float phi = 2.f * Pi * RandomUnit();
    const glm::vec3 tangent = SafeDirection(glm::cross(axis,
        std::abs(axis.y) < 0.9f ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f)));
    const glm::vec3 bitangent = glm::cross(axis, tangent);
    const glm::vec3 localVelocity = std::max(0.f, speed) *
        (axis * cosTheta + sinTheta * (tangent * std::cos(phi) + bitangent * std::sin(phi)));

    if (worldSpace)
    {
        particle.position = glm::vec3(emitterWorld * glm::vec4(localPosition, 1.f));
        particle.velocity = glm::mat3(emitterWorld) * localVelocity;
    }
    else
    {
        particle.position = localPosition;
        particle.velocity = localVelocity;
    }
    m_particles.push_back(particle);
}

void ParticleEmitter::Advance(float seconds, const glm::mat4& emitterWorld)
{
    if (!std::isfinite(seconds) || seconds <= 0.f) return;
    if (SyncConfiguredSolver()) Reset();
    m_collisionHits.clear();
    m_droppedCollisionHits = 0;
    const bool gpu = InitializeGpuSimulation();
    // Fifteen substeps is the maximum catch-up after a hitch. Both ordinary
    // playback and frame-sequence export use the same 60 Hz integration step.
    m_stepRemainder += std::min(static_cast<double>(seconds), 0.25);
    int steps = 0;
    while (m_stepRemainder + 1e-9 >= FixedStep && steps++ < 15)
    {
        if (gpu) ScheduleGpuStep(static_cast<float>(FixedStep), emitterWorld);
        else Step(static_cast<float>(FixedStep), emitterWorld);
        m_stepRemainder -= FixedStep;
    }
    if (m_stepRemainder >= FixedStep)
        m_stepRemainder = std::fmod(m_stepRemainder, FixedStep);
}

bool ParticleEmitter::Seek(double seconds, const glm::mat4& emitterWorld,
    uint64_t maxReplaySteps)
{
    return Seek(seconds, [&emitterWorld](double) { return emitterWorld; },
        maxReplaySteps);
}

bool ParticleEmitter::Seek(double seconds,
    const std::function<glm::mat4(double)>& worldAtTime,
    uint64_t maxReplaySteps)
{
    if (!worldAtTime || !std::isfinite(seconds) || seconds < 0.0 ||
        seconds > (static_cast<double>(maxReplaySteps) + 1.0) * FixedStep)
        return false;
    const double ticks = seconds / FixedStep;
    const uint64_t wholeSteps = static_cast<uint64_t>(
        std::floor(ticks + 1e-7));
    if (wholeSteps > maxReplaySteps) return false;

    Start();
    // Scrubbing is an explicit CPU reference path; it can inspect exact
    // particles without forcing a GPU readback on every drag update.
    m_gpuPreviewCpu = useGpuSimulation;
    for (uint64_t step = 0; step < wholeSteps; ++step)
        Step(static_cast<float>(FixedStep),
            worldAtTime(static_cast<double>(step + 1) * FixedStep));
    m_stepRemainder = std::max(0.0,
        seconds - static_cast<double>(wholeSteps) * FixedStep);
    if (m_stepRemainder < 1e-8) m_stepRemainder = 0.0;
    return true;
}

void ParticleEmitter::Step(float seconds, const glm::mat4& emitterWorld)
{
    int capacity = std::clamp(maxParticles, 0, ParticleLimit);
    if (Owner && Owner->GetScene())
        capacity = std::min(capacity, static_cast<int>(
            Owner->GetScene()->GetParticleCapacityFor(this)));
    TrimToCapacity(static_cast<size_t>(capacity));

    const bool fluidCoupling = fluidEnabled &&
        fluidCouplingMode != static_cast<int>(FluidCouplingMode::Off);
    if ((collisionMode != static_cast<int>(CollisionMode::None) || fluidCoupling) &&
        Owner && Owner->GetScene())
    {
        m_previousPositions.resize(m_particles.size());
        for (size_t index = 0; index < m_particles.size(); ++index)
            m_previousPositions[index] = m_particles[index].position;
    }

    glm::vec3 acceleration = gravity;
    if (!worldSpace)
    {
        const glm::mat3 basis(emitterWorld);
        const float determinant = glm::determinant(basis);
        if (std::isfinite(determinant) && std::abs(determinant) > 1e-8f)
            acceleration = glm::inverse(basis) * gravity;
    }
    m_solver->Step(m_particles.data(), m_particles.size(), seconds,
        { acceleration, drag });
    if (m_previousPositions.size() == m_particles.size() &&
        (collisionMode != static_cast<int>(CollisionMode::None) || fluidCoupling))
        ResolveCollisions(emitterWorld, seconds);
    m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
        [this](const Particle& particle)
        {
            if (particle.active && particle.age < particle.lifetime)
                return false;
            m_solver->OnParticleRemoved(particle.id);
            return true;
        }),
        m_particles.end());

    const uint64_t durationTicks = EmissionDurationTicks(duration);
    if (m_playing && durationTicks)
    {
        if (m_emissionTicks >= durationTicks)
        {
            if (loop)
            {
                m_emissionTicks = 0;
                Burst(burstOnStart);
            }
            else
                m_playing = false;
        }
    }

    int requested = std::exchange(m_pendingBurst, 0);
    if (m_playing && std::isfinite(rate) && rate > 0.f)
    {
        m_emitRemainder += static_cast<double>(rate) * seconds;
        const int available = std::max(0, capacity - static_cast<int>(m_particles.size()));
        const int continuous = static_cast<int>(std::min(
            std::floor(m_emitRemainder), static_cast<double>(available)));
        m_emitRemainder -= std::floor(m_emitRemainder);
        requested = std::min(ParticleLimit, requested + continuous);
    }
    if (m_playing) ++m_emissionTicks;
    requested = std::min(requested,
        capacity - static_cast<int>(m_particles.size()));
    for (int i = 0; i < requested; ++i)
        SpawnOne(emitterWorld);
    ++m_simulationTicks;
}

void ParticleEmitter::ResolveCollisions(const glm::mat4& emitterWorld,
    float seconds)
{
    if (!Owner || !Owner->GetScene()) return;
    const glm::mat3 basis(emitterWorld);
    const float determinant = glm::determinant(basis);
    if (!worldSpace && (!std::isfinite(determinant) ||
        std::abs(determinant) <= 1e-8f)) return;
    const glm::mat4 inverseWorld = worldSpace ? glm::mat4(1.f) :
        glm::inverse(emitterWorld);
    const glm::mat3 inverseBasis(inverseWorld);
    const auto& physics = Owner->GetScene()->GetPhysics();
    const bool fluidCoupling = fluidEnabled &&
        fluidCouplingMode != static_cast<int>(FluidCouplingMode::Off);
    for (size_t index = 0; index < m_particles.size(); ++index)
    {
        Particle& particle = m_particles[index];
        if (!particle.active || particle.age >= particle.lifetime) continue;
        const glm::vec3 start = worldSpace ? m_previousPositions[index] :
            glm::vec3(emitterWorld * glm::vec4(m_previousPositions[index], 1.f));
        const glm::vec3 end = worldSpace ? particle.position :
            glm::vec3(emitterWorld * glm::vec4(particle.position, 1.f));
        const glm::vec3 travel = end - start;
        const float distance = glm::length(travel);
        if (!std::isfinite(distance)) continue;
        // A point ray can miss a grazing wall and then begin the next step
        // inside it. Sweep the visible particle radius instead.
        const float collisionRadius = std::max(0.01f, particle.size);
        std::vector<Engine::Physics::Physics::RaycastHit> hits;
        if (distance > 1e-6f)
            hits = physics.SweepSphereAll(start, travel / distance,
                distance, collisionRadius, static_cast<uint32_t>(
                    fluidCoupling ? fluidCollisionMask : collisionMask));
        if (fluidCoupling)
        {
            // A stationary particle can still be hit by a moving boundary.
            // Sweep its relative path against the body's current collider.
            for (const auto& object : Owner->GetScene()->GetObjects())
            {
                if (!object || object.get() == Owner ||
                    !object->IsEnabledInHierarchy()) continue;
                auto* body = object->GetComponent<RigidBody>();
                if (!body || body->isTrigger || body->bodyType == "Static")
                    continue;
                const glm::vec3 bodyTravel = body->GetLinearVelocity() * seconds;
                const glm::vec3 relativeTravel = travel - bodyTravel;
                const float relativeDistance = glm::length(relativeTravel);
                if (!std::isfinite(relativeDistance) ||
                    relativeDistance <= 1e-6f) continue;
                const auto movingHits = physics.SweepSphereAll(start,
                    relativeTravel / relativeDistance, relativeDistance,
                    collisionRadius,
                    static_cast<uint32_t>(fluidCollisionMask));
                for (const auto& hit : movingHits)
                    if (hit.rigidBody == body)
                    {
                        hits.push_back(hit);
                        break;
                    }
            }
        }
        for (const auto& hit : hits)
        {
            if (hit.object == Owner ||
                (hit.rigidBody && hit.rigidBody->isTrigger) ||
                hit.kind == Engine::Physics::Physics::RaycastHitKind::PortalSplitPiece)
                continue;
            const glm::vec3 incoming = worldSpace ? particle.velocity :
                basis * particle.velocity;
            const glm::vec3 colliderVelocity = hit.rigidBody ?
                hit.rigidBody->GetLinearVelocity() : glm::vec3(0.f);
            const glm::vec3 relative = incoming - colliderVelocity;
            const float normalSpeed = glm::dot(relative, hit.normal);
            if (normalSpeed >= 0.f) continue;
            glm::vec3 outgoing(0.f);
            const bool killed = !fluidCoupling &&
                collisionMode == static_cast<int>(CollisionMode::Kill);
            if (killed)
                particle.active = false;
            else
            {
                const glm::vec3 tangent = relative - normalSpeed * hit.normal;
                outgoing = colliderVelocity +
                    tangent * (1.f - std::clamp(fluidCoupling ?
                        fluidRigidFriction : collisionFriction, 0.f, 1.f)) +
                    hit.normal * std::max(0.f, -normalSpeed) *
                        std::clamp(fluidCoupling ? fluidRigidBounce :
                            collisionBounce, 0.f, 1.f);
                const glm::vec3 resolved = hit.point + hit.normal *
                    (collisionRadius + 0.001f);
                particle.position = worldSpace ? resolved :
                    glm::vec3(inverseWorld * glm::vec4(resolved, 1.f));
                particle.velocity = worldSpace ? outgoing : inverseBasis * outgoing;
                if (fluidCoupling && fluidCouplingMode ==
                    static_cast<int>(FluidCouplingMode::GameplayImpulse) &&
                    hit.rigidBody && hit.rigidBody->bodyType == "Dynamic")
                    hit.rigidBody->AddImpulse((incoming - outgoing) *
                        std::max(0.f, fluid.particleMass) *
                        std::max(0.f, fluidImpulseScale));
            }
            if (m_collisionHits.size() < MaxCollisionHitsPerAdvance)
                m_collisionHits.push_back({ particle.id, hit.object,
                    hit.point, hit.normal, incoming, outgoing,
                    m_simulationTicks + 1, killed });
            else
                ++m_droppedCollisionHits;
            break;
        }
    }
}

bool ParticleEmitter::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    bool renderChanged = false;
    ui.DisabledLabel("Default: point, world space, 10/s, 2s life, 1024 max.");
    changed = ui.DragFloat("Rate / second", &rate, 0.1f, 0.f, 100000.f) || changed;
    changed = ui.SliderInt("Burst on start", &burstOnStart, 0, 10000) || changed;
    changed = ui.SliderInt("Capacity", &maxParticles, 0, 10000) || changed;
    changed = ui.DragFloat("Lifetime", &lifetime, 0.01f, 0.001f, 1000.f) || changed;
    changed = ui.DragFloat("Speed", &speed, 0.01f, 0.f, 10000.f) || changed;
    changed = ui.DragFloat("Spread (degrees)", &spreadDegrees, 0.5f, 0.f, 180.f) || changed;
    changed = ui.DragFloat3("Direction", &direction.x, 0.01f) || changed;
    changed = ui.DragFloat3("Gravity", &gravity.x, 0.01f) || changed;
    changed = ui.DragFloat("Drag", &drag, 0.01f, 0.f, 100.f) || changed;
    changed = ui.ColorEdit3("Color", &color.x) || changed;
    changed = ui.DragFloat("Size", &size, 0.01f, 0.f, 100.f) || changed;
    const char* shapes[] = { "Point", "Box", "Sphere" };
    changed = ui.Combo("Shape", &shape, shapes, 3) || changed;
    if (shape == static_cast<int>(Shape::Box))
        changed = ui.DragFloat3("Box half extents", &boxExtents.x, 0.01f) || changed;
    if (shape == static_cast<int>(Shape::Sphere))
        changed = ui.DragFloat("Sphere radius", &sphereRadius, 0.01f, 0.f, 1000.f) || changed;
    changed = ui.Checkbox("World space", &worldSpace) || changed;
    changed = ui.Checkbox("GPU simulation", &useGpuSimulation) || changed;
    if (useGpuSimulation)
        ui.DisabledLabel("GPU uses world-space ballistic motion; other modes use CPU.");
    ui.Separator();
    ui.DisabledLabel("Fluid solver (CPU, 3D SPH)");
    changed = ui.Checkbox("Enable fluid", &fluidEnabled) || changed;
    if (fluidEnabled)
    {
        changed = ui.DragFloat("Fluid smoothing radius", &fluid.smoothingRadius,
            0.01f, 0.02f, 10.f) || changed;
        changed = ui.DragFloat("Fluid particle mass", &fluid.particleMass,
            0.001f, 0.00001f, 100.f) || changed;
        changed = ui.DragFloat("Fluid rest density", &fluid.restDensity,
            0.1f, 0.001f, 100000.f) || changed;
        changed = ui.DragFloat("Fluid pressure stiffness",
            &fluid.pressureStiffness, 0.1f, 0.f, 10000.f) || changed;
        changed = ui.DragFloat("Fluid viscosity", &fluid.viscosity,
            0.01f, 0.f, 100.f) || changed;
        changed = ui.SliderInt("Fluid substeps", &fluid.substeps, 1, 16) || changed;
        changed = ui.DragFloat3("Fluid bounds min", &fluid.boundsMin.x,
            0.01f) || changed;
        changed = ui.DragFloat3("Fluid bounds max", &fluid.boundsMax.x,
            0.01f) || changed;
        changed = ui.DragFloat("Fluid boundary bounce", &fluid.boundaryBounce,
            0.01f, 0.f, 1.f) || changed;
        changed = ui.DragFloat("Fluid max speed", &fluid.maxSpeed,
            0.1f, 0.01f, 1000.f) || changed;
        renderChanged = ui.Checkbox("Fluid water visual", &fluidVisual) || renderChanged;
        if (fluidVisual)
            renderChanged = ui.DragFloat("Fluid visual radius",
                &fluidVisualRadiusScale, 0.01f, 0.5f, 3.f) || renderChanged;
        const char* fluidCouplingModes[] = {
            "Off", "Fluid only", "Fluid and dynamic body" };
        changed = ui.Combo("Fluid rigid coupling", &fluidCouplingMode,
            fluidCouplingModes, 3) || changed;
        if (fluidCouplingMode != static_cast<int>(FluidCouplingMode::Off))
        {
            changed = ui.DragFloat("Fluid rigid bounce", &fluidRigidBounce,
                0.01f, 0.f, 1.f) || changed;
            changed = ui.DragFloat("Fluid rigid friction", &fluidRigidFriction,
                0.01f, 0.f, 1.f) || changed;
            if (fluidCouplingMode == static_cast<int>(FluidCouplingMode::GameplayImpulse))
                changed = ui.DragFloat("Fluid impulse scale",
                    &fluidImpulseScale, 0.01f, 0.f, 100.f) || changed;
            uint32_t fluidMask = static_cast<uint32_t>(fluidCollisionMask);
            if (ui.InputUInt("Fluid collision mask", &fluidMask))
            { fluidCollisionMask = static_cast<int>(fluidMask); changed = true; }
        }
    }
    ui.Separator();
    changed = ui.Checkbox("Play on start", &playOnStart) || changed;
    changed = ui.DragFloat("Emission duration (0 = infinite)", &duration,
        0.01f, 0.f, 1000.f) || changed;
    if (duration > 0.f)
        changed = ui.Checkbox("Loop emission", &loop) || changed;
    uint32_t editorSeed = static_cast<uint32_t>(std::max(1, seed));
    if (ui.InputUInt("Seed", &editorSeed))
    { seed = static_cast<int>(std::clamp(editorSeed, 1u, 2147483647u)); changed = true; }
    renderChanged = DrawAssetPathPicker(ui, this, "texturePath", "Texture", "texture", texturePath) || renderChanged;
    renderChanged = ui.DragFloat("Opacity", &opacity, 0.01f, 0.f, 1.f) || renderChanged;
    const char* blends[] = { "Additive", "Alpha" };
    renderChanged = ui.Combo("Blend", &blendMode, blends, 2) || renderChanged;
    const char* collisionModes[] = { "Off", "Kill on contact", "Bounce" };
    changed = ui.Combo("Collision", &collisionMode, collisionModes, 3) || changed;
    if (collisionMode == static_cast<int>(CollisionMode::Bounce))
    {
        changed = ui.DragFloat("Bounce", &collisionBounce, 0.01f,
            0.f, 1.f) || changed;
        changed = ui.DragFloat("Collision friction", &collisionFriction,
            0.01f, 0.f, 1.f) || changed;
    }
    if (collisionMode != static_cast<int>(CollisionMode::None))
    {
        uint32_t mask = static_cast<uint32_t>(collisionMask);
        if (ui.InputUInt("Collision mask", &mask))
        {
            collisionMask = static_cast<int>(mask);
            changed = true;
        }
    }
    if (changed)
    {
        const bool resumePreview = m_editorPreviewRunning;
        SyncConfiguredSolver();
        Reset();
        if (resumePreview)
        {
            Play();
            m_editorPreviewRunning = true;
        }
    }
    ui.Separator();
    const std::string count = std::to_string(m_particles.size()) + " / " +
        std::to_string(std::clamp(maxParticles, 0, ParticleLimit));
    ui.ValueLabel("Live particles", count.c_str());
    if (Owner && Owner->GetScene())
    {
        const auto* scene = Owner->GetScene();
        const std::string sceneCount =
            std::to_string(scene->GetParticleLiveCount()) + " / " +
            std::to_string(scene->GetParticleBudget());
        ui.ValueLabel("Scene particles", sceneCount.c_str());
    }
    ui.ValueLabel("Emission", m_playing ? "Playing" : "Stopped / paused");
    ui.ValueLabel("Edit preview", IsEditorPreviewRunning() ? "Running" : "Paused");
    ui.ValueLabel("Simulation", IsGpuSimulationActive() ? "GPU" : "CPU");
    if (collisionMode != static_cast<int>(CollisionMode::None))
    {
        const std::string hitCount = std::to_string(m_collisionHits.size());
        ui.ValueLabel("Hits last advance", hitCount.c_str());
    }
    if (ui.Button("Play", 65.f))
    {
        Play();
        m_editorPreviewRunning = true;
    }
    ui.SameLine();
    if (ui.Button("Pause", 65.f))
    {
        Pause();
        m_editorPreviewRunning = false;
    }
    ui.SameLine();
    if (ui.Button("Restart", 75.f))
    {
        Restart();
        m_editorPreviewRunning = true;
    }
    ui.SameLine();
    if (ui.Button("Clear", 65.f)) Reset();
    if (ui.Button("Burst", 65.f))
    {
        Burst(std::max(1, burstOnStart));
        // A paused preview has no scene clock. Materialize the burst through
        // the same fixed simulation step used by Update and Seek.
        if (!m_playing && Owner)
            Advance(static_cast<float>(FixedStep),
                Owner->transform.GetWorldMatrix());
    }
    ui.SameLine();
    if (ui.Button("Step 1/60", 90.f) && Owner)
        Advance(1.f / 60.f, Owner->transform.GetWorldMatrix());
    ui.DisabledLabel("Play previews in Edit mode; Step advances one 60 Hz tick.");
    float previewSeconds = static_cast<float>(
        static_cast<double>(m_simulationTicks) * FixedStep + m_stepRemainder);
    if (ui.DragFloat("Preview time (s)", &previewSeconds, 0.01f, 0.f, 60.f))
    {
        if (Seek(previewSeconds, Owner ? Owner->transform.GetWorldMatrix() :
                glm::mat4(1.f)))
        {
            Pause();
            m_editorPreviewRunning = false;
        }
    }
    ui.DisabledLabel("Scrub replays from scene start and pauses at the target time.");
    return changed || renderChanged;
}
}
