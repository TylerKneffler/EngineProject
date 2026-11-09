#include "Core/Scene/Rendering/SceneRenderer/SceneRendererParticles.h"

#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Engine::Scene::Renderer
{
using Engine::Core::Object;
using Engine::Graphics::IGraphicsContext;
using Engine::Graphics::IGraphicsBuffer;
namespace
{
struct ParticleGPUData
{
    glm::vec4 positionSize;
    glm::vec4 colorAlpha;
    glm::vec4 rotation;
};

struct FluidSurfaceParticleData
{
    glm::vec4 positionAge;
    glm::vec4 velocityLife;
    glm::vec4 colorSize;
    glm::uvec4 meta;
};

struct ParticleCBData
{
    glm::mat4 viewProjection;
    glm::vec4 cameraRight;
    glm::vec4 cameraUp;
    glm::vec4 particleParams;
    glm::vec4 portalClipPlane;
};

static_assert(sizeof(ParticleGPUData) == 48, "Particle vertex layout must match Particle.hlsl");
static_assert(sizeof(FluidSurfaceParticleData) == 64, "Fluid surface buffer must match Particle.hlsl");
static_assert(sizeof(ParticleCBData) == 128, "Particle constants must fit Vulkan push constants");
}

void SceneRendererParticles::RecordGpuSimulation(Scene& scene, IGraphicsContext* context)
{
    if (!context) return;
    for (const auto& object : scene.m_objects)
        if (object && object->IsEnabledInHierarchy())
            if (auto* emitter = object->GetComponent<
                    Engine::Components::ParticleEmitter>())
                emitter->RecordGpuSimulation(context);
}

void SceneRendererParticles::Render(Scene& scene, IGraphicsContext* context,
    const glm::mat4& viewProjection, const glm::mat4& view,
    const Object* contentScopeRoot, const glm::vec4& clipPlane,
    bool portalView)
{
    if (!context || !scene.m_particlePipeline || !scene.m_particleAlphaPipeline ||
        !scene.m_particlePortalPipeline || !scene.m_particlePortalAlphaPipeline ||
        !scene.m_graphicsProvider)
        return;
    auto* contextFactory = scene.m_graphicsProvider->GetContextFactory();
    auto* bufferFactory = scene.m_graphicsProvider->GetBufferFactory();
    if (!contextFactory || !bufferFactory)
        return;
    const uint32_t frameSlot = contextFactory->GetPreparedFrameSlot();
    if (frameSlot >= scene.m_particleDrawSlots.size())
        scene.m_particleDrawSlots.resize(static_cast<size_t>(frameSlot) + 1u);
    const uint64_t serial = contextFactory->GetPreparedFrameSerial();
    auto& slot = scene.m_particleDrawSlots[frameSlot];
    if (serial == 0 || serial != slot.preparedSerial)
    {
        slot.preparedSerial = serial;
        slot.constantCursor = 0;
        slot.batches.clear();
        slot.fluidSurfaces.clear();
        std::vector<ParticleGPUData> records;
        constexpr size_t RenderLimit = 100000;
        for (const auto& object : scene.m_objects)
        {
            if (!object || !object->IsEnabledInHierarchy()) continue;
            auto* emitter = object->GetComponent<Engine::Components::ParticleEmitter>();
            if (!emitter) continue;
            const uint32_t batchFirst = static_cast<uint32_t>(records.size());
            const glm::mat4 world = object->transform.GetWorldMatrix();
            constexpr size_t FluidSurfaceLimit = 512;
            if (emitter->fluidEnabled && emitter->fluidVisual &&
                !emitter->GetParticles().empty())
            {
                const auto& particles = emitter->GetParticles();
                const size_t sampleCount = std::min(particles.size(), FluidSurfaceLimit);
                std::vector<FluidSurfaceParticleData> surface(sampleCount + 1u);
                surface[0].meta.x = static_cast<uint32_t>(sampleCount);
                Engine::Rendering::RenderJobs::Get().For(sampleCount, 128, [&](size_t i)
                {
                    const auto& particle = particles[i * particles.size() / sampleCount];
                    const glm::vec3 position = emitter->worldSpace
                        ? particle.position
                        : glm::vec3(world * glm::vec4(particle.position, 1.f));
                    surface[i + 1u].positionAge = glm::vec4(position, 0.f);
                    surface[i + 1u].velocityLife = glm::vec4(
                        emitter->worldSpace ? particle.velocity :
                        glm::mat3(world) * particle.velocity, 0.f);
                    surface[i + 1u].colorSize = glm::vec4(particle.color,
                        particle.size * std::clamp(emitter->fluidVisualRadiusScale,
                            0.5f, 3.f));
                });
                auto buffer = bufferFactory->CreateBuffer(
                    IGraphicsBuffer::Usage::ShaderResource,
                    IGraphicsBuffer::AccessMode::Upload,
                    static_cast<uint64_t>(surface.size()) * sizeof(surface[0]),
                    surface.data(), sizeof(surface[0]));
                if (!buffer)
                    throw std::runtime_error("Could not create fluid surface buffer");
                slot.fluidSurfaces.push_back({ std::move(buffer), object.get(),
                    std::clamp(emitter->opacity, 0.f, 1.f),
                    static_cast<uint32_t>(particles.size()) });
                continue;
            }
            const auto& particles = emitter->GetParticles();
            const size_t count = std::min(particles.size(),
                RenderLimit - records.size());
            records.resize(records.size() + count);
            Engine::Rendering::RenderJobs::Get().For(count, 512, [&](size_t index)
            {
                const auto& particle = particles[index];
                const glm::vec3 position = emitter->worldSpace
                    ? particle.position
                    : glm::vec3(world * glm::vec4(particle.position, 1.f));
                const float fade = std::clamp(1.f - particle.age /
                    std::max(particle.lifetime, 0.001f), 0.f, 1.f);
                records[batchFirst + index] = { glm::vec4(position, particle.size),
                    glm::vec4(particle.color, fade *
                        std::clamp(emitter->opacity, 0.f, 1.f)),
                    glm::vec4(particle.rotation, 0.f, 0.f, 0.f) };
            });
            const uint32_t batchCount = static_cast<uint32_t>(records.size()) - batchFirst;
            if (batchCount)
                slot.batches.push_back({ batchFirst, batchCount,
                    emitter->PrepareTexture(scene.m_graphicsProvider),
                    emitter->blendMode == static_cast<int>(
                        Engine::Components::ParticleEmitter::BlendMode::Alpha),
                    object.get() });
            if (records.size() >= RenderLimit) break;
        }
        slot.particleCount = static_cast<uint32_t>(records.size());
        if (slot.particleCount)
        {
            if (slot.particleCount > slot.vertexCapacity)
            {
                if (slot.vertices)
                    scene.m_retiredRenderBuffers.push_back(std::move(slot.vertices));
                slot.vertexCapacity = ExpandedCapacity(slot.vertexCapacity,
                    slot.particleCount);
                slot.vertices = bufferFactory->CreateBuffer(
                    IGraphicsBuffer::Usage::VertexBuffer,
                    IGraphicsBuffer::AccessMode::Upload,
                    static_cast<uint64_t>(slot.vertexCapacity) * sizeof(ParticleGPUData));
                slot.mappedVertices = slot.vertices ? slot.vertices->Map() : nullptr;
                if (!slot.mappedVertices)
                    throw std::runtime_error("Could not map particle vertex buffer");
            }
            const uint64_t bytes = static_cast<uint64_t>(slot.particleCount) *
                sizeof(ParticleGPUData);
            std::memcpy(slot.mappedVertices, records.data(), static_cast<size_t>(bytes));
            slot.vertices->FlushMappedWrites(0, bytes);
        }
    }
    uint32_t gpuBatchCount = 0;
    for (const auto& object : scene.m_objects)
        if (object && object->IsEnabledInHierarchy())
            if (const auto* emitter = object->GetComponent<
                    Engine::Components::ParticleEmitter>())
                if (emitter->GetGpuDrawBuffer()) ++gpuBatchCount;
    if ((!slot.particleCount || !slot.vertices) && !gpuBatchCount &&
        slot.fluidSurfaces.empty()) return;

    const uint32_t requiredConstants = slot.constantCursor +
        static_cast<uint32_t>(slot.batches.size() + slot.fluidSurfaces.size()) +
        gpuBatchCount;
    if (requiredConstants > slot.constantCapacity)
    {
        if (slot.constants)
            scene.m_retiredRenderBuffers.push_back(std::move(slot.constants));
        slot.constantCapacity = ExpandedCapacity(slot.constantCapacity,
            requiredConstants);
        slot.constants = bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::ConstantBuffer,
            IGraphicsBuffer::AccessMode::Upload,
            static_cast<uint64_t>(slot.constantCapacity) * 256u,
            nullptr, sizeof(ParticleCBData));
        slot.mappedConstants = slot.constants ? slot.constants->Map() : nullptr;
        if (!slot.mappedConstants)
            throw std::runtime_error("Could not map particle constant buffer");
    }
    const glm::mat4 cameraWorld = glm::inverse(view);
    ParticleCBData constants{};
    constants.viewProjection = viewProjection;
    constants.cameraRight = glm::vec4(glm::normalize(glm::vec3(cameraWorld[0])), 0.f);
    constants.cameraUp = glm::vec4(glm::normalize(glm::vec3(cameraWorld[1])), 0.f);
    constants.portalClipPlane = clipPlane;
    if (!portalView)
        context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::Transparent);
    for (const auto& batch : slot.batches)
    {
        if (portalView && !scene.IsObjectInSpatialRegion(batch.owner,
            contentScopeRoot)) continue;
        const uint64_t offset = static_cast<uint64_t>(slot.constantCursor++) * 256u;
        constants.particleParams.x = batch.texture ? 1.f : 0.f;
        constants.particleParams.z = 0.f;
        std::memcpy(static_cast<uint8_t*>(slot.mappedConstants) + offset,
            &constants, sizeof(constants));
        slot.constants->FlushMappedWrites(offset, sizeof(constants));
        context->SetPipeline(portalView
            ? (batch.alphaBlend ? scene.m_particlePortalAlphaPipeline.get()
                : scene.m_particlePortalPipeline.get())
            : (batch.alphaBlend ? scene.m_particleAlphaPipeline.get()
                : scene.m_particlePipeline.get()));
        context->SetConstantBuffer(0, slot.constants.get(), offset);
        if(batch.texture) Engine::Components::Texture::RequestGraphicsFootprint(batch.texture->GetGraphicsTexture(),
            std::numeric_limits<float>::infinity()); // CPU/GPU particle size varies after preparation.
        context->SetTexture(0, batch.texture
            ? batch.texture->GetGraphicsTexture() : nullptr);
        context->SetVertexBuffer(0, slot.vertices.get(), sizeof(ParticleGPUData),
            static_cast<uint64_t>(batch.first) * sizeof(ParticleGPUData));
        context->DrawInstanced(6, batch.count);
        if (portalView)
            scene.m_lastParticlePortalDrawCount += batch.count;
    }
    for (const auto& surface : slot.fluidSurfaces)
    {
        if (portalView && !scene.IsObjectInSpatialRegion(surface.owner,
            contentScopeRoot)) continue;
        const uint64_t offset = static_cast<uint64_t>(slot.constantCursor++) * 256u;
        constants.particleParams = glm::vec4(0.f, surface.opacity,
            static_cast<float>(scene.GetElapsedTime()), 0.f);
        std::memcpy(static_cast<uint8_t*>(slot.mappedConstants) + offset,
            &constants, sizeof(constants));
        slot.constants->FlushMappedWrites(offset, sizeof(constants));
        context->SetPipeline(portalView ? scene.m_fluidSurfacePortalPipeline.get()
            : scene.m_fluidSurfacePipeline.get());
        context->SetConstantBuffer(0, slot.constants.get(), offset);
        context->SetStructuredBuffer(6, surface.particles.get());
        context->DrawInstanced(6, 1);
        if (portalView) scene.m_lastParticlePortalDrawCount += surface.count;
    }
    for (const auto& object : scene.m_objects)
    {
        if (!object || !object->IsEnabledInHierarchy() ||
            (portalView && !scene.IsObjectInSpatialRegion(object.get(),
                contentScopeRoot))) continue;
        auto* emitter = object->GetComponent<Engine::Components::ParticleEmitter>();
        if (!emitter || !emitter->GetGpuDrawBuffer()) continue;
        const auto texture = emitter->PrepareTexture(scene.m_graphicsProvider);
        const uint64_t offset = static_cast<uint64_t>(slot.constantCursor++) * 256u;
        constants.particleParams.x = texture ? 1.f : 0.f;
        constants.particleParams.y = std::clamp(emitter->opacity, 0.f, 1.f);
        constants.particleParams.z = 0.f;
        std::memcpy(static_cast<uint8_t*>(slot.mappedConstants) + offset,
            &constants, sizeof(constants));
        slot.constants->FlushMappedWrites(offset, sizeof(constants));
        const bool alpha = emitter->blendMode == static_cast<int>(
            Engine::Components::ParticleEmitter::BlendMode::Alpha);
        context->SetPipeline(portalView
            ? (alpha ? scene.m_particleGpuPortalAlphaPipeline.get()
                : scene.m_particleGpuPortalPipeline.get())
            : (alpha ? scene.m_particleGpuAlphaPipeline.get()
                : scene.m_particleGpuPipeline.get()));
        context->SetConstantBuffer(0, slot.constants.get(), offset);
        if(texture) Engine::Components::Texture::RequestGraphicsFootprint(texture->GetGraphicsTexture(),std::numeric_limits<float>::infinity());
        context->SetTexture(0, texture ? texture->GetGraphicsTexture() : nullptr);
        context->SetStructuredBuffer(6, emitter->GetGpuDrawBuffer());
        context->DrawInstanced(6, emitter->GetGpuDrawCapacity());
        if (portalView)
            scene.m_lastParticlePortalDrawCount += emitter->GetGpuDrawCapacity();
    }
    if (!portalView)
        context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Transparent);
}

}
