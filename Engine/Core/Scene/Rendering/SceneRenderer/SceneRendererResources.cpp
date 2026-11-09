#include "Core/Scene/Rendering/SceneRenderer/SceneRendererResources.h"

#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Engine::Scene::Renderer
{
namespace
{
constexpr std::size_t kObjectGPUDataSize = 1680u;

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

void SceneRendererResources::Initialize(Scene& scene, Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider)
        throw std::runtime_error("Scene::Init requires a non-null graphics provider");

    if (scene.m_activeNativeFrameBuffers)
        scene.WaitForGpuResourceDestruction();
    scene.m_nativeFrameBuffers.clear();
    scene.m_activeNativeFrameBuffers = nullptr;

    scene.m_graphicsProvider = graphicsProvider;
    scene.m_staticRenderItemCache.clear();
    scene.QueueGraphicsPreparation();
    scene.m_particleDrawSlots.clear();

    auto* bufferFactory = scene.m_graphicsProvider->GetBufferFactory();
    if (!bufferFactory)
        throw std::runtime_error("Failed to get buffer factory from graphics provider");

    scene.m_gridConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        256);
    if (!scene.m_gridConstantBuffer)
        throw std::runtime_error("Failed to create grid constant buffer");
    scene.m_gridCBMapped = scene.m_gridConstantBuffer->Map();
    if (!scene.m_gridCBMapped)
        throw std::runtime_error("Failed to map grid constant buffer");

    scene.m_skyboxConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(Scene::kMaxPortalRenderViews + 1u) * Scene::kCBStride);
    if (!scene.m_skyboxConstantBuffer)
        throw std::runtime_error("Failed to create skybox constant buffer");
    scene.m_skyboxCBMapped = scene.m_skyboxConstantBuffer->Map();
    if (!scene.m_skyboxCBMapped)
        throw std::runtime_error("Failed to map skybox constant buffer");

    const uint64_t portalSlotsPerView =
        static_cast<uint64_t>(scene.m_objectCapacity) + 2u;
    const uint64_t portalObjectSlots =
        static_cast<uint64_t>(Scene::kMaxPortalRenderViews) * portalSlotsPerView;
    const uint64_t objectCBSize = (static_cast<uint64_t>(scene.m_objectCapacity) +
        portalObjectSlots + Scene::kMaxSpatialDebugDraws) * Scene::kCBStride;
    scene.m_objectConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        objectCBSize, nullptr, sizeof(uint32_t) * 4u);
    if (!scene.m_objectConstantBuffer)
        throw std::runtime_error("Failed to create object constant buffer");
    scene.m_objectCBMapped = scene.m_objectConstantBuffer->Map();
    if (!scene.m_objectCBMapped)
        throw std::runtime_error("Failed to map object constant buffer");

    scene.m_objectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        (static_cast<uint64_t>(scene.m_objectCapacity) + Scene::kMaxSpatialDebugDraws) *
            kObjectGPUDataSize,
        nullptr, kObjectGPUDataSize);
    scene.m_objectDataMapped = scene.m_objectDataBuffer ? scene.m_objectDataBuffer->Map() : nullptr;
    scene.m_objectUploadCache.Clear(); scene.m_shadowUploadCache.Clear();
    if (!scene.m_objectDataMapped)
        throw std::runtime_error("Failed to create object structured buffer");

    scene.m_shadowConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(scene.m_objectCapacity) * 4u * Scene::kCBStride,
        nullptr, sizeof(uint32_t) * 4u);
    scene.m_shadowCBMapped = scene.m_shadowConstantBuffer
        ? scene.m_shadowConstantBuffer->Map() : nullptr;
    scene.m_shadowObjectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(scene.m_objectCapacity) * 4u * kObjectGPUDataSize,
        nullptr, kObjectGPUDataSize);
    scene.m_shadowObjectDataMapped = scene.m_shadowObjectDataBuffer
        ? scene.m_shadowObjectDataBuffer->Map() : nullptr;
    if (!scene.m_shadowCBMapped || !scene.m_shadowObjectDataMapped)
        throw std::runtime_error("Failed to create directional shadow buffers");

    scene.m_portalObjectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        portalObjectSlots * kObjectGPUDataSize,
        nullptr, kObjectGPUDataSize);
    scene.m_portalObjectDataMapped = scene.m_portalObjectDataBuffer
        ? scene.m_portalObjectDataBuffer->Map() : nullptr;
    if (!scene.m_portalObjectDataMapped)
        throw std::runtime_error("Failed to create portal object structured buffer");

    scene.m_lightDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(Scene::kMaxLights) *
            sizeof(Engine::Model::LightData),
        nullptr, sizeof(Engine::Model::LightData));
    scene.m_lightDataMapped = scene.m_lightDataBuffer ? scene.m_lightDataBuffer->Map() : nullptr;
    if (!scene.m_lightDataMapped)
        throw std::runtime_error("Failed to create light structured buffer");

    scene.m_boneDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(scene.m_boneMatrixCapacity) *
            sizeof(glm::mat4),
        nullptr, sizeof(glm::mat4));
    scene.m_boneDataMapped = scene.m_boneDataBuffer ? scene.m_boneDataBuffer->Map() : nullptr;
    if (!scene.m_boneDataMapped)
        throw std::runtime_error("Failed to create bone palette structured buffer");

    const std::array<glm::vec4, 3> emptyMorphDelta{};
    const glm::vec4 emptyMorphWeights(0.f);
    scene.m_emptyMorphDeltaBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        sizeof(emptyMorphDelta), emptyMorphDelta.data(), sizeof(glm::vec4));
    scene.m_emptyMorphWeightBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        sizeof(emptyMorphWeights), &emptyMorphWeights, sizeof(emptyMorphWeights));
    if (!scene.m_emptyMorphDeltaBuffer || !scene.m_emptyMorphWeightBuffer)
        throw std::runtime_error("Failed to create empty morph structured buffers");

    constexpr uint32_t brdfLutResolution = 128u;
    const std::vector<float> brdfLut = GenerateBrdfIntegrationLut(
        brdfLutResolution);
    auto* textureFactory = scene.m_graphicsProvider->GetTextureFactory();
    scene.m_brdfIntegrationLut = textureFactory ? textureFactory->CreateTexture2D(
        brdfLutResolution, brdfLutResolution,
        reinterpret_cast<const uint8_t*>(brdfLut.data()), 1u,
        Engine::Graphics::GraphicsTextureFormat::Rgba32Float, false) : nullptr;
    if (!scene.m_brdfIntegrationLut)
        throw std::runtime_error("Failed to create split-sum BRDF integration LUT");

    scene.m_portalApertureBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(Scene::kMaxSpatialObjects) * Scene::kMaxSpatialVerticesPerObject *
            sizeof(Engine::Model::AnimationVertex));
    scene.m_portalApertureMapped = scene.m_portalApertureBuffer
        ? scene.m_portalApertureBuffer->Map() : nullptr;
    if (!scene.m_portalApertureMapped)
        throw std::runtime_error("Failed to create portal aperture vertex buffer");
}

void SceneRendererResources::EnsureObjectRenderCapacity(Scene& scene, uint32_t requiredObjects)
{
    if (requiredObjects <= scene.m_objectCapacity)
        return;
    if (!scene.m_graphicsProvider || !scene.m_graphicsProvider->GetBufferFactory())
        throw std::runtime_error("Cannot grow scene object buffers before graphics initialization");

    const uint32_t capacity = ExpandedCapacity(scene.m_objectCapacity, requiredObjects);
    constexpr uint32_t kMaximumAddressableCapacity =
        (std::numeric_limits<uint32_t>::max() - Scene::kMaxSpatialDebugDraws) /
        (Scene::kMaxPortalRenderViews + 1u);
    if (capacity > kMaximumAddressableCapacity)
        throw std::runtime_error("Scene draw count exceeds portal buffer index range");
    const uint64_t portalSlotsPerView = static_cast<uint64_t>(capacity) + 2u;
    const uint64_t portalSlots =
        static_cast<uint64_t>(Scene::kMaxPortalRenderViews) * portalSlotsPerView;
    const uint64_t constantSlots = static_cast<uint64_t>(capacity) +
        portalSlots + Scene::kMaxSpatialDebugDraws;
    auto* factory = scene.m_graphicsProvider->GetBufferFactory();
    auto constantBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        constantSlots * Scene::kCBStride, nullptr, sizeof(uint32_t) * 4u);
    auto objectBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        (static_cast<uint64_t>(capacity) + Scene::kMaxSpatialDebugDraws) *
            kObjectGPUDataSize, nullptr, kObjectGPUDataSize);
    auto portalBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        portalSlots * kObjectGPUDataSize, nullptr, kObjectGPUDataSize);
    auto shadowConstantBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(capacity) * 4u * Scene::kCBStride,
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

    if (scene.m_objectConstantBuffer && scene.m_objectCBMapped)
        scene.m_objectConstantBuffer->Unmap();
    if (scene.m_objectDataBuffer && scene.m_objectDataMapped)
        scene.m_objectDataBuffer->Unmap();
    if (scene.m_portalObjectDataBuffer && scene.m_portalObjectDataMapped)
        scene.m_portalObjectDataBuffer->Unmap();
    if (scene.m_shadowConstantBuffer && scene.m_shadowCBMapped)
        scene.m_shadowConstantBuffer->Unmap();
    if (scene.m_shadowObjectDataBuffer && scene.m_shadowObjectDataMapped)
        scene.m_shadowObjectDataBuffer->Unmap();
    if (scene.m_objectConstantBuffer)
        scene.m_retiredRenderBuffers.push_back(std::move(scene.m_objectConstantBuffer));
    if (scene.m_objectDataBuffer)
        scene.m_retiredRenderBuffers.push_back(std::move(scene.m_objectDataBuffer));
    if (scene.m_portalObjectDataBuffer)
        scene.m_retiredRenderBuffers.push_back(std::move(scene.m_portalObjectDataBuffer));
    if (scene.m_shadowConstantBuffer)
        scene.m_retiredRenderBuffers.push_back(std::move(scene.m_shadowConstantBuffer));
    if (scene.m_shadowObjectDataBuffer)
        scene.m_retiredRenderBuffers.push_back(std::move(scene.m_shadowObjectDataBuffer));
    scene.m_objectConstantBuffer = std::move(constantBuffer);
    scene.m_objectDataBuffer = std::move(objectBuffer);
    scene.m_portalObjectDataBuffer = std::move(portalBuffer);
    scene.m_shadowConstantBuffer = std::move(shadowConstantBuffer);
    scene.m_shadowObjectDataBuffer = std::move(shadowObjectBuffer);
    scene.m_objectCBMapped = constantMapped;
    scene.m_objectDataMapped = objectMapped;
    scene.m_objectUploadCache.Clear(); scene.m_shadowUploadCache.Clear();
    scene.m_portalObjectDataMapped = portalMapped;
    scene.m_shadowCBMapped = shadowConstantMapped;
    scene.m_shadowObjectDataMapped = shadowObjectMapped;
    scene.m_objectCapacity = capacity;
}

void SceneRendererResources::SelectRenderPassBuffers(Scene& scene, uint32_t requiredObjectSlots)
{
    auto* contexts = scene.m_graphicsProvider->GetContextFactory();
    const auto frameSlot = contexts->GetPreparedFrameSlot();
    const auto serial = contexts->GetPreparedFrameSerial();
    if (scene.m_renderPassBufferSlots.size() <= frameSlot)
        scene.m_renderPassBufferSlots.resize(size_t(frameSlot) + 1);
    auto& slot = scene.m_renderPassBufferSlots[frameSlot];
    if (slot.serial != serial) { slot.serial = serial; slot.cursor = 0; }
    if (slot.passes.size() <= slot.cursor) slot.passes.push_back(std::make_unique<Scene::RenderPassBuffers>());
    auto* selected = slot.passes[slot.cursor++].get();
    const auto exchange = [&](Scene::RenderPassBuffers& buffers)
    {
        using std::swap;
        swap(buffers.constants, scene.m_objectConstantBuffer); swap(buffers.objects, scene.m_objectDataBuffer);
        swap(buffers.shadowConstants, scene.m_shadowConstantBuffer); swap(buffers.shadowObjects, scene.m_shadowObjectDataBuffer);
        swap(buffers.constantsMapped, scene.m_objectCBMapped); swap(buffers.objectsMapped, scene.m_objectDataMapped);
        swap(buffers.shadowConstantsMapped, scene.m_shadowCBMapped); swap(buffers.shadowObjectsMapped, scene.m_shadowObjectDataMapped);
        swap(buffers.capacity, scene.m_objectCapacity);
        swap(buffers.objectCache, scene.m_objectUploadCache); swap(buffers.shadowCache, scene.m_shadowUploadCache);
    };
    if (!scene.m_activeRenderPassBuffers)
    {
        EnsureObjectRenderCapacity(scene, requiredObjectSlots);
        scene.m_activeRenderPassBuffers = selected;
        return;
    }
    const uint32_t requiredCapacity = scene.m_objectCapacity;
    exchange(*scene.m_activeRenderPassBuffers);
    if (!selected->objects)
    {
        selected->capacity = requiredCapacity;
        const uint64_t capacity = requiredCapacity;
        const uint64_t constants = capacity + Scene::kMaxPortalRenderViews * (capacity + 2) + Scene::kMaxSpatialDebugDraws;
        auto* factory = scene.m_graphicsProvider->GetBufferFactory();
        using Buffer = Engine::Graphics::IGraphicsBuffer;
        selected->constants = factory->CreateBuffer(Buffer::Usage::ConstantBuffer, Buffer::AccessMode::Upload, constants * Scene::kCBStride, nullptr, sizeof(uint32_t) * 4u);
        selected->objects = factory->CreateBuffer(Buffer::Usage::ShaderResource, Buffer::AccessMode::Upload, (capacity + Scene::kMaxSpatialDebugDraws) * kObjectGPUDataSize, nullptr, kObjectGPUDataSize);
        selected->shadowConstants = factory->CreateBuffer(Buffer::Usage::ConstantBuffer, Buffer::AccessMode::Upload, capacity * 4 * Scene::kCBStride, nullptr, sizeof(uint32_t) * 4u);
        selected->shadowObjects = factory->CreateBuffer(Buffer::Usage::ShaderResource, Buffer::AccessMode::Upload, capacity * 4 * kObjectGPUDataSize, nullptr, kObjectGPUDataSize);
        selected->constantsMapped = selected->constants->Map(); selected->objectsMapped = selected->objects->Map();
        selected->shadowConstantsMapped = selected->shadowConstants->Map(); selected->shadowObjectsMapped = selected->shadowObjects->Map();
    }
    exchange(*selected); scene.m_activeRenderPassBuffers = selected;
    EnsureObjectRenderCapacity(scene, requiredObjectSlots);
}

void SceneRendererResources::EnsureSkinPaletteCapacity(Scene& scene, uint32_t requiredMatrices)
{
    if (requiredMatrices <= scene.m_boneMatrixCapacity)
        return;
    if (!scene.m_graphicsProvider || !scene.m_graphicsProvider->GetBufferFactory())
        throw std::runtime_error("Cannot grow bone palette buffer before graphics initialization");

    const uint32_t capacity = ExpandedCapacity(
        scene.m_boneMatrixCapacity, requiredMatrices);
    auto buffer = scene.m_graphicsProvider->GetBufferFactory()->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(capacity) * sizeof(glm::mat4), nullptr, sizeof(glm::mat4));
    void* mapped = buffer ? buffer->Map() : nullptr;
    if (!mapped)
        throw std::runtime_error("Failed to grow bone palette buffer to " +
            std::to_string(capacity) + " joint matrices");
    if (scene.m_boneDataBuffer && scene.m_boneDataMapped)
        scene.m_boneDataBuffer->Unmap();
    if (scene.m_boneDataBuffer)
        scene.m_retiredRenderBuffers.push_back(std::move(scene.m_boneDataBuffer));
    scene.m_boneDataBuffer = std::move(buffer);
    scene.m_boneDataMapped = mapped;
    scene.m_boneMatrixCapacity = capacity;
}
}
