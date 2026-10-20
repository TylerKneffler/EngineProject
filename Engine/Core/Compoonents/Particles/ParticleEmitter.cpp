#include "ParticleEmitter.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Materials/Texture.h"
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

namespace Engine::Components
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr int ParticleLimit = 1000000;
constexpr double FixedStep = 1.0 / 60.0;

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
}

void ParticleEmitter::Reset()
{
    m_solver->Reset();
    m_particles.clear();
    m_randomState = seed > 0 ? static_cast<uint32_t>(seed) : 1u;
    m_nextId = 1;
    m_emitRemainder = 0.0;
    m_stepRemainder = 0.0;
    m_emissionTicks = 0;
    m_pendingBurst = 0;
    m_playing = false;
}

void ParticleEmitter::SetSolver(std::unique_ptr<IParticleSolver> solver)
{
    m_solver = solver ? std::move(solver) :
        std::make_unique<BallisticParticleSolver>();
    Reset();
}

void ParticleEmitter::Play()
{
    const uint64_t durationTicks = EmissionDurationTicks(duration);
    if (!m_playing && durationTicks && m_emissionTicks >= durationTicks)
    {
        m_emissionTicks = 0;
        Burst(burstOnStart);
    }
    m_playing = true;
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
        const glm::vec3 radius(std::max(0.f, particle.size) * CornerRadius);
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
    m_playing = playOnStart;
    if (m_playing)
        Burst(burstOnStart);
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
    // Fifteen substeps is the maximum catch-up after a hitch. Both ordinary
    // playback and frame-sequence export use the same 60 Hz integration step.
    m_stepRemainder += std::min(static_cast<double>(seconds), 0.25);
    int steps = 0;
    while (m_stepRemainder + 1e-9 >= FixedStep && steps++ < 15)
    {
        Step(static_cast<float>(FixedStep), emitterWorld);
        m_stepRemainder -= FixedStep;
    }
    if (m_stepRemainder >= FixedStep)
        m_stepRemainder = std::fmod(m_stepRemainder, FixedStep);
}

void ParticleEmitter::Step(float seconds, const glm::mat4& emitterWorld)
{
    const int capacity = std::clamp(maxParticles, 0, ParticleLimit);
    if (m_particles.size() > static_cast<size_t>(capacity))
        m_particles.resize(static_cast<size_t>(capacity));

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
    m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
        [](const Particle& particle)
        { return !particle.active || particle.age >= particle.lifetime; }),
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
}

bool ParticleEmitter::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    bool renderChanged = false;
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
    if (changed) Reset();
    return changed || renderChanged;
}
}
