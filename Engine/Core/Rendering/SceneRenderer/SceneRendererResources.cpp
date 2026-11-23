#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererResources.h"

#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/RenderUtilities.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
namespace
{
constexpr std::size_t kObjectGPUDataSize = sizeof(ObjectGPUData);
static_assert(kObjectGPUDataSize == 1936u, "Object buffer layout must match Object.hlsl");

std::vector<float> GenerateBrdfIntegrationLut(uint32_t resolution)
{
    std::vector<float> pixels(static_cast<size_t>(resolution) *
        resolution * 4u, 0.f);
    for (uint32_t y = 0; y < resolution; ++y)
    for (uint32_t x = 0; x < resolution; ++x)
    {
        const float noV = (static_cast<float>(x) + 0.5f) /
            static_cast<float>(resolution);
        const float roughness = (static_cast<float>(y) + 0.5f) /
            static_cast<float>(resolution);
        const glm::vec2 integrated = IntegrateBrdf(noV, roughness);
        const size_t offset = (static_cast<size_t>(y) * resolution + x) * 4u;
        pixels[offset] = integrated.x;
        pixels[offset + 1u] = integrated.y;
        pixels[offset + 3u] = 1.f;
    }
    return pixels;
}
}

void SceneRendererResources::Initialize(SceneRenderer& renderer, Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    Scene& scene = renderer.GetScene();
    if (!graphicsProvider)
        throw std::runtime_error("SceneRenderer requires a graphics provider (call Scene::Init first)");

    if (renderer.m_activeNativeFrameBuffers)
        scene.WaitForGpuResourceDestruction();
    renderer.m_frameUploadContexts.clear();
    renderer.m_activeFrameUploadContext = nullptr;
    renderer.m_activeNativeFrameBuffers = nullptr;
    renderer.m_activeRenderPassBuffers = nullptr;

    renderer.m_graphicsProvider = graphicsProvider;
    renderer.m_staticRenderItemCache.clear();
    renderer.m_particleDrawSlots.clear();

    auto* bufferFactory = renderer.m_graphicsProvider->GetBufferFactory();
    if (!bufferFactory)
        throw std::runtime_error("Failed to get buffer factory from graphics provider");

    renderer.m_gridConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        256);
    if (!renderer.m_gridConstantBuffer)
        throw std::runtime_error("Failed to create grid constant buffer");
    renderer.m_gridCBMapped = renderer.m_gridConstantBuffer->Map();
    if (!renderer.m_gridCBMapped)
        throw std::runtime_error("Failed to map grid constant buffer");

    renderer.m_skyboxConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(SceneRenderer::kCloudConstantSlot + 1u) * SceneRenderer::kCBStride);
    if (!renderer.m_skyboxConstantBuffer)
        throw std::runtime_error("Failed to create skybox constant buffer");
    renderer.m_skyboxCBMapped = renderer.m_skyboxConstantBuffer->Map();
    if (!renderer.m_skyboxCBMapped)
        throw std::runtime_error("Failed to map skybox constant buffer");

    const uint64_t portalSlotsPerView =
        static_cast<uint64_t>(renderer.m_objectCapacity) + 2u;
    const uint64_t portalObjectSlots =
        static_cast<uint64_t>(SceneRenderer::kMaxPortalRenderViews) * portalSlotsPerView;
    const uint64_t objectCBSize = (static_cast<uint64_t>(renderer.m_objectCapacity) +
        portalObjectSlots + SceneRenderer::kMaxSpatialDebugDraws) * SceneRenderer::kCBStride;
    renderer.m_objectConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        objectCBSize, nullptr, sizeof(uint32_t) * 4u);
    if (!renderer.m_objectConstantBuffer)
        throw std::runtime_error("Failed to create object constant buffer");
    renderer.m_objectCBMapped = renderer.m_objectConstantBuffer->Map();
    if (!renderer.m_objectCBMapped)
        throw std::runtime_error("Failed to map object constant buffer");

    renderer.m_objectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        (static_cast<uint64_t>(renderer.m_objectCapacity) + SceneRenderer::kMaxSpatialDebugDraws) *
            kObjectGPUDataSize,
        nullptr, kObjectGPUDataSize);
    renderer.m_objectDataMapped = renderer.m_objectDataBuffer ? renderer.m_objectDataBuffer->Map() : nullptr;
    renderer.m_objectUploadCache.Clear(); renderer.m_shadowUploadCache.Clear();
    if (!renderer.m_objectDataMapped)
        throw std::runtime_error("Failed to create object structured buffer");

    renderer.m_shadowConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(renderer.m_objectCapacity) * 4u * SceneRenderer::kCBStride,
        nullptr, sizeof(uint32_t) * 4u);
    renderer.m_shadowCBMapped = renderer.m_shadowConstantBuffer
        ? renderer.m_shadowConstantBuffer->Map() : nullptr;
    renderer.m_shadowObjectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(renderer.m_objectCapacity) * 4u * kObjectGPUDataSize,
        nullptr, kObjectGPUDataSize);
    renderer.m_shadowObjectDataMapped = renderer.m_shadowObjectDataBuffer
        ? renderer.m_shadowObjectDataBuffer->Map() : nullptr;
    if (!renderer.m_shadowCBMapped || !renderer.m_shadowObjectDataMapped)
        throw std::runtime_error("Failed to create directional shadow buffers");

    renderer.m_portalObjectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        portalObjectSlots * kObjectGPUDataSize,
        nullptr, kObjectGPUDataSize);
    renderer.m_portalObjectDataMapped = renderer.m_portalObjectDataBuffer
        ? renderer.m_portalObjectDataBuffer->Map() : nullptr;
    if (!renderer.m_portalObjectDataMapped)
        throw std::runtime_error("Failed to create portal object structured buffer");

    renderer.m_lightDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(SceneRenderer::kMaxLights) *
            sizeof(Engine::Model::LightData),
        nullptr, sizeof(Engine::Model::LightData));
    renderer.m_lightDataMapped = renderer.m_lightDataBuffer ? renderer.m_lightDataBuffer->Map() : nullptr;
    if (!renderer.m_lightDataMapped)
        throw std::runtime_error("Failed to create light structured buffer");

    renderer.m_boneDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(renderer.m_boneMatrixCapacity) *
            sizeof(glm::mat4),
        nullptr, sizeof(glm::mat4));
    renderer.m_boneDataMapped = renderer.m_boneDataBuffer ? renderer.m_boneDataBuffer->Map() : nullptr;
    if (!renderer.m_boneDataMapped)
        throw std::runtime_error("Failed to create bone palette structured buffer");

    const std::array<glm::vec4, 3> emptyMorphDelta{};
    const glm::vec4 emptyMorphWeights(0.f);
    renderer.m_emptyMorphDeltaBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        sizeof(emptyMorphDelta), emptyMorphDelta.data(), sizeof(glm::vec4));
    renderer.m_emptyMorphWeightBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        sizeof(emptyMorphWeights), &emptyMorphWeights, sizeof(emptyMorphWeights));
    if (!renderer.m_emptyMorphDeltaBuffer || !renderer.m_emptyMorphWeightBuffer)
        throw std::runtime_error("Failed to create empty morph structured buffers");

    constexpr uint32_t brdfLutResolution = 128u;
    const std::vector<float> brdfLut = GenerateBrdfIntegrationLut(
        brdfLutResolution);
    auto* textureFactory = renderer.m_graphicsProvider->GetTextureFactory();
    renderer.m_brdfIntegrationLut = textureFactory ? textureFactory->CreateTexture2D(
        brdfLutResolution, brdfLutResolution,
        reinterpret_cast<const uint8_t*>(brdfLut.data()), 1u,
        Engine::Graphics::GraphicsTextureFormat::Rgba32Float, false) : nullptr;
    if (!renderer.m_brdfIntegrationLut)
        throw std::runtime_error("Failed to create split-sum BRDF integration LUT");

    renderer.m_portalApertureBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(SceneRenderer::kMaxSpatialObjects) * SceneRenderer::kMaxSpatialVerticesPerObject *
            sizeof(Engine::Model::AnimationVertex));
    renderer.m_portalApertureMapped = renderer.m_portalApertureBuffer
        ? renderer.m_portalApertureBuffer->Map() : nullptr;
    if (!renderer.m_portalApertureMapped)
        throw std::runtime_error("Failed to create portal aperture vertex buffer");
}

void SceneRendererResources::EnsureObjectRenderCapacity(SceneRenderer& renderer, uint32_t requiredObjects)
{
    Scene& scene = renderer.GetScene();
    if (requiredObjects <= renderer.m_objectCapacity)
        return;
    if (!renderer.m_graphicsProvider || !renderer.m_graphicsProvider->GetBufferFactory())
        throw std::runtime_error("Cannot grow scene object buffers before graphics initialization");

    const uint32_t capacity = ExpandedCapacity(renderer.m_objectCapacity, requiredObjects);
    constexpr uint32_t kMaximumAddressableCapacity =
        (std::numeric_limits<uint32_t>::max() - SceneRenderer::kMaxSpatialDebugDraws) /
        (SceneRenderer::kMaxPortalRenderViews + 1u);
    if (capacity > kMaximumAddressableCapacity)
        throw std::runtime_error("Scene draw count exceeds portal buffer index range");
    const uint64_t portalSlotsPerView = static_cast<uint64_t>(capacity) + 2u;
    const uint64_t portalSlots =
        static_cast<uint64_t>(SceneRenderer::kMaxPortalRenderViews) * portalSlotsPerView;
    const uint64_t constantSlots = static_cast<uint64_t>(capacity) +
        portalSlots + SceneRenderer::kMaxSpatialDebugDraws;
    auto* factory = renderer.m_graphicsProvider->GetBufferFactory();
    auto constantBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        constantSlots * SceneRenderer::kCBStride, nullptr, sizeof(uint32_t) * 4u);
    auto objectBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        (static_cast<uint64_t>(capacity) + SceneRenderer::kMaxSpatialDebugDraws) *
            kObjectGPUDataSize, nullptr, kObjectGPUDataSize);
    auto portalBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        portalSlots * kObjectGPUDataSize, nullptr, kObjectGPUDataSize);
    auto shadowConstantBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(capacity) * 4u * SceneRenderer::kCBStride,
        nullptr, sizeof(uint32_t) * 4u);
    auto shadowObjectBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(capacity) * 4u * kObjectGPUDataSize,
        nullptr, kObjectGPUDataSize);
    void* constantMapped = constantBuffer ? constantBuffer->Map() : nullptr;
    void* objectMapped = objectBuffer ? objectBuffer->Map() : nullptr;
    void* portalMapped = portalBuffer ? portalBuffer->Map() : nullptr;
    void* shadowConstantMapped = shadowConstantBuffer
        ? shadowConstantBuffer->Map() : nullptr;
    void* shadowObjectMapped = shadowObjectBuffer
        ? shadowObjectBuffer->Map() : nullptr;
    if (!constantMapped || !objectMapped || !portalMapped ||
        !shadowConstantMapped || !shadowObjectMapped)
    {
        if (constantMapped) constantBuffer->Unmap();
        if (objectMapped) objectBuffer->Unmap();
        if (portalMapped) portalBuffer->Unmap();
        if (shadowConstantMapped) shadowConstantBuffer->Unmap();
        if (shadowObjectMapped) shadowObjectBuffer->Unmap();
        throw std::runtime_error("Failed to grow scene object buffers to " +
            std::to_string(capacity) + " draw records");
    }

    if (renderer.m_objectConstantBuffer && renderer.m_objectCBMapped)
        renderer.m_objectConstantBuffer->Unmap();
    if (renderer.m_objectDataBuffer && renderer.m_objectDataMapped)
        renderer.m_objectDataBuffer->Unmap();
    if (renderer.m_portalObjectDataBuffer && renderer.m_portalObjectDataMapped)
        renderer.m_portalObjectDataBuffer->Unmap();
    if (renderer.m_shadowConstantBuffer && renderer.m_shadowCBMapped)
        renderer.m_shadowConstantBuffer->Unmap();
    if (renderer.m_shadowObjectDataBuffer && renderer.m_shadowObjectDataMapped)
        renderer.m_shadowObjectDataBuffer->Unmap();
    if (renderer.m_objectConstantBuffer)
        renderer.RetiredSceneBuffers().push_back(std::move(renderer.m_objectConstantBuffer));
    if (renderer.m_objectDataBuffer)
        renderer.RetiredSceneBuffers().push_back(std::move(renderer.m_objectDataBuffer));
    if (renderer.m_portalObjectDataBuffer)
        renderer.RetiredSceneBuffers().push_back(std::move(renderer.m_portalObjectDataBuffer));
    if (renderer.m_shadowConstantBuffer)
        renderer.RetiredSceneBuffers().push_back(std::move(renderer.m_shadowConstantBuffer));
    if (renderer.m_shadowObjectDataBuffer)
        renderer.RetiredSceneBuffers().push_back(std::move(renderer.m_shadowObjectDataBuffer));
    renderer.m_objectConstantBuffer = std::move(constantBuffer);
    renderer.m_objectDataBuffer = std::move(objectBuffer);
    renderer.m_portalObjectDataBuffer = std::move(portalBuffer);
    renderer.m_shadowConstantBuffer = std::move(shadowConstantBuffer);
    renderer.m_shadowObjectDataBuffer = std::move(shadowObjectBuffer);
    renderer.m_objectCBMapped = constantMapped;
    renderer.m_objectDataMapped = objectMapped;
    renderer.m_objectUploadCache.Clear(); renderer.m_shadowUploadCache.Clear();
    renderer.m_portalObjectDataMapped = portalMapped;
    renderer.m_shadowCBMapped = shadowConstantMapped;
    renderer.m_shadowObjectDataMapped = shadowObjectMapped;
    renderer.m_objectCapacity = capacity;
    if (renderer.m_activeFrameUploadContext)
    {
        renderer.m_activeFrameUploadContext->bufferAllocations += 5;
        ++renderer.m_activeFrameUploadContext->allocatorGrowths;
    }
}

void SceneRendererResources::SelectRenderPassBuffers(SceneRenderer& renderer, uint32_t requiredObjectSlots)
{
    Scene& scene = renderer.GetScene();
    auto* contexts = renderer.m_graphicsProvider->GetContextFactory();
    const auto frameSlot = contexts->GetPreparedFrameSlot();
    const auto serial = contexts->GetPreparedFrameSerial();
    if (renderer.m_frameUploadContexts.size() <= frameSlot)
        renderer.m_frameUploadContexts.resize(size_t(frameSlot) + 1);
    if (!renderer.m_frameUploadContexts[frameSlot])
        renderer.m_frameUploadContexts[frameSlot] = std::make_unique<SceneRenderer::FrameUploadContext>();
    auto& frame = *renderer.m_frameUploadContexts[frameSlot];
    if (frame.serial != serial)
    {
        frame.serial = serial;
        frame.uploadedBytes = 0;
        frame.bufferAllocations = 0;
        frame.allocatorGrowths = 0;
    }
    renderer.m_activeFrameUploadContext = &frame;
    auto& slot = frame.renderPasses;
    if (slot.serial != serial) { slot.serial = serial; slot.cursor = 0; }
    if (slot.passes.size() <= slot.cursor) slot.passes.push_back(std::make_unique<SceneRenderer::RenderPassBuffers>());
    auto* selected = slot.passes[slot.cursor++].get();
    const auto exchange = [&](SceneRenderer::RenderPassBuffers& buffers)
    {
        using std::swap;
        swap(buffers.constants, renderer.m_objectConstantBuffer); swap(buffers.objects, renderer.m_objectDataBuffer);
        swap(buffers.shadowConstants, renderer.m_shadowConstantBuffer); swap(buffers.shadowObjects, renderer.m_shadowObjectDataBuffer);
        swap(buffers.constantsMapped, renderer.m_objectCBMapped); swap(buffers.objectsMapped, renderer.m_objectDataMapped);
        swap(buffers.shadowConstantsMapped, renderer.m_shadowCBMapped); swap(buffers.shadowObjectsMapped, renderer.m_shadowObjectDataMapped);
        swap(buffers.capacity, renderer.m_objectCapacity);
        swap(buffers.objectCache, renderer.m_objectUploadCache); swap(buffers.shadowCache, renderer.m_shadowUploadCache);
    };
    if (!renderer.m_activeRenderPassBuffers)
    {
        EnsureObjectRenderCapacity(renderer, requiredObjectSlots);
        renderer.m_activeRenderPassBuffers = selected;
        return;
    }
    const uint32_t requiredCapacity = renderer.m_objectCapacity;
    exchange(*renderer.m_activeRenderPassBuffers);
    if (!selected->objects)
    {
        selected->capacity = requiredCapacity;
        const uint64_t capacity = requiredCapacity;
        const uint64_t constants = capacity + SceneRenderer::kMaxPortalRenderViews * (capacity + 2) + SceneRenderer::kMaxSpatialDebugDraws;
        auto* factory = renderer.m_graphicsProvider->GetBufferFactory();
        using Buffer = Engine::Graphics::IGraphicsBuffer;
        selected->constants = factory->CreateBuffer(Buffer::Usage::ConstantBuffer, Buffer::AccessMode::Upload, constants * SceneRenderer::kCBStride, nullptr, sizeof(uint32_t) * 4u);
        selected->objects = factory->CreateBuffer(Buffer::Usage::ShaderResource, Buffer::AccessMode::Upload, (capacity + SceneRenderer::kMaxSpatialDebugDraws) * kObjectGPUDataSize, nullptr, kObjectGPUDataSize);
        selected->shadowConstants = factory->CreateBuffer(Buffer::Usage::ConstantBuffer, Buffer::AccessMode::Upload, capacity * 4 * SceneRenderer::kCBStride, nullptr, sizeof(uint32_t) * 4u);
        selected->shadowObjects = factory->CreateBuffer(Buffer::Usage::ShaderResource, Buffer::AccessMode::Upload, capacity * 4 * kObjectGPUDataSize, nullptr, kObjectGPUDataSize);
        selected->constantsMapped = selected->constants->Map(); selected->objectsMapped = selected->objects->Map();
        selected->shadowConstantsMapped = selected->shadowConstants->Map(); selected->shadowObjectsMapped = selected->shadowObjects->Map();
        frame.bufferAllocations += 4;
        ++frame.allocatorGrowths;
    }
    exchange(*selected); renderer.m_activeRenderPassBuffers = selected;
    EnsureObjectRenderCapacity(renderer, requiredObjectSlots);
}

void SceneRendererResources::EnsureSkinPaletteCapacity(SceneRenderer& renderer, uint32_t requiredMatrices)
{
    Scene& scene = renderer.GetScene();
    if (requiredMatrices <= renderer.m_boneMatrixCapacity)
        return;
    if (!renderer.m_graphicsProvider || !renderer.m_graphicsProvider->GetBufferFactory())
        throw std::runtime_error("Cannot grow bone palette buffer before graphics initialization");

    const uint32_t capacity = ExpandedCapacity(
        renderer.m_boneMatrixCapacity, requiredMatrices);
    auto buffer = renderer.m_graphicsProvider->GetBufferFactory()->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(capacity) * sizeof(glm::mat4), nullptr, sizeof(glm::mat4));
    void* mapped = buffer ? buffer->Map() : nullptr;
    if (!mapped)
        throw std::runtime_error("Failed to grow bone palette buffer to " +
            std::to_string(capacity) + " joint matrices");
    if (renderer.m_boneDataBuffer && renderer.m_boneDataMapped)
        renderer.m_boneDataBuffer->Unmap();
    if (renderer.m_boneDataBuffer)
        renderer.RetiredSceneBuffers().push_back(std::move(renderer.m_boneDataBuffer));
    renderer.m_boneDataBuffer = std::move(buffer);
    renderer.m_boneDataMapped = mapped;
    renderer.m_boneMatrixCapacity = capacity;
    if (renderer.m_activeFrameUploadContext)
    {
        ++renderer.m_activeFrameUploadContext->bufferAllocations;
        ++renderer.m_activeFrameUploadContext->allocatorGrowths;
    }
}
}
