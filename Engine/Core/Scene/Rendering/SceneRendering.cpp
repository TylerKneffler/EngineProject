#include "Core/Rendering/ShadowReceiverCulling.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Scene/Scene.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include <chrono>
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/TerrainObjectLayer.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Obj/TerrainBatchAppearance.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include "Core/Model/LightingData.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IShader.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/PostProcess.h"
#include "Core/Renderers/UIRenderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#if defined(_WIN32)
#include "Core/Renderers/DX11/DX11GraphicsProvider.h"
#include "Core/Renderers/DX12/DX12GraphicsProvider.h"
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#endif

#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Renderers/Vulkan/VulkanGraphicsProvider.h"
#endif

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

namespace Engine::Scene
{
void Scene::CaptureEmbeddedUiInput(Engine::Core::NativeInputSnapshot& input) const
{
    AssertOwnerThread();
    if (m_uiRenderer) m_uiRenderer->CaptureEmbeddedPointer(input);
}

void Scene::SetUiPointerInput(float x, float y, float viewportWidth,
    float viewportHeight, bool hovered, bool mouseDown)
{
    if (m_uiRenderer)
        m_uiRenderer->SetPointerInput(x, y, viewportWidth, viewportHeight,
            hovered, mouseDown);
}


#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace
{
    uint32_t ExpandedCapacity(uint32_t current, uint32_t required)
    {
        uint32_t capacity = std::max(1u, current);
        while (capacity < required)
        {
            if (capacity > std::numeric_limits<uint32_t>::max() / 2u)
                return required;
            capacity *= 2u;
        }
        return capacity;
    }

    std::string EngineShaderPath(const char* fileName)
    {
        const std::filesystem::path shaderFile(fileName);
        const std::filesystem::path relativePath =
            shaderFile.stem() / shaderFile;
        const std::filesystem::path bundled =
            std::filesystem::path("Engine") / "Shaders" / relativePath;
        if (std::filesystem::is_regular_file(bundled))
            return bundled.string();
        return (std::filesystem::path(ENGINE_SHADERS_PATH) / relativePath).string();
    }

    bool IsFastFoliageSurface(const Engine::Components::Material* material)
    {
        if (!material || material->baseColorUvSet != 0 ||
            material->GetAlphaMode() == Engine::Components::MaterialAlphaMode::Blend)
            return false;
        const Engine::Components::MaterialShader* surface = nullptr;
        for (const auto& shader : material->shaders)
        {
            if (!shader.enabled) continue;
            if (surface) return false;
            surface = &shader;
        }
        return surface && surface->path == "Assets/Shaders/Foliage.hlsl" &&
            (surface->blendMode == "Auto" || surface->blendMode == "Opaque");
    }

    bool IsObjectOrDescendant(const Engine::Core::Object* object, const Engine::Core::Object* root)
    {
        for (const Engine::Core::Object* current = object; current; current = current->Parent)
            if (current == root)
                return true;
        return false;
    }

    void HashRevision(uint64_t& hash, uint64_t value)
    {
        constexpr uint64_t prime = 1099511628211ull;
        for (unsigned byte = 0; byte < sizeof(value); ++byte)
        {
            hash ^= (value >> (byte * 8u)) & 0xffu;
            hash *= prime;
        }
    }

    float RadicalInverseVdc(uint32_t bits)
    {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) |
            ((bits & 0xaaaaaaaau) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) |
            ((bits & 0xccccccccu) >> 2u);
        bits = ((bits & 0x0f0f0f0fu) << 4u) |
            ((bits & 0xf0f0f0f0u) >> 4u);
        bits = ((bits & 0x00ff00ffu) << 8u) |
            ((bits & 0xff00ff00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }

    glm::vec2 IntegrateBrdf(float noV, float roughness)
    {
        constexpr uint32_t sampleCount = 256u;
        constexpr float twoPi = 6.28318530717958647692f;
        const glm::vec3 normal(0.f, 0.f, 1.f);
        const glm::vec3 view(std::sqrt(std::max(0.f, 1.f - noV * noV)),
            0.f, noV);
        float scale = 0.f;
        float bias = 0.f;
        const float alpha = roughness * roughness;
        const float alphaSquared = alpha * alpha;
        const float geometryK = alpha * 0.5f;
        for (uint32_t sample = 0; sample < sampleCount; ++sample)
        {
            const float xiX = static_cast<float>(sample) /
                static_cast<float>(sampleCount);
            const float xiY = RadicalInverseVdc(sample);
            const float phi = twoPi * xiX;
            const float cosTheta = std::sqrt(std::max(0.f,
                (1.f - xiY) /
                (1.f + (alphaSquared - 1.f) * xiY)));
            const float sinTheta = std::sqrt(std::max(0.f,
                1.f - cosTheta * cosTheta));
            const glm::vec3 halfVector(
                std::cos(phi) * sinTheta,
                std::sin(phi) * sinTheta,
                cosTheta);
            const glm::vec3 light = glm::normalize(
                2.f * glm::dot(view, halfVector) * halfVector - view);
            const float noL = std::max(light.z, 0.f);
            const float noH = std::max(halfVector.z, 0.f);
            const float voH = std::max(glm::dot(view, halfVector), 0.f);
            if (noL <= 0.f || noH <= 0.f || voH <= 0.f)
                continue;
            const float gv = noV /
                (noV * (1.f - geometryK) + geometryK);
            const float gl = noL /
                (noL * (1.f - geometryK) + geometryK);
            const float visibility = gv * gl * voH /
                std::max(noH * noV, 0.00001f);
            const float fresnel = std::pow(1.f - voH, 5.f);
            scale += (1.f - fresnel) * visibility;
            bias += fresnel * visibility;
        }
        return glm::vec2(scale, bias) / static_cast<float>(sampleCount);
    }

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

// ---------------------------------------------------------------------------
// Constant buffer data structures
// ---------------------------------------------------------------------------

// Per-draw constants stay deliberately small. Vulkan sends these as push
// constants; DirectX binds them through its native constant-buffer path.
struct DrawCBData
{
    uint32_t objectIndex;
    uint32_t lightCount;
    uint32_t flags;
    uint32_t padding;
};

constexpr uint32_t kPortalApertureNearClampFlag = 0x40000000u;
constexpr uint32_t kPortalDepthResetFlag = 0x80000000u;
constexpr uint32_t kClusteredLightingFlag = 0x00000001u;

// Large, indexed records live in shader-readable buffers on every backend.
struct ObjectGPUData
{
    // Kept separate from world so adjoining meshes transform a shared world
    // position through exactly the same view-projection arithmetic. Baking
    // world into this matrix per object can expose sub-pixel cracks between
    // otherwise identical terrain borders.
    glm::mat4 viewProjection;
    glm::mat4 sphereView; // View rotation for PMX normal-based sphere coordinates.
    glm::mat4 world;
    glm::vec4 baseColor;
    glm::vec4 ambientUnlit;
    glm::vec4 emissiveOcclusion;
    glm::vec4 materialParams; // metallic, roughness, normal scale, texture mask
    glm::vec4 viewPositionAlphaCutoff;
    glm::vec4 bakedDirectional;
    glm::vec4 bakedLightDirection;
    glm::vec4 parallaxParams; // scale, minimum steps, maximum steps, reserved
    glm::vec4 spriteUvRect; // offset.xy, scale.xy
    glm::vec4 textureUvSets0;
    glm::vec4 textureUvSets1;
    glm::vec4 skinParams; // palette offset, joint count, reserved, reserved
    glm::vec4 morphParams; // target count, vertex count, reserved, reserved
    glm::vec4 environmentParams; // intensity, rotation radians, diffuse, reflections
    glm::vec4 environmentSH[9]; // RGB radiance coefficients
    glm::vec4 reflectionEnvironmentParams; // exposure scale, rotation, custom enabled, reserved
    glm::vec4 reflectionEnvironmentSH[9];
    glm::mat4 shadowViewProjections[4];
    // depth bias, normal/slope bias, strength, PCF radius
    glm::vec4 shadowParams;
    // View-distance end for each directional cascade.
    glm::vec4 shadowCascadeSplits;
    // cascade count, atlas tile scale, boundary blend fraction, max distance
    glm::vec4 shadowCascadeData;
    // Camera forward direction and near distance used for cascade selection.
    glm::vec4 shadowCameraData;
    // tile size, tiles X, tiles Y, depth slices
    glm::vec4 lightingClusterParams;
    // camera forward, logarithmic depth scale
    glm::vec4 lightingCameraData;
    // Enabled per portal draw through DrawCBData::flags; dot(world, plane)
    // selects the connected half-space at the target aperture.
    glm::vec4 portalClipPlane;
    // Independent from portal-view clipping: active traversal bodies render
    // their local and remote chart instances from one untouched mesh buffer.
    glm::vec4 traversalClipPlane;
    glm::uvec4 lightingChannels{ 0xffffffffu, 0u, 0u, 0u };
    glm::vec4 subsurfaceColorWeight{ 0.f }; // RGB color, enabled weight
    glm::vec4 subsurfaceRadiusThickness{ 0.f }; // RGB scene units, thickness in scene units
    glm::vec4 subsurfaceTransmission{ 0.f }; // x = transmitted-light weight
    glm::vec4 blinnPhongSpecularShininess{ 0.f }; // RGB reflectance; w = shininess + 1 when enabled
    glm::vec4 pmxTextureTint { 1.f };
    glm::vec4 pmxSphereTint { 1.f };
    glm::vec4 pmxToonTint { 1.f };
    glm::vec4 pmxEdgeColor { 0.f };
    glm::vec4 pmxParams { 0.f }; // edge width, sphere mode, toon source, edge enabled
    glm::uvec4 terrainMapMasks{ 0u }; // normal, roughness, height, reserved
    glm::vec4 terrainLayerParams[6]{}; // roughness, normal scale, height blend, reserved
    glm::vec4 terrainMacroColors[6]{}; // measured linear albedo averages
    glm::vec4 geometryFade{0.f, 1.f, 0.f, 0.f}; // dither interval, reserved, enabled
    glm::vec4 shadowCasterFade{0.f}; // range, fade width, shadow pass enabled, reserved
    glm::vec4 grassAnimation{0.f}; // scene time, grass enabled, reserved
    glm::vec4 textureLodParams{0.f}; // near, far, maximum mip bias, enabled/shading tier
    glm::vec4 alphaLodParams{0.f};
};

// Value-only boundary: resources are resolved by the scene owner before assembly.
// Texture pointers are binding identities; this function never dereferences them.
struct ResolvedMaterialRecord
{
    ObjectGPUData data{};
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> textures{};
    bool doubleSided = false;
    Engine::Components::MaterialAlphaMode alphaMode{};
};

void ApplyResolvedMaterialRecord(const ResolvedMaterialRecord& material,
    const glm::vec3& cameraPosition, const glm::vec3& irradiance,
    ObjectGPUData& objectData)
{
    const auto& cachedData = material.data;
    objectData.ambientUnlit=cachedData.ambientUnlit;
    objectData.baseColor=cachedData.baseColor;
    objectData.blinnPhongSpecularShininess=cachedData.blinnPhongSpecularShininess;
    objectData.emissiveOcclusion=cachedData.emissiveOcclusion;
    objectData.environmentParams=cachedData.environmentParams;
    objectData.materialParams=cachedData.materialParams;
    objectData.parallaxParams=cachedData.parallaxParams;
    objectData.pmxEdgeColor=cachedData.pmxEdgeColor;
    objectData.pmxParams=cachedData.pmxParams;
    objectData.pmxSphereTint=cachedData.pmxSphereTint;
    objectData.pmxTextureTint=cachedData.pmxTextureTint;
    objectData.pmxToonTint=cachedData.pmxToonTint;
    objectData.reflectionEnvironmentParams=cachedData.reflectionEnvironmentParams;
    std::copy(std::begin(cachedData.reflectionEnvironmentSH),std::end(cachedData.reflectionEnvironmentSH),objectData.reflectionEnvironmentSH);
    objectData.subsurfaceColorWeight=cachedData.subsurfaceColorWeight;
    objectData.subsurfaceRadiusThickness=cachedData.subsurfaceRadiusThickness;
    objectData.subsurfaceTransmission=cachedData.subsurfaceTransmission;
    objectData.textureUvSets0=cachedData.textureUvSets0;
    objectData.textureUvSets1=cachedData.textureUvSets1;
    objectData.viewPositionAlphaCutoff=cachedData.viewPositionAlphaCutoff;
    objectData.ambientUnlit += glm::vec4(irradiance, 0.f);
    objectData.viewPositionAlphaCutoff = glm::vec4(cameraPosition,
        cachedData.viewPositionAlphaCutoff.w);
}

// Per-object values are copied after probe sampling and component/resource reads.
// Assembly can consume this snapshot without reaching back into the scene.
struct ResolvedObjectRecord
{
    ObjectGPUData data{};
    glm::vec4 directional{0.f};
    glm::vec4 direction{0.f};
    bool preview = false;
    bool forceUnlit = false;
};

ObjectGPUData AssembleObjectRecord(const ResolvedObjectRecord& input)
{
    ObjectGPUData result = input.data;
    result.bakedDirectional = input.directional;
    result.bakedLightDirection = input.direction;
    if (input.preview) result.baseColor.a *= 0.45f;
    if (input.forceUnlit) result.ambientUnlit.w = 1.f;
    return result;
}

// Constant buffer for grid rendering
struct GridCBData
{
    glm::mat4 invVP;
    glm::vec3 cameraPos;
    float cellSize;
    glm::vec4 gridColor;
    glm::vec4 axisColor;
    float fadeDistance;
    float nearPlane;
    float farPlane;
    float mode2D;
};

struct SkyboxCBData
{
    glm::mat4 invVP;
    glm::vec4 displayParams; // x: fullscreen 2D background mode
};

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

static_assert(sizeof(DrawCBData) == 16, "Draw constants must remain small");
static_assert(sizeof(ObjectGPUData) == 1552, "Object buffer layout must match Object.hlsl");
static_assert(sizeof(Engine::Model::LightData) == 128,
    "Light buffer layout must match Object.hlsl");
static_assert(sizeof(GridCBData) == 128, "Grid constant-buffer layout must match Grid.hlsl");
static_assert(sizeof(SkyboxCBData) == 80, "Skybox constant-buffer layout must match Skybox.hlsl");
static_assert(sizeof(ParticleGPUData) == 48, "Particle vertex layout must match Particle.hlsl");
static_assert(sizeof(FluidSurfaceParticleData) == 64, "Fluid surface buffer must match Particle.hlsl");
static_assert(sizeof(ParticleCBData) == 128, "Particle constants must fit Vulkan push constants");

// ---------------------------------------------------------------------------
// Scene::Init
// ---------------------------------------------------------------------------

void Scene::Init(Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider)
        throw std::runtime_error("Scene::Init requires a non-null graphics provider");

    if (m_activeNativeFrameBuffers)
        WaitForGpuResourceDestruction();
    m_nativeFrameBuffers.clear();
    m_activeNativeFrameBuffers = nullptr;

    m_graphicsProvider = graphicsProvider;
    m_staticRenderItemCache.clear();
    QueueGraphicsPreparation();
    m_particleDrawSlots.clear();

    // Create constant buffers via the graphics buffer factory
    auto* bufferFactory = m_graphicsProvider->GetBufferFactory();
    if (!bufferFactory)
        throw std::runtime_error("Failed to get buffer factory from graphics provider");

    // Grid constant buffer (256 bytes, uploadable)
    m_gridConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        256);
    if (!m_gridConstantBuffer)
        throw std::runtime_error("Failed to create grid constant buffer");
    m_gridCBMapped = m_gridConstantBuffer->Map();
    if (!m_gridCBMapped)
        throw std::runtime_error("Failed to map grid constant buffer");

    m_skyboxConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(kMaxPortalRenderViews + 1u) * kCBStride);
    if (!m_skyboxConstantBuffer)
        throw std::runtime_error("Failed to create skybox constant buffer");
    m_skyboxCBMapped = m_skyboxConstantBuffer->Map();
    if (!m_skyboxCBMapped)
        throw std::runtime_error("Failed to map skybox constant buffer");

    // Ordinary draws occupy the first m_objectCapacity entries. Portal views use
    // stable per-view ranges so deferred command buffers never observe data
    // overwritten by a later recursive pass.
    const uint64_t portalSlotsPerView =
        static_cast<uint64_t>(m_objectCapacity) + 2u;
    const uint64_t portalObjectSlots =
        static_cast<uint64_t>(kMaxPortalRenderViews) * portalSlotsPerView;
    const uint64_t objectCBSize = (static_cast<uint64_t>(m_objectCapacity) +
        portalObjectSlots + kMaxSpatialDebugDraws) * kCBStride;
    m_objectConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        objectCBSize, nullptr, sizeof(DrawCBData));
    if (!m_objectConstantBuffer)
        throw std::runtime_error("Failed to create object constant buffer");
    m_objectCBMapped = m_objectConstantBuffer->Map();
    if (!m_objectCBMapped)
        throw std::runtime_error("Failed to map object constant buffer");

    m_objectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        (static_cast<uint64_t>(m_objectCapacity) + kMaxSpatialDebugDraws) *
            sizeof(ObjectGPUData),
        nullptr, sizeof(ObjectGPUData));
    m_objectDataMapped = m_objectDataBuffer ? m_objectDataBuffer->Map() : nullptr;
    if (!m_objectDataMapped)
        throw std::runtime_error("Failed to create object structured buffer");

    m_shadowConstantBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(m_objectCapacity) * 4u * kCBStride,
        nullptr, sizeof(DrawCBData));
    m_shadowCBMapped = m_shadowConstantBuffer
        ? m_shadowConstantBuffer->Map() : nullptr;
    m_shadowObjectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(m_objectCapacity) * 4u * sizeof(ObjectGPUData),
        nullptr, sizeof(ObjectGPUData));
    m_shadowObjectDataMapped = m_shadowObjectDataBuffer
        ? m_shadowObjectDataBuffer->Map() : nullptr;
    if (!m_shadowCBMapped || !m_shadowObjectDataMapped)
        throw std::runtime_error("Failed to create directional shadow buffers");

    // Portal recursion has a much larger worst-case record count than an
    // ordinary view. Keep it separate so terrain-only frames never upload or
    // invalidate the reserved portal address space.
    m_portalObjectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        portalObjectSlots * sizeof(ObjectGPUData),
        nullptr, sizeof(ObjectGPUData));
    m_portalObjectDataMapped = m_portalObjectDataBuffer
        ? m_portalObjectDataBuffer->Map() : nullptr;
    if (!m_portalObjectDataMapped)
        throw std::runtime_error("Failed to create portal object structured buffer");

    m_lightDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(kMaxLights) *
            sizeof(Engine::Model::LightData),
        nullptr, sizeof(Engine::Model::LightData));
    m_lightDataMapped = m_lightDataBuffer ? m_lightDataBuffer->Map() : nullptr;
    if (!m_lightDataMapped)
        throw std::runtime_error("Failed to create light structured buffer");

    m_boneDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(m_boneMatrixCapacity) *
            sizeof(glm::mat4),
        nullptr, sizeof(glm::mat4));
    m_boneDataMapped = m_boneDataBuffer ? m_boneDataBuffer->Map() : nullptr;
    if (!m_boneDataMapped)
        throw std::runtime_error("Failed to create bone palette structured buffer");

    const std::array<glm::vec4, 3> emptyMorphDelta{};
    const glm::vec4 emptyMorphWeights(0.f);
    m_emptyMorphDeltaBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        sizeof(emptyMorphDelta), emptyMorphDelta.data(), sizeof(glm::vec4));
    m_emptyMorphWeightBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        sizeof(emptyMorphWeights), &emptyMorphWeights, sizeof(emptyMorphWeights));
    if (!m_emptyMorphDeltaBuffer || !m_emptyMorphWeightBuffer)
        throw std::runtime_error("Failed to create empty morph structured buffers");

    constexpr uint32_t brdfLutResolution = 128u;
    const std::vector<float> brdfLut = GenerateBrdfIntegrationLut(
        brdfLutResolution);
    auto* textureFactory = m_graphicsProvider->GetTextureFactory();
    m_brdfIntegrationLut = textureFactory ? textureFactory->CreateTexture2D(
        brdfLutResolution, brdfLutResolution,
        reinterpret_cast<const uint8_t*>(brdfLut.data()), 1u,
        Engine::Graphics::GraphicsTextureFormat::Rgba32Float, false) : nullptr;
    if (!m_brdfIntegrationLut)
        throw std::runtime_error("Failed to create split-sum BRDF integration LUT");

    m_portalApertureBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(kMaxSpatialObjects) * kMaxSpatialVerticesPerObject *
            sizeof(Engine::Model::AnimationVertex));
    m_portalApertureMapped = m_portalApertureBuffer
        ? m_portalApertureBuffer->Map() : nullptr;
    if (!m_portalApertureMapped)
        throw std::runtime_error("Failed to create portal aperture vertex buffer");

    // Set up the default editor camera
    Engine::Components::Camera* editorCameraComponent = editorCamera.AddComponent<Engine::Components::Camera>();
    editorCameraComponent->useTransformRotation = false;
    editorCameraComponent->farPlane = 1000.f;
    SetEditorMode2D(
        settings.dimension == Engine::Model::SceneDimension::TwoD);

    // Build pipeline states
    BuildGridPipeline();
    BuildParticlePipeline();
    BuildSkyboxPipeline();
    BuildObjectPipeline();
    m_uiRenderer = std::make_unique<Engine::Renderers::UIRenderer>();
    m_uiRenderer->Initialize(m_graphicsProvider);
}

void Scene::EnsureObjectRenderCapacity(uint32_t requiredObjects)
{
    if (requiredObjects <= m_objectCapacity)
        return;
    if (!m_graphicsProvider || !m_graphicsProvider->GetBufferFactory())
        throw std::runtime_error("Cannot grow scene object buffers before graphics initialization");

    const uint32_t capacity = ExpandedCapacity(m_objectCapacity, requiredObjects);
    constexpr uint32_t kMaximumAddressableCapacity =
        (std::numeric_limits<uint32_t>::max() - kMaxSpatialDebugDraws) /
        (kMaxPortalRenderViews + 1u);
    if (capacity > kMaximumAddressableCapacity)
        throw std::runtime_error("Scene draw count exceeds portal buffer index range");
    const uint64_t portalSlotsPerView = static_cast<uint64_t>(capacity) + 2u;
    const uint64_t portalSlots =
        static_cast<uint64_t>(kMaxPortalRenderViews) * portalSlotsPerView;
    const uint64_t constantSlots = static_cast<uint64_t>(capacity) +
        portalSlots + kMaxSpatialDebugDraws;
    auto* factory = m_graphicsProvider->GetBufferFactory();
    auto constantBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        constantSlots * kCBStride, nullptr, sizeof(DrawCBData));
    auto objectBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        (static_cast<uint64_t>(capacity) + kMaxSpatialDebugDraws) *
            sizeof(ObjectGPUData), nullptr, sizeof(ObjectGPUData));
    auto portalBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        portalSlots * sizeof(ObjectGPUData), nullptr, sizeof(ObjectGPUData));
    auto shadowConstantBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(capacity) * 4u * kCBStride,
        nullptr, sizeof(DrawCBData));
    auto shadowObjectBuffer = factory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(capacity) * 4u * sizeof(ObjectGPUData),
        nullptr, sizeof(ObjectGPUData));
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

    if (m_objectConstantBuffer && m_objectCBMapped)
        m_objectConstantBuffer->Unmap();
    if (m_objectDataBuffer && m_objectDataMapped)
        m_objectDataBuffer->Unmap();
    if (m_portalObjectDataBuffer && m_portalObjectDataMapped)
        m_portalObjectDataBuffer->Unmap();
    if (m_shadowConstantBuffer && m_shadowCBMapped)
        m_shadowConstantBuffer->Unmap();
    if (m_shadowObjectDataBuffer && m_shadowObjectDataMapped)
        m_shadowObjectDataBuffer->Unmap();
    if (m_objectConstantBuffer)
        m_retiredRenderBuffers.push_back(std::move(m_objectConstantBuffer));
    if (m_objectDataBuffer)
        m_retiredRenderBuffers.push_back(std::move(m_objectDataBuffer));
    if (m_portalObjectDataBuffer)
        m_retiredRenderBuffers.push_back(std::move(m_portalObjectDataBuffer));
    if (m_shadowConstantBuffer)
        m_retiredRenderBuffers.push_back(std::move(m_shadowConstantBuffer));
    if (m_shadowObjectDataBuffer)
        m_retiredRenderBuffers.push_back(std::move(m_shadowObjectDataBuffer));
    m_objectConstantBuffer = std::move(constantBuffer);
    m_objectDataBuffer = std::move(objectBuffer);
    m_portalObjectDataBuffer = std::move(portalBuffer);
    m_shadowConstantBuffer = std::move(shadowConstantBuffer);
    m_shadowObjectDataBuffer = std::move(shadowObjectBuffer);
    m_objectCBMapped = constantMapped;
    m_objectDataMapped = objectMapped;
    m_portalObjectDataMapped = portalMapped;
    m_shadowCBMapped = shadowConstantMapped;
    m_shadowObjectDataMapped = shadowObjectMapped;
    m_objectCapacity = capacity;
}

void Scene::SelectRenderPassBuffers(uint32_t requiredObjectSlots)
{
    auto* contexts=m_graphicsProvider->GetContextFactory();
    const auto frameSlot=contexts->GetPreparedFrameSlot();
    const auto serial=contexts->GetPreparedFrameSerial();
    if(m_renderPassBufferSlots.size()<=frameSlot) m_renderPassBufferSlots.resize(size_t(frameSlot)+1);
    auto& slot=m_renderPassBufferSlots[frameSlot];
    if(slot.serial!=serial) { slot.serial=serial; slot.cursor=0; }
    if(slot.passes.size()<=slot.cursor) slot.passes.push_back(std::make_unique<RenderPassBuffers>());
    auto* selected=slot.passes[slot.cursor++].get();
    const auto exchange=[&](RenderPassBuffers& buffers) {
        using std::swap;
        swap(buffers.constants,m_objectConstantBuffer); swap(buffers.objects,m_objectDataBuffer);
        swap(buffers.shadowConstants,m_shadowConstantBuffer); swap(buffers.shadowObjects,m_shadowObjectDataBuffer);
        swap(buffers.constantsMapped,m_objectCBMapped); swap(buffers.objectsMapped,m_objectDataMapped);
        swap(buffers.shadowConstantsMapped,m_shadowCBMapped); swap(buffers.shadowObjectsMapped,m_shadowObjectDataMapped);
        swap(buffers.capacity,m_objectCapacity);
    };
    if(!m_activeRenderPassBuffers)
    {
        EnsureObjectRenderCapacity(requiredObjectSlots);
        m_activeRenderPassBuffers=selected;
        return;
    }
    const uint32_t requiredCapacity=m_objectCapacity;
    exchange(*m_activeRenderPassBuffers);
    if(!selected->objects)
    {
        selected->capacity=requiredCapacity;
        const uint64_t capacity=requiredCapacity;
        const uint64_t constants=capacity+kMaxPortalRenderViews*(capacity+2)+kMaxSpatialDebugDraws;
        auto* factory=m_graphicsProvider->GetBufferFactory();
        using Buffer=Engine::Graphics::IGraphicsBuffer;
        selected->constants=factory->CreateBuffer(Buffer::Usage::ConstantBuffer,Buffer::AccessMode::Upload,constants*kCBStride,nullptr,sizeof(DrawCBData));
        selected->objects=factory->CreateBuffer(Buffer::Usage::ShaderResource,Buffer::AccessMode::Upload,(capacity+kMaxSpatialDebugDraws)*sizeof(ObjectGPUData),nullptr,sizeof(ObjectGPUData));
        selected->shadowConstants=factory->CreateBuffer(Buffer::Usage::ConstantBuffer,Buffer::AccessMode::Upload,capacity*4*kCBStride,nullptr,sizeof(DrawCBData));
        selected->shadowObjects=factory->CreateBuffer(Buffer::Usage::ShaderResource,Buffer::AccessMode::Upload,capacity*4*sizeof(ObjectGPUData),nullptr,sizeof(ObjectGPUData));
        selected->constantsMapped=selected->constants->Map(); selected->objectsMapped=selected->objects->Map();
        selected->shadowConstantsMapped=selected->shadowConstants->Map(); selected->shadowObjectsMapped=selected->shadowObjects->Map();
    }
    exchange(*selected); m_activeRenderPassBuffers=selected;
    EnsureObjectRenderCapacity(requiredObjectSlots);
}

void Scene::EnsureSkinPaletteCapacity(uint32_t requiredMatrices)
{
    if (requiredMatrices <= m_boneMatrixCapacity)
        return;
    if (!m_graphicsProvider || !m_graphicsProvider->GetBufferFactory())
        throw std::runtime_error("Cannot grow bone palette buffer before graphics initialization");

    const uint32_t capacity = ExpandedCapacity(
        m_boneMatrixCapacity, requiredMatrices);
    auto buffer = m_graphicsProvider->GetBufferFactory()->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(capacity) *
            sizeof(glm::mat4), nullptr, sizeof(glm::mat4));
    void* mapped = buffer ? buffer->Map() : nullptr;
    if (!mapped)
        throw std::runtime_error("Failed to grow bone palette buffer to " +
            std::to_string(capacity) + " joint matrices");
    if (m_boneDataBuffer && m_boneDataMapped)
        m_boneDataBuffer->Unmap();
    if (m_boneDataBuffer)
        m_retiredRenderBuffers.push_back(std::move(m_boneDataBuffer));
    m_boneDataBuffer = std::move(buffer);
    m_boneDataMapped = mapped;
    m_boneMatrixCapacity = capacity;
}

void Scene::SetEditorMode2D(bool enabled)
{
    settings.dimension = enabled
        ? Engine::Model::SceneDimension::TwoD
        : Engine::Model::SceneDimension::ThreeD;
    if (m_editorCameraModeInitialized && m_editorMode2D == enabled)
        return;
    m_editorMode2D = enabled;
    Engine::Components::Camera* camera = editorCamera.GetComponent<Engine::Components::Camera>();
    if (!camera)
        return;
    m_editorCameraModeInitialized = true;
    camera->useTransformRotation = false;
    camera->orthographic = enabled;
    camera->target = { 0.f, 0.f, 0.f };
    camera->up = { 0.f, 1.f, 0.f };
    editorCamera.transform.position = enabled
        ? glm::vec3(0.f, 0.f, -10.f)
        : glm::vec3(0.f, 1.5f, 3.f);
}

void Scene::BuildSkyboxPipeline()
{
    auto* shaderCompiler = m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = m_graphicsProvider->GetPipelineStateFactory();
    if (!shaderCompiler || !pipelineFactory)
        throw std::runtime_error("Failed to get skybox shader or pipeline factory");

    const std::string shaderPath = EngineShaderPath("Skybox.hlsl");
    auto vertexShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "VSMain", Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!vertexShader)
        throw std::runtime_error("Failed to compile skybox vertex shader: " + shaderCompiler->GetLastError());
    auto pixelShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "PSMain", Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!pixelShader)
        throw std::runtime_error("Failed to compile skybox pixel shader: " + shaderCompiler->GetLastError());

    const auto buildSkyboxPipeline = [&](bool stencilClip,
        const char* description)
    {
        auto builder = pipelineFactory->CreateBuilder();
        if (!builder)
            throw std::runtime_error(std::string("Failed to create ") +
                description + " pipeline builder");
        auto& state = builder->SetVertexShader(vertexShader.get())
            .SetPixelShader(pixelShader.get())
            .SetFillMode(false)
            .SetCullMode(false)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(false)
            .SetBlendEnable(false)
            .SetDepthEnable(false)
            .SetDepthWriteEnable(false)
            .SetDepthFunc(7)
            .SetInputLayout(nullptr, 0)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20);
        if (stencilClip)
        {
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0x00)
                .SetStencilFunc(2)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(0)
                .SetStencilRef(1);
        }
        auto pipeline = state.Build();
        if (!pipeline)
            throw std::runtime_error(std::string("Failed to build ") +
                description + " pipeline: " + builder->GetLastError());
        return pipeline;
    };
    m_skyboxPipeline = buildSkyboxPipeline(false, "skybox");
    m_portalSkyboxStencilReadPipeline = buildSkyboxPipeline(true,
        "portal skybox stencil read");

    const std::string defaultPath =
        (std::filesystem::path(ENGINE_ASSETS_PATH) / "Textures" / "Skyboxes" /
            "editor-default-sky.png").string();
    m_defaultSkyboxTexture = Engine::Components::Texture::Acquire(defaultPath);
    if (!m_defaultSkyboxTexture->PrepareImmediate(m_graphicsProvider))
        m_defaultSkyboxTexture.reset();
}

const Engine::Components::Texture* Scene::ResolveSkyboxTexture()
{
    // A 2D scene has no surrounding environment.  Let the render target's
    // clear colour be its flat, edge-to-edge background instead of projecting
    // either the authored panorama or the editor's fallback skybox.
    if (settings.dimension == Engine::Model::SceneDimension::TwoD ||
        !settings.skyboxEnabled)
        return nullptr;

    if (settings.skyboxTexture.empty())
        return m_defaultSkyboxTexture.get();

    if (m_loadedSkyboxPath != settings.skyboxTexture)
    {
        m_loadedSkyboxPath = settings.skyboxTexture;
        m_sceneSkyboxTexture = Engine::Components::Texture::Acquire(settings.skyboxTexture);
    }
    if (m_sceneSkyboxTexture && m_sceneSkyboxTexture->Prepare(m_graphicsProvider))
        return m_sceneSkyboxTexture.get();
    return m_defaultSkyboxTexture.get();
}

const Engine::Components::Texture* Scene::GetSkyboxPreviewTexture()
{
    return ResolveSkyboxTexture();
}

void Scene::UpdateEnvironmentLighting(const Engine::Components::Texture* texture)
{
    const std::string path = texture ? texture->GetFilePath() : std::string{};
    auto environment = texture
        ? Engine::Rendering::EnvironmentMap::Acquire(*texture) : nullptr;
    if (path == m_environmentLightingPath && environment == m_environmentMap)
        return;

    m_environmentLightingPath = path;
    m_environmentMap = std::move(environment);
    if (m_environmentMap)
        m_environmentMap->Prepare(m_graphicsProvider);
    m_environmentSH = m_environmentMap
        ? m_environmentMap->GetRadianceSH() : std::array<glm::vec4, 9>{};
}

std::shared_ptr<Engine::Rendering::EnvironmentMap>
Scene::ResolveReflectionEnvironment(
    const Engine::Components::Material& material)
{
    if (!material.useCustomReflectionEnvironment ||
        !material.reflectionEnvironmentMap ||
        !material.reflectionEnvironmentMap->HasPixels())
        return nullptr;
    auto environment = Engine::Rendering::EnvironmentMap::Acquire(
        *material.reflectionEnvironmentMap);
    if (environment)
        environment->Prepare(m_graphicsProvider);
    return environment;
}

// ---------------------------------------------------------------------------
// Scene::BuildGridPipeline
// ---------------------------------------------------------------------------

void Scene::BuildGridPipeline()
{
    if (!m_graphicsProvider)
        throw std::runtime_error("Scene::BuildGridPipeline: graphicsProvider not initialized");

    auto* shaderCompiler = m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = m_graphicsProvider->GetPipelineStateFactory();

    if (!shaderCompiler || !pipelineFactory)
        throw std::runtime_error("Failed to get shader compiler or pipeline factory");

    // Engine shaders live with the referenced engine, not in each project's
    // Assets directory.
    std::string shaderPath = EngineShaderPath("Grid.hlsl");
    std::string currentDir = std::filesystem::current_path().string();
    std::string absolutePath = std::filesystem::absolute(shaderPath).string();

    OutputDebugStringA(("[Scene::BuildGridPipeline] Current directory: " + currentDir + "\n").c_str());
    OutputDebugStringA(("[Scene::BuildGridPipeline] Shader path: " + shaderPath + "\n").c_str());
    OutputDebugStringA(("[Scene::BuildGridPipeline] Absolute path: " + absolutePath + "\n").c_str());
    OutputDebugStringA(("[Scene::BuildGridPipeline] File exists: " + std::string(std::filesystem::exists(shaderPath) ? "YES" : "NO") + "\n").c_str());

    auto vsShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(),
        "VSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!vsShader)
        throw std::runtime_error("Failed to compile grid vertex shader: " + shaderCompiler->GetLastError());

    auto psShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(),
        "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!psShader)
        throw std::runtime_error("Failed to compile grid pixel shader: " + shaderCompiler->GetLastError());

    // Build pipeline state using fluent API
    auto builder = pipelineFactory->CreateBuilder();
    if (!builder)
        throw std::runtime_error("Failed to create pipeline state builder");

    // Grid pipeline:
    // - No vertex buffer (fullscreen triangle via SV_VertexID)
    // - Alpha blend for semi-transparency
    // - Preserve scene alpha under transparent grid cells and lines
    // - Depth test at the reconstructed grid plane
    auto& bp = *builder;
    m_gridPipeline = bp.SetVertexShader(vsShader.get())
        .SetPixelShader(psShader.get())
        .SetFillMode(false)                    // Solid fill
        .SetCullMode(false)                    // No culling (draw lines from both sides)
        .SetFrontCounterClockwise(false)
        .SetDepthClipEnable(true)
        .SetBlendEnable(true)
        .SetSrcBlend(4)                        // D3D12_BLEND_SRC_ALPHA (0-indexed: 4)
        .SetDestBlend(5)                       // D3D12_BLEND_INV_SRC_ALPHA (0-indexed: 5)
        .SetBlendOp(0)                         // D3D12_BLEND_OP_ADD (0-indexed: 0)
        .SetSrcBlendAlpha(1)                   // D3D12_BLEND_ONE (0-indexed: 1)
        .SetDestBlendAlpha(5)                  // D3D12_BLEND_INV_SRC_ALPHA (0-indexed: 5)
        .SetBlendOpAlpha(0)                    // D3D12_BLEND_OP_ADD (0-indexed: 0)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)                       // D3D12_COMPARISON_FUNC_LESS_EQUAL (0-indexed: 3)
        .SetInputLayout(nullptr, 0)            // No vertex buffer
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)         // DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT
        .Build();

    if (!m_gridPipeline)
        throw std::runtime_error("Failed to build grid pipeline: " + builder->GetLastError());
}

// ---------------------------------------------------------------------------
// Scene::BuildObjectPipeline
// ---------------------------------------------------------------------------

void Scene::BuildParticlePipeline()
{
    auto* compiler = m_graphicsProvider->GetShaderCompiler();
    auto* factory = m_graphicsProvider->GetPipelineStateFactory();
    if (!compiler || !factory)
        throw std::runtime_error("Particle rendering requires shader and pipeline factories");
    const std::string path = EngineShaderPath("Particle.hlsl");
    auto vertex = compiler->CompileFromFile(path, "VSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!vertex)
        throw std::runtime_error("Particle vertex shader: " + compiler->GetLastError());
    auto pixel = compiler->CompileFromFile(path, "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!pixel)
        throw std::runtime_error("Particle pixel shader: " + compiler->GetLastError());
    auto gpuVertex = compiler->CompileFromFile(path, "VSMainGpu",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!gpuVertex)
        throw std::runtime_error("GPU particle vertex shader: " +
            compiler->GetLastError());
    auto fluidVertex = compiler->CompileFromFile(path, "VSMainFluid",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!fluidVertex)
        throw std::runtime_error("Fluid surface vertex shader: " + compiler->GetLastError());
    auto fluidPixel = compiler->CompileFromFile(path, "PSMainFluid",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!fluidPixel)
        throw std::runtime_error("Fluid surface pixel shader: " + compiler->GetLastError());

    const Engine::Graphics::IPipelineStateBuilder::VertexElement layout[] = {
        { "POSITION", 0, 2, 0, 0, true },
        { "COLOR", 0, 2, 0, 16, true },
        { "TEXCOORD", 0, 2, 0, 32, true }
    };
    const auto build = [&](bool alphaBlend, bool portalView, bool gpu)
    {
        auto builder = factory->CreateBuilder();
        auto& state = builder->SetVertexShader(gpu ? gpuVertex.get() : vertex.get())
            .SetPixelShader(pixel.get())
            .SetFillMode(false)
            .SetCullMode(false)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(true)
            .SetBlendEnable(true)
            .SetSrcBlend(4) // source alpha
            .SetDestBlend(alphaBlend ? 5 : 1)
            .SetBlendOp(0)
            .SetSrcBlendAlpha(1)
            .SetDestBlendAlpha(alphaBlend ? 5 : 1)
            .SetBlendOpAlpha(0)
            .SetDepthEnable(true)
            .SetDepthWriteEnable(false)
            .SetDepthFunc(3);
        if (portalView)
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0x00)
                .SetStencilFunc(2)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(0)
                .SetStencilRef(1);
        auto pipeline = state
            .SetInputLayout(gpu ? nullptr : layout, gpu ? 0 : 3)
            .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20)
            .Build();
        if (!pipeline)
            throw std::runtime_error(std::string("Particle pipeline ") +
                (gpu ? "GPU " : "CPU ") +
                (alphaBlend ? "alpha " : "additive ") +
                (portalView ? "portal: " : "main: ") +
                builder->GetLastError());
        return pipeline;
    };
    m_particlePipeline = build(false, false, false);
    m_particleAlphaPipeline = build(true, false, false);
    m_particlePortalPipeline = build(false, true, false);
    m_particlePortalAlphaPipeline = build(true, true, false);
    m_particleGpuPipeline = build(false, false, true);
    m_particleGpuAlphaPipeline = build(true, false, true);
    m_particleGpuPortalPipeline = build(false, true, true);
    m_particleGpuPortalAlphaPipeline = build(true, true, true);
    const auto buildFluid = [&](bool portalView)
    {
        auto builder = factory->CreateBuilder();
        auto& state = builder->SetVertexShader(fluidVertex.get())
            .SetPixelShader(fluidPixel.get())
            .SetFillMode(false).SetCullMode(false)
            .SetFrontCounterClockwise(false).SetDepthClipEnable(true)
            .SetBlendEnable(true).SetSrcBlend(4).SetDestBlend(5)
            .SetBlendOp(0).SetSrcBlendAlpha(1).SetDestBlendAlpha(5)
            .SetBlendOpAlpha(0).SetDepthEnable(true)
            .SetDepthWriteEnable(false).SetDepthFunc(3);
        if (portalView)
            state.SetStencilEnable(true).SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0x00).SetStencilFunc(2)
                .SetStencilFailOp(0).SetStencilDepthFailOp(0)
                .SetStencilPassOp(0).SetStencilRef(1);
        auto pipeline = state.SetInputLayout(nullptr, 0)
            .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20).Build();
        if (!pipeline)
            throw std::runtime_error("Fluid surface pipeline: " + builder->GetLastError());
        return pipeline;
    };
    m_fluidSurfacePipeline = buildFluid(false);
    m_fluidSurfacePortalPipeline = buildFluid(true);
}

void Scene::RecordGpuParticleSimulation(IGraphicsContext* context)
{
    if (!context) return;
    for (const auto& object : m_objects)
        if (object && object->IsEnabledInHierarchy())
            if (auto* emitter = object->GetComponent<
                    Engine::Components::ParticleEmitter>())
                emitter->RecordGpuSimulation(context);
}

void Scene::RenderParticles(IGraphicsContext* context,
    const glm::mat4& viewProjection, const glm::mat4& view,
    const Object* contentScopeRoot, const glm::vec4& clipPlane,
    bool portalView)
{
    if (!context || !m_particlePipeline || !m_particleAlphaPipeline ||
        !m_particlePortalPipeline || !m_particlePortalAlphaPipeline ||
        !m_graphicsProvider)
        return;
    auto* contextFactory = m_graphicsProvider->GetContextFactory();
    auto* bufferFactory = m_graphicsProvider->GetBufferFactory();
    if (!contextFactory || !bufferFactory)
        return;
    const uint32_t frameSlot = contextFactory->GetPreparedFrameSlot();
    if (frameSlot >= m_particleDrawSlots.size())
        m_particleDrawSlots.resize(static_cast<size_t>(frameSlot) + 1u);
    const uint64_t serial = contextFactory->GetPreparedFrameSerial();
    auto& slot = m_particleDrawSlots[frameSlot];
    if (serial == 0 || serial != slot.preparedSerial)
    {
        slot.preparedSerial = serial;
        slot.constantCursor = 0;
        slot.batches.clear();
        slot.fluidSurfaces.clear();
        std::vector<ParticleGPUData> records;
        constexpr size_t RenderLimit = 100000;
        for (const auto& object : m_objects)
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
                    emitter->PrepareTexture(m_graphicsProvider),
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
                    m_retiredRenderBuffers.push_back(std::move(slot.vertices));
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
    for (const auto& object : m_objects)
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
            m_retiredRenderBuffers.push_back(std::move(slot.constants));
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
        if (portalView && !IsObjectInSpatialRegion(batch.owner,
            contentScopeRoot)) continue;
        const uint64_t offset = static_cast<uint64_t>(slot.constantCursor++) * 256u;
        constants.particleParams.x = batch.texture ? 1.f : 0.f;
        constants.particleParams.z = 0.f;
        std::memcpy(static_cast<uint8_t*>(slot.mappedConstants) + offset,
            &constants, sizeof(constants));
        slot.constants->FlushMappedWrites(offset, sizeof(constants));
        context->SetPipeline(portalView
            ? (batch.alphaBlend ? m_particlePortalAlphaPipeline.get()
                : m_particlePortalPipeline.get())
            : (batch.alphaBlend ? m_particleAlphaPipeline.get()
                : m_particlePipeline.get()));
        context->SetConstantBuffer(0, slot.constants.get(), offset);
        context->SetTexture(0, batch.texture
            ? batch.texture->GetGraphicsTexture() : nullptr);
        context->SetVertexBuffer(0, slot.vertices.get(), sizeof(ParticleGPUData),
            static_cast<uint64_t>(batch.first) * sizeof(ParticleGPUData));
        context->DrawInstanced(6, batch.count);
        if (portalView)
            m_lastParticlePortalDrawCount += batch.count;
    }
    for (const auto& surface : slot.fluidSurfaces)
    {
        if (portalView && !IsObjectInSpatialRegion(surface.owner,
            contentScopeRoot)) continue;
        const uint64_t offset = static_cast<uint64_t>(slot.constantCursor++) * 256u;
        constants.particleParams = glm::vec4(0.f, surface.opacity,
            static_cast<float>(GetElapsedTime()), 0.f);
        std::memcpy(static_cast<uint8_t*>(slot.mappedConstants) + offset,
            &constants, sizeof(constants));
        slot.constants->FlushMappedWrites(offset, sizeof(constants));
        context->SetPipeline(portalView ? m_fluidSurfacePortalPipeline.get()
            : m_fluidSurfacePipeline.get());
        context->SetConstantBuffer(0, slot.constants.get(), offset);
        context->SetStructuredBuffer(6, surface.particles.get());
        context->DrawInstanced(6, 1);
        if (portalView) m_lastParticlePortalDrawCount += surface.count;
    }
    for (const auto& object : m_objects)
    {
        if (!object || !object->IsEnabledInHierarchy() ||
            (portalView && !IsObjectInSpatialRegion(object.get(),
                contentScopeRoot))) continue;
        auto* emitter = object->GetComponent<Engine::Components::ParticleEmitter>();
        if (!emitter || !emitter->GetGpuDrawBuffer()) continue;
        const auto texture = emitter->PrepareTexture(m_graphicsProvider);
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
            ? (alpha ? m_particleGpuPortalAlphaPipeline.get()
                : m_particleGpuPortalPipeline.get())
            : (alpha ? m_particleGpuAlphaPipeline.get()
                : m_particleGpuPipeline.get()));
        context->SetConstantBuffer(0, slot.constants.get(), offset);
        context->SetTexture(0, texture ? texture->GetGraphicsTexture() : nullptr);
        context->SetStructuredBuffer(6, emitter->GetGpuDrawBuffer());
        context->DrawInstanced(6, emitter->GetGpuDrawCapacity());
        if (portalView)
            m_lastParticlePortalDrawCount += emitter->GetGpuDrawCapacity();
    }
    if (!portalView)
        context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Transparent);
}

Scene::MaterialShaderFuture Scene::QueueMaterialShaderBytecode(
    const std::string& path, bool terrain, int64_t stamp)
{
    const std::string key = path + "|" + (terrain ? "terrain|" : "mesh|") +
        std::to_string(stamp);
    if (auto found = m_materialShaderBytecode.find(key);
        found != m_materialShaderBytecode.end()) return found->second;
    auto compiler = m_graphicsProvider->GetShaderCompiler()->CreateWorkerCompiler();
    auto future = Engine::Rendering::RenderJobs::Get().Submit(
        [path, terrain, compiler = std::move(compiler)]() mutable {
            MaterialShaderBytecode result;
            const char* vertexEntry = terrain ? "VSTerrainMain" : "VSMain";
            auto vertex = compiler->CompileFromFile(path, vertexEntry,
                Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
            if (!vertex) { result.error = path + ": " + compiler->GetLastError(); return result; }
            auto pixel = compiler->CompileFromFile(path, "PSMain",
                Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
            if (!pixel) { result.error = path + ": " + compiler->GetLastError(); return result; }
            result.vertex = std::move(vertex);
            result.pixel = std::move(pixel);
            return result;
        }).share();
    return m_materialShaderBytecode.emplace(key, std::move(future)).first->second;
}

const Engine::Rendering::ShaderSourceCache::Source& Scene::ResolveMaterialShaderSource(
    const Engine::Components::Material& material, const std::string& path)
{
    AssertOwnerThread();
    static const Engine::Rendering::ShaderSourceCache::Source builtin{};
    if(path.empty()) return builtin;
    const auto before=m_materialShaderSources.GetStatistics().resolutions;
    const auto& source=m_materialShaderSources.Resolve(material.GetFilePath(),path,
        [&]{return material.ShaderSearchPaths(path);});
    m_lastPrepareTimings.itemShaderResolutions+=m_materialShaderSources.GetStatistics().resolutions-before;
    return source;
}

void Scene::PrewarmMaterialShaders(const Engine::Components::Material& material,
    bool terrain)
{
    if (!m_graphicsProvider || !m_graphicsProvider->GetShaderCompiler()) return;
    for (const auto& shader : material.shaders)
    {
        if (!shader.enabled || shader.path.empty()) continue;
        ++m_lastPrepareTimings.itemShaderRequests;
        if (!m_preparedMaterialShaderSources.emplace(material.GetFilePath(), shader.path, terrain).second)
            continue;
        const auto& source=ResolveMaterialShaderSource(material,shader.path);
        if(source.hlsl && source.exists)
            QueueMaterialShaderBytecode(source.path,terrain,source.revision);
    }
}

void Scene::PublishMaterialShaderPipelines(size_t maxPipelines)
{
    size_t published = 0;
    for (auto& [key, cached] : m_materialShaderPipelines)
    {
        if (published >= maxPipelines) break;
        if (!cached.bytecode.valid() || cached.pipeline || !cached.error.empty() ||
            cached.bytecode.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;
        try
        {
            const auto& bytecode = cached.bytecode.get();
            if (!bytecode.error.empty()) cached.error = bytecode.error;
            else if (auto builder = m_materialShaderBuilders.find(cached.base);
                builder != m_materialShaderBuilders.end())
                cached.pipeline = builder->second(bytecode.vertex.get(),
                    bytecode.pixel.get(), cached.overlay, cached.blendMode);
            else cached.error = "Material shader is unsupported for this render pass";
        }
        catch (const std::exception& exception) { cached.error = exception.what(); }
        ++published;
    }
}

Scene::IPipelineState* Scene::ResolveMaterialShaderPipeline(
    const Engine::Rendering::ShaderSourceCache::Source& requestedSource, IPipelineState* base, bool terrain,
    bool overlay, const std::string& blendMode, std::string& error)
{
    if (requestedSource.path.empty() && !overlay && blendMode == "Auto")
    {
        error.clear();
        return base;
    }
    const auto& source=requestedSource.path.empty()
        ? m_materialShaderSources.Resolve(EngineShaderPath("Object.hlsl")) : requestedSource;
    const std::string& path=source.path;
    const std::string key = std::to_string(reinterpret_cast<uintptr_t>(base)) +
        "|" + (overlay ? "overlay|" : "surface|") + blendMode + "|" + path;
    const int64_t stamp=source.revision;
    auto found = m_materialShaderPipelines.find(key);
    if (found != m_materialShaderPipelines.end() && found->second.sourceStamp == stamp)
    {
        error = found->second.error;
        return found->second.pipeline.get();
    }
    auto& cached = m_materialShaderPipelines[key];
    if (cached.pipeline)
        m_retiredMaterialShaderPipelines.push_back(std::move(cached.pipeline));
    cached.sourceStamp = stamp;
    cached.error.clear();
    cached.base = base;
    cached.overlay = overlay;
    cached.blendMode = blendMode;
    cached.bytecode = {};
    try
    {
        if (!source.hlsl)
            throw std::runtime_error("Material shader must reference an .hlsl asset: " + path);
        if (!source.exists)
            throw std::runtime_error("Material shader asset not found: " + path);
        const auto builder = m_materialShaderBuilders.find(base);
        if (builder == m_materialShaderBuilders.end())
            throw std::runtime_error("Material shader is unsupported for this render pass");
        cached.bytecode = QueueMaterialShaderBytecode(path, terrain, stamp);
    }
    catch (const std::exception& exception)
    {
        cached.error = exception.what();
    }
    error = cached.error;
    return cached.pipeline.get();
}

void Scene::BuildObjectPipeline()
{
    if (!m_graphicsProvider)
        throw std::runtime_error("Scene::BuildObjectPipeline: graphicsProvider not initialized");

    auto* shaderCompiler = m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = m_graphicsProvider->GetPipelineStateFactory();

    if (!shaderCompiler || !pipelineFactory)
        throw std::runtime_error("Failed to get shader compiler or pipeline factory");

    // Compile shaders
    const std::string shaderPath = EngineShaderPath("Object.hlsl");
    auto vsShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(),
        "VSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!vsShader)
        throw std::runtime_error("Failed to compile object vertex shader: " + shaderCompiler->GetLastError());
    auto terrainVsShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "VSTerrainMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!terrainVsShader)
        throw std::runtime_error("Failed to compile terrain vertex shader: " +
            shaderCompiler->GetLastError());

    auto psShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(),
        "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!psShader)
        throw std::runtime_error("Failed to compile object pixel shader: " + shaderCompiler->GetLastError());
    auto shadowPsShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "PSShadowMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!shadowPsShader)
        throw std::runtime_error("Failed to compile shadow pixel shader: " +
            shaderCompiler->GetLastError());

    // Engine-native model stream, including the second UV set and vertex colour.
    Engine::Graphics::IPipelineStateBuilder::VertexElement layout[] =
    {
        { "POSITION", 0, 6, 0,  0, false },   // DXGI_FORMAT_R32G32B32_FLOAT = 6
        { "NORMAL",   0, 6, 0, 12, false },   // DXGI_FORMAT_R32G32B32_FLOAT = 6, offset 12
        { "TEXCOORD", 0, 16, 0, 24, false },  // DXGI_FORMAT_R32G32_FLOAT = 16
        { "TANGENT",  0, 2, 0, 32, false },   // DXGI_FORMAT_R32G32B32A32_FLOAT = 2
        { "TEXCOORD", 1, 16, 0, 48, false },
        { "COLOR",    0, 2, 0, 56, false },
        { "JOINTS",   0, 2, 0, 72, false },
        { "WEIGHTS",  0, 2, 0, 88, false },
        { "JOINTS",   1, 2, 0, 104, false },
        { "WEIGHTS",  1, 2, 0, 120, false },
        { "TEXCOORD", 3, 2, 0, 136, false },
        { "TEXCOORD", 4, 2, 0, 152, false },
        { "TEXCOORD", 5, 2, 0, 168, false },
        { "TEXCOORD", 6, 2, 0, 184, false },
        { "TEXCOORD", 7, 2, 0, 200, false },
        { "TEXCOORD", 8, 2, 0, 216, false },
        { "TEXCOORD", 9, 2, 0, 232, false },
        { "TEXCOORD", 10, 2, 0, 248, false },
    };
    Engine::Graphics::IPipelineStateBuilder::VertexElement terrainLayout[] =
    {
        { "POSITION", 0, 6, 0,  0, false },
        { "NORMAL",   0, 6, 0, 12, false },
        { "TEXCOORD", 0, 16, 0, 24, false },
        { "COLOR",    0, 2, 0, 32, false },
    };
    m_terrainPipelineByBase.clear();
    m_materialShaderBuilders.clear();
    m_materialShaderPipelines.clear();
    m_materialShaderBytecode.clear();
    if (!m_deferGraphicsRetirement)
        m_retiredMaterialShaderPipelines.clear();

    enum class PortalStencilMode
    {
        None,
        Write,
        Increment,
        ResetDepth,
        Read
    };

    // The same state builder is used by built-in and asset-backed passes so
    // terrain layouts, wireframe, culling and portal stencil tests stay aligned.
    auto buildConfiguredMaterialPipeline = [pipelineFactory, layout, terrainLayout](
                                     const Engine::Graphics::IShader* vertex,
                                     const Engine::Graphics::IShader* pixel,
                                     bool doubleSided, bool blend,
                                     bool wireframe,
                                     PortalStencilMode stencilMode,
                                     bool colorWriteEnabled,
                                     const char* description,
                                     bool terrainFormat,
                                     bool overlay, const std::string& blendMode)
    {
        if (blendMode == "Opaque") blend = false;
        else if (blendMode == "Alpha" || blendMode == "Additive" || overlay) blend = true;
        auto materialBuilder = pipelineFactory->CreateBuilder();
        if (!materialBuilder)
            throw std::runtime_error(
                std::string("Failed to create ") + description +
                " pipeline state builder");
        auto& state = materialBuilder->SetVertexShader(vertex)
            .SetPixelShader(pixel)
            .SetFillMode(wireframe)
            .SetCullMode(!doubleSided)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(true)
            .SetBlendEnable(blend)
            .SetColorWriteMask(colorWriteEnabled ? 0x0F : 0x00);
        if (blend)
        {
            state.SetSrcBlend(4)
                .SetDestBlend(blendMode == "Additive" ? 1 : 5)
                .SetBlendOp(0)
                .SetSrcBlendAlpha(1)
                .SetDestBlendAlpha(0)
                .SetBlendOpAlpha(0);
        }

        switch (stencilMode)
        {
        case PortalStencilMode::Write:
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0xFF)
                .SetStencilFunc(7)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(2)
                .SetStencilRef(1);
            break;
        case PortalStencilMode::Increment:
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0xFF)
                .SetStencilFunc(2)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(3)
                .SetStencilRef(1);
            break;
        case PortalStencilMode::Read:
        case PortalStencilMode::ResetDepth:
            state.SetStencilEnable(true)
                .SetStencilReadMask(0xFF)
                .SetStencilWriteMask(0x00)
                .SetStencilFunc(2)
                .SetStencilFailOp(0)
                .SetStencilDepthFailOp(0)
                .SetStencilPassOp(0)
                .SetStencilRef(1);
            break;
        case PortalStencilMode::None:
        default:
            state.SetStencilEnable(false);
            break;
        }

        auto pipeline = state.SetDepthEnable(true)
            .SetDepthWriteEnable(
                stencilMode == PortalStencilMode::ResetDepth ? true :
                stencilMode == PortalStencilMode::Write ||
                stencilMode == PortalStencilMode::Increment
                    ? false : (!overlay && !blend))
            .SetDepthFunc(
                stencilMode == PortalStencilMode::ResetDepth ? 7 :
                stencilMode == PortalStencilMode::Write ||
                stencilMode == PortalStencilMode::Increment
                ? 3 : ((blend || overlay) ? 3 : 1))
            .SetInputLayout(terrainFormat ? terrainLayout : layout,
                terrainFormat ? 4u : 18u)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20)
            .Build();
        if (!pipeline)
            throw std::runtime_error(
                std::string("Failed to build ") + description +
                " pipeline: " + materialBuilder->GetLastError());
        return pipeline;
    };

    auto buildMaterialPipelineVariant = [&](bool doubleSided, bool blend,
                                     bool wireframe,
                                     PortalStencilMode stencilMode,
                                     bool colorWriteEnabled,
                                     const char* description,
                                     bool terrainFormat,
                                     bool shadowPass = false)
    {
        auto pipeline = buildConfiguredMaterialPipeline(
            terrainFormat ? terrainVsShader.get() : vsShader.get(),
            shadowPass ? shadowPsShader.get() : psShader.get(),
            doubleSided, blend, wireframe, stencilMode, colorWriteEnabled,
            description, terrainFormat, false, "Auto");
        if (colorWriteEnabled && !shadowPass)
            m_materialShaderBuilders.emplace(pipeline.get(),
                [buildConfiguredMaterialPipeline, doubleSided, blend, wireframe,
                 stencilMode, terrainFormat](const Engine::Graphics::IShader* vertex,
                    const Engine::Graphics::IShader* pixel, bool overlay,
                    const std::string& blendMode)
                {
                    return buildConfiguredMaterialPipeline(vertex, pixel,
                        doubleSided, blend, wireframe, stencilMode, true,
                        "material shader asset", terrainFormat, overlay, blendMode);
                });
        return pipeline;
    };

    auto buildMaterialPipeline = [&](bool doubleSided, bool blend,
                                     bool wireframe,
                                     PortalStencilMode stencilMode,
                                     bool colorWriteEnabled,
                                     const char* description,
                                     bool shadowPass = false)
    {
        auto base = buildMaterialPipelineVariant(doubleSided, blend,
            wireframe, stencilMode, colorWriteEnabled, description, false,
            shadowPass);
        auto terrain = buildMaterialPipelineVariant(doubleSided, blend,
            wireframe, stencilMode, colorWriteEnabled, description, true,
            shadowPass);
        m_terrainPipelineByBase.emplace(base.get(), std::move(terrain));
        return base;
    };

    m_objectPipeline =
        buildMaterialPipeline(false, false, false, PortalStencilMode::None,
            true, "opaque material");
    m_objectDoubleSidedPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::None,
            true,
            "double-sided material");
    m_objectBlendPipeline =
        buildMaterialPipeline(false, true, false, PortalStencilMode::None,
            true, "blended material");
    m_objectBlendDoubleSidedPipeline =
        buildMaterialPipeline(true, true, false, PortalStencilMode::None,
            true,
            "blended double-sided material");

    // Reuse the complete object vertex path for skinning and morphs. The
    // shadow pixel shader only evaluates alpha-mask cutouts before depth write.
    m_directionalShadowPipeline =
        buildMaterialPipeline(false, false, false, PortalStencilMode::None,
            false, "directional shadow caster", true);
    m_directionalShadowDoubleSidedPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::None,
            false, "double-sided directional shadow caster", true);

    m_objectWirePipeline =
        buildMaterialPipeline(false, false, true, PortalStencilMode::None,
            true,
            "wireframe opaque material");
    m_objectWireDoubleSidedPipeline =
        buildMaterialPipeline(true, false, true, PortalStencilMode::None,
            true,
            "wireframe double-sided material");
    m_objectBlendWirePipeline =
        buildMaterialPipeline(false, true, true, PortalStencilMode::None,
            true,
            "wireframe blended material");
    m_objectBlendWireDoubleSidedPipeline =
        buildMaterialPipeline(true, true, true, PortalStencilMode::None,
            true,
            "wireframe blended double-sided material");

    // Placement previews always blend, regardless of the source alpha mode.
    m_objectPreviewPipeline =
        buildMaterialPipeline(false, true, false, PortalStencilMode::None,
            true, "object preview");
    m_objectPreviewDoubleSidedPipeline =
        buildMaterialPipeline(true, true, false, PortalStencilMode::None,
            true,
            "double-sided object preview");
    m_objectPreviewWirePipeline =
        buildMaterialPipeline(false, true, true, PortalStencilMode::None,
            true,
            "wireframe object preview");
    m_objectPreviewWireDoubleSidedPipeline =
        buildMaterialPipeline(true, true, true, PortalStencilMode::None,
            true,
            "wireframe double-sided object preview");

    m_objectPortalStencilWritePipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::Write,
            false, "portal stencil write");
    m_objectPortalStencilIncrementPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::Increment,
            false, "recursive portal stencil increment");
    m_objectPortalDepthResetPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::ResetDepth,
            false, "portal aperture depth reset");
    m_objectPortalStencilReadPipeline =
        buildMaterialPipeline(false, false, false, PortalStencilMode::Read,
            true, "portal stencil read opaque");
    m_objectPortalStencilReadDoubleSidedPipeline =
        buildMaterialPipeline(true, false, false, PortalStencilMode::Read,
            true, "portal stencil read double-sided");
    m_objectPortalStencilReadBlendPipeline =
        buildMaterialPipeline(false, true, false, PortalStencilMode::Read,
            true, "portal stencil read blend");
    m_objectPortalStencilReadBlendDoubleSidedPipeline =
        buildMaterialPipeline(true, true, false, PortalStencilMode::Read,
            true, "portal stencil read blend double-sided");
    m_objectPortalStencilReadWirePipeline =
        buildMaterialPipeline(false, false, true, PortalStencilMode::Read,
            true, "portal stencil read wire");
    m_objectPortalStencilReadWireDoubleSidedPipeline =
        buildMaterialPipeline(true, false, true, PortalStencilMode::Read,
            true, "portal stencil read wire double-sided");
    m_objectPortalStencilReadBlendWirePipeline =
        buildMaterialPipeline(false, true, true, PortalStencilMode::Read,
            true, "portal stencil read wire blend");
    m_objectPortalStencilReadBlendWireDoubleSidedPipeline =
        buildMaterialPipeline(true, true, true, PortalStencilMode::Read,
            true, "portal stencil read wire blend double-sided");
    m_objectPortalStencilReadPreviewPipeline =
        buildMaterialPipeline(false, true, false, PortalStencilMode::Read,
            true, "portal stencil read preview");
    m_objectPortalStencilReadPreviewDoubleSidedPipeline =
        buildMaterialPipeline(true, true, false, PortalStencilMode::Read,
            true, "portal stencil read preview double-sided");
    m_objectPortalStencilReadPreviewWirePipeline =
        buildMaterialPipeline(false, true, true, PortalStencilMode::Read,
            true, "portal stencil read preview wire");
    m_objectPortalStencilReadPreviewWireDoubleSidedPipeline =
        buildMaterialPipeline(true, true, true, PortalStencilMode::Read,
            true, "portal stencil read preview wire double-sided");

    const auto buildSpatialDebugPipeline = [&](bool wireframe)
    {
        auto debugBuilder = pipelineFactory->CreateBuilder();
        if (!debugBuilder)
            throw std::runtime_error("Failed to create spatial debug pipeline builder");
        auto pipeline = debugBuilder->SetVertexShader(vsShader.get())
            .SetPixelShader(psShader.get())
            .SetFillMode(wireframe)
            .SetCullMode(false)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(true)
            .SetBlendEnable(true)
            .SetSrcBlend(4)
            .SetDestBlend(5)
            .SetBlendOp(0)
            .SetSrcBlendAlpha(1)
            .SetDestBlendAlpha(0)
            .SetBlendOpAlpha(0)
            .SetDepthEnable(false)
            .SetDepthWriteEnable(false)
            .SetDepthFunc(7)
            .SetInputLayout(layout, 18)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(10, 20)
            .Build();
        if (!pipeline)
            throw std::runtime_error("Failed to build spatial debug pipeline: " +
                debugBuilder->GetLastError());
        return pipeline;
    };
    m_objectSpatialDebugPipeline = buildSpatialDebugPipeline(false);
    m_objectSpatialDebugWirePipeline = buildSpatialDebugPipeline(true);

    // Build a wireframe outline pipeline for selected object highlighting.
    auto outlineBuilder = pipelineFactory->CreateBuilder();
    if (!outlineBuilder)
        throw std::runtime_error("Failed to create outline pipeline state builder");

    // Compile dedicated outline shaders from file instead of reusing object shader.
    const std::string outlineShaderPath = EngineShaderPath("ObjectOutline.hlsl");
    auto outlineVsShader = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(),
        "VSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!outlineVsShader)
        throw std::runtime_error("Failed to compile object outline vertex shader: " + shaderCompiler->GetLastError());
    auto terrainOutlineVsShader = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(), "VSTerrainMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    if (!terrainOutlineVsShader)
        throw std::runtime_error("Failed to compile terrain outline vertex shader: " +
            shaderCompiler->GetLastError());

    auto outlinePsShader = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(),
        "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!outlinePsShader)
        throw std::runtime_error("Failed to compile object outline pixel shader: " + shaderCompiler->GetLastError());

    auto& op = *outlineBuilder;
    m_objectOutlinePipeline = op.SetVertexShader(outlineVsShader.get())
        .SetPixelShader(outlinePsShader.get())
        .SetFillMode(true)                     // Wireframe outline
        .SetCullMode(false)
        .SetFrontCounterClockwise(false)
        .SetDepthClipEnable(true)
        .SetBlendEnable(false)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)                       // D3D12_COMPARISON_FUNC_LESS_EQUAL
        .SetInputLayout(layout, 18)
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)
        .Build();
    if (!m_objectOutlinePipeline)
        throw std::runtime_error("Failed to build object outline pipeline: " + outlineBuilder->GetLastError());

    auto pmxOutlineVs = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(), "VSMmdMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    auto pmxOutlinePs = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(), "PSMmdMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!pmxOutlineVs || !pmxOutlinePs)
        throw std::runtime_error("Failed to compile PMX outline shaders: " +
            shaderCompiler->GetLastError());
    auto pmxOutlineBuilder = pipelineFactory->CreateBuilder();
    m_pmxOutlinePipeline = pmxOutlineBuilder->SetVertexShader(pmxOutlineVs.get())
        .SetPixelShader(pmxOutlinePs.get())
        .SetFillMode(false)
        .SetCullMode(true)
        .SetFrontCounterClockwise(true)
        .SetDepthClipEnable(true)
        .SetBlendEnable(true)
        .SetSrcBlend(4)
        .SetDestBlend(5)
        .SetBlendOp(0)
        .SetSrcBlendAlpha(1)
        .SetDestBlendAlpha(0)
        .SetBlendOpAlpha(0)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)
        .SetInputLayout(layout, 18)
        .SetPrimitiveTopology(
            Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)
        .Build();
    if (!m_pmxOutlinePipeline)
        throw std::runtime_error("Failed to build PMX outline pipeline: " +
            pmxOutlineBuilder->GetLastError());

    auto terrainOutlineBuilder = pipelineFactory->CreateBuilder();
    if (!terrainOutlineBuilder)
        throw std::runtime_error("Failed to create terrain outline pipeline builder");
    auto terrainOutline = terrainOutlineBuilder
        ->SetVertexShader(terrainOutlineVsShader.get())
        .SetPixelShader(outlinePsShader.get())
        .SetFillMode(true)
        .SetCullMode(false)
        .SetFrontCounterClockwise(false)
        .SetDepthClipEnable(true)
        .SetBlendEnable(false)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)
        .SetInputLayout(terrainLayout, 4)
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(10, 20)
        .Build();
    if (!terrainOutline)
        throw std::runtime_error("Failed to build terrain outline pipeline: " +
            terrainOutlineBuilder->GetLastError());
    m_terrainPipelineByBase.emplace(m_objectOutlinePipeline.get(),
        std::move(terrainOutline));
}

// ---------------------------------------------------------------------------
// Scene::PrepareRenderFrame
// ---------------------------------------------------------------------------

Scene::RetiredGraphicsResources Scene::TakeRetiredGraphicsResources()
{
    RetiredGraphicsResources retired;
    retired.buffers.swap(m_retiredRenderBuffers);
    retired.sharedBuffers.swap(m_retiredSharedRenderBuffers);
    retired.pipelines.swap(m_retiredMaterialShaderPipelines);
    retired.textures.swap(m_retiredShadowTextures);
    return retired;
}

void Scene::RetireGraphicsBuffer(std::unique_ptr<IGraphicsBuffer> buffer)
{
    if (buffer && m_deferGraphicsRetirement)
        m_retiredRenderBuffers.push_back(std::move(buffer));
}

void Scene::RetireGraphicsBuffer(std::shared_ptr<IGraphicsBuffer> buffer)
{
    if (buffer && m_deferGraphicsRetirement)
        m_retiredSharedRenderBuffers.push_back(std::move(buffer));
}

struct Scene::NativeRenderPacket
{
    std::vector<FrameRenderItem> items;
    std::vector<std::shared_ptr<IGraphicsBuffer>> meshBuffers;
};

std::shared_ptr<const Scene::NativeRenderPacket> Scene::TakeNativeRenderPacket()
{
    AssertOwnerThread();
    if (!m_renderFramePrepared || m_nativeRenderPacketTaken)
        throw std::logic_error("Native render packet requires a prepared scene frame");
    m_nativeRenderPacketTaken = true;
    auto packet = std::make_shared<NativeRenderPacket>();
    packet->items = std::move(m_frameRenderItems);
    packet->meshBuffers.reserve(packet->items.size() * 2);
    for (const FrameRenderItem& item : packet->items)
    {
        if (!item.mesh) continue;
        if (auto vertex = item.mesh->GetVertexBufferOwner())
            packet->meshBuffers.push_back(std::move(vertex));
        if (auto index = item.mesh->GetIndexBufferOwner())
            packet->meshBuffers.push_back(std::move(index));
    }
    return packet;
}

void Scene::RetainNativePacketMeshBuffers(const NativeRenderPacket& packet,
    Engine::Graphics::GraphicsCommandPacket& commands) const
{
    AssertOwnerThread();
    for (const auto& buffer : packet.meshBuffers)
        commands.RetainBuffer(buffer);
}

void Scene::SelectNativeFrameBuffers()
{
    if (!m_deferGraphicsRetirement || !m_graphicsProvider ||
        !m_graphicsProvider->GetContextFactory()) return;
    const uint32_t slotIndex = m_graphicsProvider->GetContextFactory()
        ->GetPreparedFrameSlot();
    if (m_nativeFrameBuffers.size() <= slotIndex)
        m_nativeFrameBuffers.resize(static_cast<size_t>(slotIndex) + 1u);
    if (!m_nativeFrameBuffers[slotIndex])
        m_nativeFrameBuffers[slotIndex] = std::make_unique<NativeFrameBuffers>();
    auto* selected = m_nativeFrameBuffers[slotIndex].get();
    if (selected == m_activeNativeFrameBuffers) return;
    if (!m_activeNativeFrameBuffers)
    {
        m_activeNativeFrameBuffers = selected;
        return;
    }
    const auto exchange = [&](NativeFrameBuffers& slot)
    {
        using std::swap;
        swap(m_gridConstantBuffer, slot.grid);
        swap(m_gridCBMapped, slot.gridMapped);
        swap(m_skyboxConstantBuffer, slot.skybox);
        swap(m_skyboxCBMapped, slot.skyboxMapped);
        swap(m_lightDataBuffer, slot.lights);
        swap(m_lightDataMapped, slot.lightsMapped);
        swap(m_boneDataBuffer, slot.bones);
        swap(m_boneDataMapped, slot.bonesMapped);
        swap(m_boneMatrixCapacity, slot.boneCapacity);
        swap(m_clusterHeaderBuffer, slot.clusterHeaders);
        swap(m_clusterHeaderMapped, slot.clusterHeadersMapped);
        swap(m_clusterHeaderCapacity, slot.clusterHeaderCapacity);
        swap(m_clusterLightIndexBuffer, slot.clusterIndices);
        swap(m_clusterLightIndexMapped, slot.clusterIndicesMapped);
        swap(m_clusterLightIndexCapacity, slot.clusterIndexCapacity);
        swap(m_portalApertureBuffer, slot.portalAperture);
        swap(m_portalApertureMapped, slot.portalApertureMapped);
    };
    auto* previous = m_activeNativeFrameBuffers;
    exchange(*previous);
    exchange(*selected);
    auto* factory = m_graphicsProvider->GetBufferFactory();
    const auto createLike = [&](const std::unique_ptr<IGraphicsBuffer>& source,
        std::unique_ptr<IGraphicsBuffer>& destination, void*& mapped)
    {
        if (!source || destination) return;
        destination = factory->CreateBuffer(source->GetUsage(),
            source->GetAccessMode(), source->GetSize(), nullptr,
            source->GetElementStride());
        mapped = destination ? destination->Map() : nullptr;
        if (!mapped)
            throw std::runtime_error("Failed to allocate native frame upload buffer");
    };
    createLike(previous->grid, m_gridConstantBuffer, m_gridCBMapped);
    createLike(previous->skybox, m_skyboxConstantBuffer, m_skyboxCBMapped);
    createLike(previous->lights, m_lightDataBuffer, m_lightDataMapped);
    createLike(previous->bones, m_boneDataBuffer, m_boneDataMapped);
    if (m_boneDataBuffer && !m_boneMatrixCapacity)
        m_boneMatrixCapacity = previous->boneCapacity;
    createLike(previous->portalAperture, m_portalApertureBuffer,
        m_portalApertureMapped);
    m_activeNativeFrameBuffers = selected;
}

void Scene::ProcessGraphicsPreparation(IGraphicsProvider* graphicsProvider)
{
    AssertOwnerThread();
    if (!m_graphicsPreparationPending || !graphicsProvider) return;
    m_graphicsPreparationPending = false;
    for (const auto& object : m_objects)
        for (auto* component : object->Components)
            if (component) component->PrepareGraphicsResources(graphicsProvider);
}

void Scene::PrepareRenderFrame(bool publishSnapshot)
{
    AssertOwnerThread();
    Engine::Components::Texture::PublishQueued(m_graphicsProvider);
    m_materialShaderSources.Poll();
    PublishMaterialShaderPipelines();
    const bool priorPublicationSuppressed = m_suppressSceneSnapshotPublication;
    struct RestorePublication
    {
        bool& flag;
        bool previous;
        ~RestorePublication() { flag = previous; }
    } restorePublication{m_suppressSceneSnapshotPublication, priorPublicationSuppressed};
    m_suppressSceneSnapshotPublication = priorPublicationSuppressed || !publishSnapshot;
    // Commit editor edits before preparing owned draw data. Replay-only editor
    // frames publish on their simulation commit instead of scanning twice.
    CommitPendingObjectChanges();
    // Copy the registry view: deriving prefab batches adds scene components.
    const auto objectLayers=GetComponents<Engine::Components::TerrainObjectLayer>();
    for (auto* layer:objectLayers) layer->PreparePrefabInstances(m_graphicsProvider);
    CommitPendingObjectChanges();
    ProcessGraphicsPreparation(m_graphicsProvider);
    SelectNativeFrameBuffers();
    const auto preparationJobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
    using PrepareClock = std::chrono::steady_clock;
    m_lastPrepareTimings={};
    m_renderFramePrepared = false;
    m_nativeRenderPacketTaken = false;
    m_frameRenderItems.clear();
    ++m_materialPreparationSerial;
    m_preparedMaterialShaderSources.clear();
    m_frameLightCount = 0;
    m_lastSkinnedObjectCount = 0;

    if (!m_graphicsProvider || !m_lightDataMapped || !m_boneDataMapped)
        return;

    const auto transformBegin=PrepareClock::now();
    const auto structureRevision=GetStructureRevision();
    const bool preparedTransforms=structureRevision==m_transformPreparationRevision;
    m_transformPreparationRevision=structureRevision;
    Engine::Components::TransformHierarchy::Statistics transforms;
    double transformGatherMs=0;
    if(preparedTransforms)
    {
        m_transformPreparationScratch.clear(); m_transformPreparationScratch.reserve(m_objects.size());
        for(const auto& object:m_objects) m_transformPreparationScratch.push_back(&object->transform);
        const auto transformCaptured=PrepareClock::now();
        transformGatherMs=std::chrono::duration<double,std::milli>(transformCaptured-transformBegin).count();
        transforms=m_transformHierarchy.Prepare(m_transformPreparationScratch);
        if(!transforms.valid) throw std::runtime_error("Cannot prepare a cyclic or duplicate transform hierarchy");
    }
    // Streaming/object commit frames retain lazy owner validation. This avoids
    // rebuilding a whole hierarchy index to evaluate a handful of new leaves.
    const auto afterTransforms=PrepareClock::now();

    if(m_frameLookupRevision!=GetStructureRevision() || m_frameLookupEditPose!=m_editorMeshEditPose || m_frameLookupFull)
    {
        const auto describe=[&](Engine::Core::Object* object) {
            FrameComponentLookup lookup{}; lookup.object=object;
            lookup.mesh=object->GetComponent<Engine::Components::Mesh>();
            lookup.sprite=object->GetComponent<Engine::Components::Sprite>();
            lookup.material=object->GetComponent<Engine::Components::Material>();
            lookup.spatialManipulator=object->GetComponent<Engine::Components::SpatialManipulator>();
            lookup.skinnedMesh=m_editorMeshEditPose ? nullptr:object->GetComponent<Engine::Components::SkinnedMesh>();
            lookup.bakedLighting=object->GetComponent<Engine::Rendering::BakedLightingData>(); return lookup;
        };
        if(m_frameLookupFull || m_frameLookupEditPose!=m_editorMeshEditPose)
        {
            m_staticRenderItemCache.clear();
            m_frameComponentLookupScratch.clear(); m_frameLookupIndices.clear(); m_frameLookupFree.clear();
            m_frameComponentLookupScratch.reserve(m_objects.size());
            for(const auto& object:m_objects)
            { m_frameLookupIndices[object.get()]=m_frameComponentLookupScratch.size(); m_frameComponentLookupScratch.push_back(describe(object.get())); }
        }
        else for(auto* owner:m_frameLookupDirty)
        {
            m_staticRenderItemCache.erase(owner);
            auto found=m_frameLookupIndices.find(owner);
            if(!m_componentRegistry.Contains(owner))
            {
                if(found!=m_frameLookupIndices.end())
                { m_frameComponentLookupScratch[found->second]={}; m_frameLookupFree.push_back(found->second); m_frameLookupIndices.erase(found); }
                continue;
            }
            if(found!=m_frameLookupIndices.end()) m_frameComponentLookupScratch[found->second]=describe(owner);
            else
            {
                size_t index=m_frameComponentLookupScratch.size();
                if(m_frameLookupFree.empty()) m_frameComponentLookupScratch.push_back(describe(owner));
                else { index=m_frameLookupFree.back(); m_frameLookupFree.pop_back(); m_frameComponentLookupScratch[index]=describe(owner); }
                m_frameLookupIndices[owner]=index;
            }
        }
        m_frameSpatialManipulatorScratch.clear();
        for(const auto& lookup:m_frameComponentLookupScratch) if(lookup.spatialManipulator) m_frameSpatialManipulatorScratch.push_back(lookup.spatialManipulator);
        m_frameLookupDirty.clear(); m_frameLookupFull=false;
        m_frameLookupRevision=GetStructureRevision(); m_frameLookupEditPose=m_editorMeshEditPose;
    }
    if(m_materialCacheStructureRevision!=GetStructureRevision())
    {
        std::unordered_set<const Engine::Components::Material*> live;
        for(const auto& lookup:m_frameComponentLookupScratch) if(lookup.material) live.insert(lookup.material);
        for(auto it=m_renderMaterialCache.begin();it!=m_renderMaterialCache.end();)
            if(!live.count(it->first)) it=m_renderMaterialCache.erase(it); else ++it;
        m_materialCacheStructureRevision=GetStructureRevision();
    }
    size_t requiredBoneMatrices = 0;
    for (const auto& lookup : m_frameComponentLookupScratch)
        if (lookup.skinnedMesh)
            if (const auto* skeleton = lookup.skinnedMesh->ResolveSkeleton())
                requiredBoneMatrices += skeleton->jointNodes.size();
    if (requiredBoneMatrices > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Bone matrix count exceeds renderer index range");
    EnsureSkinPaletteCapacity(static_cast<uint32_t>(requiredBoneMatrices));

    // Nonlinear mesh results depend on authored geometry and active warp
    // volumes, not on camera motion. A compact signature lets both editor and
    // runtime frames reuse static CPU deformation and its upload buffer.
    uint64_t warpRevision = 1469598103934665603ull;
    bool hasActiveWarpVolume = false;
    for (const FrameComponentLookup& lookup : m_frameComponentLookupScratch)
    {
        if (!lookup.object || !lookup.spatialManipulator)
            continue;
        const auto* manipulator = lookup.spatialManipulator;
        hasActiveWarpVolume = hasActiveWarpVolume ||
            (manipulator->enabled && manipulator->definesWarpVolume &&
                lookup.object->IsEnabledInHierarchy());
        HashRevision(warpRevision,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(manipulator)));
        HashRevision(warpRevision, manipulator->GetConfigurationRevision());
        HashRevision(warpRevision,
            lookup.object->transform.GetWorldRevision());
        HashRevision(warpRevision, manipulator->enabled ? 1u : 0u);
        HashRevision(warpRevision, manipulator->definesWarpVolume ? 1u : 0u);
        HashRevision(warpRevision,
            lookup.object->IsEnabledInHierarchy() ? 1u : 0u);
    }

    std::optional<SpatialWarpSnapshot> warpSnapshot;
    m_frameRenderItems.reserve(m_objects.size());
    if (!hasActiveWarpVolume)
    {
        if (m_deferGraphicsRetirement)
            for (auto& [object, cached] : m_warpedRenderMeshes)
                RetireGraphicsBuffer(std::move(cached.vertexBuffer));
        m_warpedRenderMeshes.clear();
    }
    // The authored Mesh buffer cannot be changed for rendering a nonlinear
    // volume: it may also back a collider, an editor asset, or another draw.
    // Keep a private upload buffer for each affected object instead.
    for (auto cache = m_warpedRenderMeshes.begin();
         cache != m_warpedRenderMeshes.end();)
    {
        const bool stillInScene = std::any_of(m_objects.begin(), m_objects.end(),
            [&](const std::unique_ptr<Engine::Core::Object>& object)
            {
                return object.get() == cache->first;
            });
        if (!stillInScene)
        {
            RetireGraphicsBuffer(std::move(cache->second.vertexBuffer));
            cache = m_warpedRenderMeshes.erase(cache);
        }
        else
            ++cache;
    }
    std::unordered_map<Engine::Core::Object*,
        std::vector<Engine::Components::SpatialManipulator::TraversalRenderInstance>>
        traversalRenderInstances;
    for (const FrameComponentLookup& lookup : m_frameComponentLookupScratch)
    {
        if (!lookup.object)
            continue;
        if (auto* manipulator = lookup.spatialManipulator)
        {
            std::vector<Engine::Components::SpatialManipulator::TraversalRenderInstance>
                instances;
            manipulator->AppendTraversalRenderInstances(instances);
            for (const auto& instance : instances)
            {
                if (instance.object)
                    traversalRenderInstances[instance.object].push_back(instance);
            }
        }
    }
    uint32_t skinPaletteSlot = 0;
    uint32_t boneMatrixOffset = 0;
    const auto afterLookup = m_updateProfilingEnabled
        ? PrepareClock::now() : PrepareClock::time_point{};
    struct PrepareItemTimer
    {
        double& milliseconds;
        bool enabled;
        PrepareClock::time_point started;
        PrepareItemTimer(double& value, bool active) : milliseconds(value), enabled(active),
            started(active ? PrepareClock::now() : PrepareClock::time_point{}) {}
        ~PrepareItemTimer()
        {
            if (enabled) milliseconds += std::chrono::duration<double, std::milli>(
                PrepareClock::now()-started).count();
        }
    };
    const auto prepareItemMaterial = [&](FrameRenderItem& item)
    {
        if (!item.material) return;
        { PrepareItemTimer timer(m_lastPrepareTimings.itemValidationMs, m_updateProfilingEnabled);
          item.material->Validate(); }
        { PrepareItemTimer timer(m_lastPrepareTimings.itemTexturesMs, m_updateProfilingEnabled);
          item.material->PrepareTextures(m_graphicsProvider); }
        { PrepareItemTimer timer(m_lastPrepareTimings.itemShadersMs, m_updateProfilingEnabled);
          PrewarmMaterialShaders(*item.material, item.terrainSurface); }
        item.castsShadows = item.material->castsShadows;
        item.blended = item.material->GetAlphaMode() == Engine::Components::MaterialAlphaMode::Blend;
    };
    std::unordered_map<const Engine::Components::Skeleton*,
        std::vector<glm::mat4>> jointSkinMatrices;
    bool boneDataChanged = false;
    for (const FrameComponentLookup& lookup : m_frameComponentLookupScratch)
    {
        Engine::Core::Object* candidate = lookup.object;
        if (!candidate)
            continue;
        Engine::Components::Mesh* mesh = lookup.mesh;
        Engine::Components::Sprite* sprite = lookup.sprite;
        const auto worldRevision=[&] { return preparedTransforms
            ? m_transformHierarchy.ReadPreparedWorldRevision(candidate->transform)
            : candidate->transform.GetWorldRevision(); };
        // Rigid meshes, including resident instances, reuse their base record.
        // Camera-independent geometry is cached; fades and GPU LOD parameters
        // are refreshed below without rebuilding transforms or bounds.
        const bool staticCandidate = mesh && !sprite && !lookup.skinnedMesh &&
            !hasActiveWarpVolume && !m_previewObject &&
            (!mesh->gpuLodSelection || mesh->IsInstanced()) &&
            !mesh->HasMorphTargets() && !mesh->HasSurfaceOffsets() &&
            !mesh->GetPreviousRuntimeLodMesh() &&
            traversalRenderInstances.find(candidate) ==
                traversalRenderInstances.end();
        if (staticCandidate && candidate->IsEnabledInHierarchy() &&
            mesh->IsReady() && mesh->IsEditorVisible() &&
            mesh->IsAnimationVisible())
        {
            auto cached = m_staticRenderItemCache.find(candidate);
            if (cached != m_staticRenderItemCache.end())
            {
                auto& entry = cached->second;
                const auto& layer = candidate->transform.matrixLayer;
                const bool terrainCell =
                    candidate->GetComponent<Engine::Components::Terrain>() != nullptr;
                const auto* terrainBatch = candidate->GetComponent<
                    Engine::Components::TerrainBatchAppearance>();
                if (entry.item.mesh == mesh && entry.item.material == lookup.material &&
                    entry.item.spatialManipulator == lookup.spatialManipulator &&
                    entry.item.bakedLighting == lookup.bakedLighting &&
                    entry.worldRevision == worldRevision() &&
                    entry.meshRevision == mesh->GetConfigurationRevision() &&
                    entry.geometryRevision == mesh->GetAuthoredGeometryRevision() &&
                    entry.instanceData == mesh->GetInstanceData().get() &&
                    entry.instanceRevision == (mesh->GetInstanceData() ? mesh->GetInstanceData()->revision : 0) &&
                    entry.instanceSource == mesh->GetInstanceSource() &&
                    entry.sourceGeometryRevision == (mesh->GetInstanceSource() ? mesh->GetInstanceSource()->GetAuthoredGeometryRevision() : 0) &&
                    entry.item.terrainCell == terrainCell &&
                    entry.item.terrainSurface == (terrainCell || terrainBatch) &&
                    (!terrainBatch || entry.item.completeTerrainFootprint ==
                        (terrainBatch->completeFootprint && !terrainBatch->distantVisual)) &&
                    entry.matrixLayerEnabled == layer.enabled &&
                    (!layer.enabled || std::memcmp(&entry.matrixLayer,
                        &layer.localToLayer, sizeof(layer.localToLayer)) == 0))
                {
                    FrameRenderItem item = entry.item;
                    prepareItemMaterial(item);
                    item.geometryFade = mesh->GetRuntimeGeometryFade();
                    item.shadowCasterFade={std::max(0.f,mesh->runtimeShadowDistance),
                        std::max(0.f,mesh->runtimeShadowFadeWidth)};
                    if (mesh->IsInstanced())
                    {
                        item.gpuMaximumLod=0;
                        item.gpuLodDistances={0,0};
                        if (mesh->gpuLodSelection && candidate!=m_selectedObject &&
                            mesh!=m_editorSelectedMesh && mesh!=m_editorWeightPaintMesh &&
                            mesh->IsRuntimeLodResident(0) && mesh->IsRuntimeLodResident(1) &&
                            (!item.material || IsFastFoliageSurface(item.material) ||
                                std::none_of(item.material->shaders.begin(),item.material->shaders.end(),
                                    [](const auto& shader){return shader.enabled;})))
                        {
                            item.gpuMaximumLod=mesh->IsRuntimeLodResident(2) ? 2:1;
                            item.gpuLodDistances={std::max(0.f,mesh->gpuLod1Distance),
                                std::max(mesh->gpuLod1Distance,mesh->gpuLod2Distance)};
                            if(item.gpuMaximumLod==1) item.gpuLodDistances.y=1.e20f;
                            item.geometryFade={0,item.gpuLodDistances.x,item.gpuLodDistances.y,
                                2.f+mesh->GetRuntimeVisibility()};
                        }
                        ++m_lastPrepareTimings.instancedRenderItemsReused;
                    }
                    else ++m_lastPrepareTimings.staticRenderItemsReused;
                    m_frameRenderItems.push_back(item);
                    continue;
                }
            }
        }
        m_staticRenderItemCache.erase(candidate);
        Engine::Components::Sprite::RenderData spriteData;
        const auto meshStateStarted=m_updateProfilingEnabled ? PrepareClock::now():PrepareClock::time_point{};
        const bool spriteReady = sprite && sprite->IsEditorVisible() &&
            sprite->PrepareRenderData(m_graphicsProvider, spriteData);
        const bool renderable = spriteReady ||
            (mesh && mesh->IsReady() && mesh->IsEditorVisible() &&
                mesh->IsAnimationVisible());
        if(m_updateProfilingEnabled) m_lastPrepareTimings.itemMeshStateMs +=
            std::chrono::duration<double,std::milli>(PrepareClock::now()-meshStateStarted).count();
        if (!candidate->IsEnabledInHierarchy() || !renderable)
            continue;

        FrameRenderItem item{};
        item.object = candidate;
        item.mesh = mesh;
        if(mesh) item.shadowCasterFade={std::max(0.f,mesh->runtimeShadowDistance),
            std::max(0.f,mesh->runtimeShadowFadeWidth)};
        item.sprite = sprite;
        item.material = lookup.material;
        item.spatialManipulator = lookup.spatialManipulator;
        item.skinnedMesh = lookup.skinnedMesh;
        item.bakedLighting = lookup.bakedLighting;
        item.castsShadows = !lookup.material || lookup.material->castsShadows;
        item.terrainCell = candidate->GetComponent<Engine::Components::Terrain>() != nullptr;
        item.terrainSurface = item.terrainCell;
        if (const auto* batch = candidate->GetComponent<Engine::Components::TerrainBatchAppearance>())
        {
            item.terrainSurface = true;
            item.completeTerrainFootprint = batch->completeFootprint && !batch->distantVisual;
        }
        item.belongsToPreview = m_previewObject &&
            IsObjectOrDescendant(candidate, m_previewObject);
        glm::mat4 authoredWorld = preparedTransforms ? m_transformHierarchy.ReadPreparedWorldMatrix(candidate->transform):candidate->transform.GetWorldMatrix();
        if (candidate->transform.matrixLayer.enabled)
        {
            authoredWorld = candidate->transform.matrixLayer.localToLayer *
                authoredWorld;
        }
        item.authoredWorld = authoredWorld;
        // The scene walk above already established that no nonlinear mapping
        // is active. Avoid making every terrain patch independently scan the
        // complete scene for warp volumes on every rendered frame.
        item.world = hasActiveWarpVolume
            ? MapSpatialMatrix(authoredWorld,
                { SpatialQueryDomain::Rendering, candidate })
            : authoredWorld;
        const auto splitInstances = traversalRenderInstances.find(candidate);
        const bool hasSplitRenderInstances = !sprite &&
            splitInstances != traversalRenderInstances.end() &&
            !splitInstances->second.empty();
        if (hasSplitRenderInstances)
        {
            // The first instance is local; each following entry represents the
            // same authored GPU buffer in a connected spatial chart.
            item.world = splitInstances->second.front().world;
            item.traversalClipPlane = splitInstances->second.front().clipPlane;
            item.traversalChartPortal =
                splitInstances->second.front().chartPortal;
        }

        // Map ordinary geometry at its vertices, rather than approximating a
        // nonlinear space with the Jacobian at the object origin.  The latter
        // visibly shears a single mesh that straddles a warp-volume boundary.
        // Portal traversal instances already provide independently clipped
        // source/target chart draws, so retain their specialized path.
        const bool hasSkinnedMesh = lookup.skinnedMesh != nullptr;
        if (hasActiveWarpVolume && mesh && !sprite &&
            !hasSplitRenderInstances && !hasSkinnedMesh &&
            !mesh->GetVertices().empty())
        {
            WarpedRenderMesh& cached = m_warpedRenderMeshes[candidate];
            const uint64_t meshRevision = mesh->GetConfigurationRevision();
            const bool cacheValid = cached.evaluated &&
                cached.meshRevision == meshRevision &&
                cached.warpRevision == warpRevision &&
                std::memcmp(&cached.authoredWorld, &authoredWorld,
                    sizeof(authoredWorld)) == 0;
            if (cacheValid)
            {
                if (cached.affectedByWarp && cached.vertexBuffer)
                {
                    item.warpedVertexBuffer = cached.vertexBuffer.get();
                    item.world = glm::translate(glm::mat4(1.f),
                        cached.mappedOrigin);
                }
            }
            else
            {
            if (!warpSnapshot)
                warpSnapshot.emplace(CaptureSpatialWarpSnapshot());
            const glm::mat3 authoredLinear(authoredWorld);
            const float authoredDeterminant = glm::determinant(authoredLinear);
            const glm::mat3 authoredNormal =
                std::isfinite(authoredDeterminant) &&
                std::abs(authoredDeterminant) > 1e-7f
                    ? glm::transpose(glm::inverse(authoredLinear))
                    : glm::mat3(1.f);
            const SpatialQuerySample originSample = warpSnapshot->Sample(
                glm::vec3(authoredWorld[3]), candidate);
            const auto& sourceVertices = mesh->GetVertices();
            std::vector<Engine::Model::AnimationVertex> warpedVertices(
                sourceVertices.size());
            std::vector<uint8_t> affectedVertices(sourceVertices.size());
            Engine::Rendering::RenderJobs::Get().For(sourceVertices.size(), 256,
                [&](size_t index)
            {
                const auto& sourceVertex = sourceVertices[index];
                Engine::Model::AnimationVertex warpedVertex = sourceVertex;
                const glm::vec3 localPosition(sourceVertex.pos[0],
                    sourceVertex.pos[1], sourceVertex.pos[2]);
                const glm::vec3 worldPosition = glm::vec3(authoredWorld *
                    glm::vec4(localPosition, 1.f));
                const SpatialQuerySample sample = warpSnapshot->Sample(
                    worldPosition, candidate);
                affectedVertices[index] = sample.affectedByWarpVolume ? 1 : 0;

                const glm::vec3 localNormal(sourceVertex.normal[0],
                    sourceVertex.normal[1], sourceVertex.normal[2]);
                const glm::vec3 rawWorldNormal = authoredNormal * localNormal;
                const float jacobianDeterminant = glm::determinant(sample.jacobian);
                const glm::mat3 warpedNormalMatrix =
                    std::isfinite(jacobianDeterminant) &&
                    std::abs(jacobianDeterminant) > 1e-7f
                        ? glm::transpose(glm::inverse(sample.jacobian))
                        : glm::mat3(1.f);
                glm::vec3 warpedNormal = warpedNormalMatrix * rawWorldNormal;
                const float normalLength = glm::length(warpedNormal);
                warpedNormal = normalLength > 1e-6f
                    ? warpedNormal / normalLength : glm::vec3(0.f, 1.f, 0.f);

                const glm::vec3 localTangent(sourceVertex.tangent[0],
                    sourceVertex.tangent[1], sourceVertex.tangent[2]);
                glm::vec3 warpedTangent = sample.jacobian *
                    (authoredLinear * localTangent);
                warpedTangent -= warpedNormal * glm::dot(warpedTangent,
                    warpedNormal);
                const float tangentLength = glm::length(warpedTangent);
                warpedTangent = tangentLength > 1e-6f
                    ? warpedTangent / tangentLength : glm::vec3(1.f, 0.f, 0.f);

                const glm::vec3 relativePosition = sample.point - originSample.point;
                warpedVertex.pos[0] = relativePosition.x;
                warpedVertex.pos[1] = relativePosition.y;
                warpedVertex.pos[2] = relativePosition.z;
                warpedVertex.normal[0] = warpedNormal.x;
                warpedVertex.normal[1] = warpedNormal.y;
                warpedVertex.normal[2] = warpedNormal.z;
                warpedVertex.tangent[0] = warpedTangent.x;
                warpedVertex.tangent[1] = warpedTangent.y;
                warpedVertex.tangent[2] = warpedTangent.z;
                warpedVertices[index] = warpedVertex;
            });
            const bool affectedByWarp = originSample.affectedByWarpVolume ||
                std::any_of(affectedVertices.begin(), affectedVertices.end(),
                    [](uint8_t affected) { return affected != 0; });
            if (affectedByWarp)
            {
                const size_t byteSize = warpedVertices.size() *
                    sizeof(Engine::Model::AnimationVertex);
                const bool topologyChanged = cached.vertices.size() !=
                    warpedVertices.size();
                const bool contentsChanged = topologyChanged || cached.vertices.empty() ||
                    std::memcmp(cached.vertices.data(), warpedVertices.data(),
                        byteSize) != 0;
                if (contentsChanged)
                {
                    if (topologyChanged || !cached.vertexBuffer ||
                        m_deferGraphicsRetirement)
                    {
                        RetireGraphicsBuffer(std::move(cached.vertexBuffer));
                        cached.vertexBuffer = m_graphicsProvider->GetBufferFactory()
                            ->CreateBuffer(Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer,
                                Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                                byteSize, warpedVertices.data());
                    }
                    else if (void* mapped = cached.vertexBuffer->Map())
                    {
                        std::memcpy(mapped, warpedVertices.data(), byteSize);
                        cached.vertexBuffer->Unmap();
                    }
                    cached.vertices = std::move(warpedVertices);
                }
                if (cached.vertexBuffer)
                {
                    item.warpedVertexBuffer = cached.vertexBuffer.get();
                    item.world = glm::translate(glm::mat4(1.f),
                        originSample.point);
                }
            }
            else
            {
                cached.vertexBuffer.reset();
                cached.vertices.clear();
            }
            cached.authoredWorld = authoredWorld;
            cached.mappedOrigin = originSample.point;
            cached.meshRevision = meshRevision;
            cached.warpRevision = warpRevision;
            cached.affectedByWarp = affectedByWarp;
            cached.evaluated = true;
            }
        }

        if (sprite)
        {
            item.spriteVertexBuffer = spriteData.vertexBuffer;
            item.spriteTexture = spriteData.texture;
            item.spriteWorldSize = spriteData.worldSize;
            item.spriteUvRect = spriteData.uvRect;
            item.sortingLayer = sprite->sortingLayer;
            item.blended = true;
        }
        else if (item.material)
        {
            prepareItemMaterial(item);
        }
        item.blended = item.belongsToPreview || item.blended;

        // Pack complete palettes by their actual size, including large rigs.
        {
            if (mesh && !mesh->IsPortalPoseSnapshot())
            if (Engine::Components::SkinnedMesh* skinned = lookup.skinnedMesh)
            {
                const auto* skeleton = skinned->ResolveSkeleton();
                const std::vector<glm::mat4>* sharedJoints = nullptr;
                if (skeleton)
                {
                    auto [found, inserted] = jointSkinMatrices.try_emplace(skeleton);
                    if (inserted)
                    {
                        const auto& joints = skeleton->ResolveJoints();
                        auto& matrices = found->second;
                        matrices.assign(skeleton->jointNodes.size(), glm::mat4(1.f));
                        for (size_t i = 0; i < std::min(joints.size(),
                                matrices.size()); ++i)
                            if (auto* joint = joints[i])
                                matrices[i] = (preparedTransforms ? m_transformHierarchy.ReadPreparedWorldMatrix(joint->transform):joint->transform.GetWorldMatrix()) *
                                    (i < skeleton->inverseBindMatrices.size()
                                        ? skeleton->inverseBindMatrices[i]
                                        : glm::mat4(1.f));
                    }
                    sharedJoints = &found->second;
                }
                const std::vector<glm::mat4>& palette =
                    sharedJoints ? skinned->BuildPalette(*sharedJoints) :
                        skinned->BuildPalette();
                if (!palette.empty())
                {
                    const size_t count = palette.size();
                    if (count > m_boneMatrixCapacity - boneMatrixOffset)
                        throw std::runtime_error("Bone palette capacity changed during frame preparation");
                    item.skinPaletteOffset = boneMatrixOffset;
                    item.skinJointCount = static_cast<uint32_t>(count);
                    std::memcpy(
                        static_cast<glm::mat4*>(m_boneDataMapped) +
                            item.skinPaletteOffset,
                        palette.data(), count * sizeof(glm::mat4));
                    ++skinPaletteSlot;
                    boneMatrixOffset += static_cast<uint32_t>(count);
                    boneDataChanged = true;
                }
            }
        }

        const auto appendGeometry = [&](FrameRenderItem draw) {
            PrepareItemTimer lodTimer(m_lastPrepareTimings.itemLodMs, m_updateProfilingEnabled);
            Engine::Components::Mesh* source = draw.mesh;
            const auto publish = [&](FrameRenderItem& current) {
                PrepareItemTimer boundsTimer(m_lastPrepareTimings.itemBoundsMs, m_updateProfilingEnabled);
                current.hasBounds = current.mesh && current.mesh->HasBounds();
                current.hasMorphTargets = current.mesh && current.mesh->HasMorphTargets();
                current.hasSurfaceOffsets = current.mesh && current.mesh->HasSurfaceOffsets();
                if (current.hasBounds)
                {
                    current.boundsMinimum = current.mesh->GetBoundsMin();
                    current.boundsMaximum = current.mesh->GetBoundsMax();
                }
                m_frameRenderItems.push_back(current);
            };
            if (source && !draw.warpedVertexBuffer)
                draw.geometryFade = source->GetRuntimeGeometryFade();
            if(source && source->IsInstanced() && source->gpuLodSelection &&
                draw.object!=m_selectedObject && source!=m_editorSelectedMesh && source!=m_editorWeightPaintMesh &&
                !draw.warpedVertexBuffer && !draw.traversalChartPortal && source->IsRuntimeLodResident(0) && source->IsRuntimeLodResident(1) &&
                (!draw.material || IsFastFoliageSurface(draw.material) ||
                    std::none_of(draw.material->shaders.begin(),draw.material->shaders.end(),[](const auto& shader){return shader.enabled;})))
            {
                draw.gpuMaximumLod=source->IsRuntimeLodResident(2) ? 2:1;
                draw.gpuLodDistances={std::max(0.f,source->gpuLod1Distance),std::max(source->gpuLod1Distance,source->gpuLod2Distance)};
                if(draw.gpuMaximumLod==1) draw.gpuLodDistances.y=1.e20f;
                draw.geometryFade={0,draw.gpuLodDistances.x,draw.gpuLodDistances.y,2.f+source->GetRuntimeVisibility()};
                publish(draw);
                return;
            }
            if (source && source->gpuLodSelection && draw.object!=m_selectedObject &&
                source!=m_editorSelectedMesh && source!=m_editorWeightPaintMesh && !source->IsInstanced() &&
                !source->HasMorphTargets() && !source->HasSurfaceOffsets() && !draw.skinJointCount &&
                !draw.warpedVertexBuffer && !draw.traversalChartPortal && source->IsRuntimeLodResident(0) &&
                source->IsRuntimeLodResident(1))
            {
                const int maximumLod = source->IsRuntimeLodResident(2) ? 2 : 1;
                for (int lod=0;lod<=maximumLod;++lod)
                {
                    draw.mesh=source->GetRuntimeLodMesh(lod);
                    draw.mesh->useLightProbes=source->useLightProbes;
                    draw.mesh->grassShadowCaster=source->grassShadowCaster;
                    draw.mesh->useBoundsForShadowCulling=source->useBoundsForShadowCulling;
                    draw.gpuLod=lod; draw.gpuMaximumLod=maximumLod;
                    draw.gpuLodDistances={std::max(0.f,source->gpuLod1Distance),
                        std::max(source->gpuLod1Distance,source->gpuLod2Distance)};
                    if(source->IsInstanced() && maximumLod==1) draw.gpuLodDistances.y=1.e20f;
                    if(source->IsInstanced()) draw.geometryFade={float(lod),draw.gpuLodDistances.x,draw.gpuLodDistances.y,2.f+source->GetRuntimeVisibility()};
                    publish(draw);
                }
                return;
            }
            publish(draw);
            if (source && !draw.warpedVertexBuffer)
                if (auto* previous = source->GetPreviousRuntimeLodMesh())
                {
                    draw.mesh = previous;
                    draw.previousLod=true;
                    previous->grassShadowCaster=source->grassShadowCaster;
                    draw.geometryFade = source->GetRuntimeGeometryFade(true);
                    publish(draw);
                }
        };
        const size_t firstDraw = m_frameRenderItems.size();
        appendGeometry(item);
        if (hasSplitRenderInstances)
        {
            for (size_t index = 1u; index < splitInstances->second.size(); ++index)
            {
                FrameRenderItem remoteItem = item;
                remoteItem.world = splitInstances->second[index].world;
                remoteItem.traversalClipPlane = splitInstances->second[index].clipPlane;
                remoteItem.traversalChartPortal =
                    splitInstances->second[index].chartPortal;
                appendGeometry(remoteItem);
            }
        }
        if (staticCandidate && m_frameRenderItems.size() == firstDraw + 1 &&
            !item.warpedVertexBuffer && !hasSplitRenderInstances &&
            !mesh->HasMorphTargets() &&
            !mesh->HasSurfaceOffsets())
        {
            const auto& layer = candidate->transform.matrixLayer;
            m_staticRenderItemCache[candidate] = {
                m_frameRenderItems.back(), worldRevision(),
                mesh->GetConfigurationRevision(), mesh->GetAuthoredGeometryRevision(),
                layer.enabled, layer.localToLayer,
                mesh->GetInstanceData().get(), mesh->GetInstanceData() ? mesh->GetInstanceData()->revision : 0,
                mesh->GetInstanceSource(), mesh->GetInstanceSource() ? mesh->GetInstanceSource()->GetAuthoredGeometryRevision() : 0 };
            if(mesh->IsInstanced()) ++m_lastPrepareTimings.instancedRenderItemsUpdated;
            else ++m_lastPrepareTimings.staticRenderItemsUpdated;
        }
    }

    const auto afterItems = m_updateProfilingEnabled
        ? PrepareClock::now() : PrepareClock::time_point{};
    if (boneDataChanged)
        m_boneDataBuffer->FlushMappedWrites();
    const auto afterBoneUpload = m_updateProfilingEnabled
        ? PrepareClock::now() : PrepareClock::time_point{};
    m_lastSkinnedObjectCount = skinPaletteSlot;
    if (m_frameRenderItems.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Prepared draw count exceeds renderer index range");
    if(m_renderObjectSlotsRevision!=GetStructureRevision())
    {
        std::unordered_set<uintptr_t> owners,charts;
        for(const auto& object:m_objects) owners.insert(uintptr_t(object.get()));
        for(auto* chart:m_frameSpatialManipulatorScratch) charts.insert(uintptr_t(chart));
        for(auto it=m_renderObjectSlots.begin();it!=m_renderObjectSlots.end();)
            if(!owners.count(std::get<0>(it->first)) || (std::get<2>(it->first) && !charts.count(std::get<2>(it->first))))
            { m_freeRenderObjectSlots.push_back(it->second); it=m_renderObjectSlots.erase(it); }
            else ++it;
        m_renderObjectSlotsRevision=GetStructureRevision();
    }
    for(auto& item:m_frameRenderItems)
    {
        RenderObjectKey key{uintptr_t(item.object),item.gpuLod,uintptr_t(item.traversalChartPortal),item.previousLod};
        auto found=m_renderObjectSlots.find(key);
        if(found==m_renderObjectSlots.end())
        {
            uint32_t index=m_renderObjectSlotCount;
            if(m_freeRenderObjectSlots.empty()) ++m_renderObjectSlotCount;
            else { index=m_freeRenderObjectSlots.back(); m_freeRenderObjectSlots.pop_back(); }
            found=m_renderObjectSlots.emplace(key,index).first;
        }
        item.objectSlot=found->second;
    }
    EnsureObjectRenderCapacity(m_renderObjectSlotCount);
    if (m_updateProfilingEnabled)
    {
        const auto ms = [](auto first, auto last)
        { return std::chrono::duration<double, std::milli>(last - first).count(); };
        m_lastPrepareTimings.lookupMs=ms(afterTransforms, afterLookup);
        m_lastPrepareTimings.itemsMs=ms(afterLookup, afterItems);
        auto& detail=m_lastPrepareTimings;
        detail.itemLodMs=std::max(0.0, detail.itemLodMs-detail.itemBoundsMs);
        detail.itemBuildMs=std::max(0.0, detail.itemsMs-detail.itemValidationMs-
            detail.itemTexturesMs-detail.itemShadersMs-detail.itemMeshStateMs-
            detail.itemBoundsMs-detail.itemLodMs);
        m_lastPrepareTimings.boneUploadMs=ms(afterItems, afterBoneUpload);
        m_lastPrepareTimings.capacityMs=ms(afterBoneUpload, PrepareClock::now());
    }
    m_lastPrepareTimings.transformsMs=transforms.totalMs+transformGatherMs;
    m_lastPrepareTimings.transformSnapshotMs=transforms.snapshotMs+transformGatherMs;
    m_lastPrepareTimings.transformComputeMs=transforms.computeMs;
    m_lastPrepareTimings.transformCommitMs=transforms.commitMs;
    m_lastPrepareTimings.transformLocalUpdated=transforms.localUpdated;
    m_lastPrepareTimings.transformWorldUpdated=transforms.worldUpdated;
    m_lastPrepareTimings.transformWorldReused=transforms.worldReused;
    m_lastPrepareTimings.transformLevels=transforms.levels;
    m_lastPrepareTimings.transformStructureFallback=preparedTransforms ? 0:1;
    const auto spatialStart=PrepareClock::now();
    std::vector<Engine::Rendering::RenderBounds> spatialBounds;
    std::vector<Engine::Rendering::RenderBoundsCache::Input> boundsInput;
    boundsInput.resize(m_frameRenderItems.size());
    Engine::Rendering::RenderJobs::Get().For(m_frameRenderItems.size(),128,[&](size_t index)
    {
        const auto& item = m_frameRenderItems[index];
        Engine::Rendering::RenderBoundsCache::Input input;
        input.slot=item.objectSlot; input.world=item.world;
        if (item.hasBounds && !item.skinJointCount &&
            !item.hasMorphTargets && !item.hasSurfaceOffsets &&
            !item.warpedVertexBuffer && !item.traversalChartPortal)
        { input.bounded=true; input.minimum=item.boundsMinimum; input.maximum=item.boundsMaximum; }
        boundsInput[index]=input;
    });
    const auto bounds=m_worldBoundsCache.Prepare(boundsInput,spatialBounds);
    m_lastPrepareTimings.boundsReused=bounds.reused; m_lastPrepareTimings.boundsUpdated=bounds.updated;
    m_spatialSlotBounds.resize(m_renderObjectSlotCount);
    m_spatialSlotSeen.assign(m_renderObjectSlotCount,0);
    for(size_t index=0;index<m_frameRenderItems.size();++index)
    {
        const auto slot=m_frameRenderItems[index].objectSlot;
        const auto& current=spatialBounds[index];
        if(!m_spatialSlotSeen[slot])
        { m_spatialSlotBounds[slot]=current; m_spatialSlotSeen[slot]=1; }
        else
        {
            auto& aggregate=m_spatialSlotBounds[slot];
            // Several chart instances can share a draw slot. A single
            // unbounded instance must keep all of them conservatively visible.
            if(!aggregate.bounded || !current.bounded) aggregate.bounded=false;
            else
            {
                aggregate.minimum=glm::min(aggregate.minimum,current.minimum);
                aggregate.maximum=glm::max(aggregate.maximum,current.maximum);
            }
        }
    }
    // Retain the last bound for absent slots. A temporary LOD or visibility
    // change then leaves the tree topology intact; no draw consumes that slot.
    m_lastPrepareTimings.spatialActiveSlots=m_frameRenderItems.size();
    m_lastPrepareTimings.spatialRetainedSlots=m_spatialSlotBounds.size();
    const auto spatial=m_renderSpatialIndex.Update(m_spatialSlotBounds);
    m_lastPrepareTimings.spatialIndexMs=spatial.totalMs; m_lastPrepareTimings.spatialCompareMs=spatial.compareMs;
    m_lastPrepareTimings.spatialRefitMs=spatial.refitMs; m_lastPrepareTimings.spatialBuildMs=spatial.buildMs;
    m_lastPrepareTimings.spatialChangedBounds=spatial.changedBounds; m_lastPrepareTimings.spatialReusedBounds=spatial.reusedBounds;
    m_lastPrepareTimings.spatialDirtyLeaves=spatial.dirtyLeaves; m_lastPrepareTimings.spatialRefitNodes=spatial.refitNodes;
    m_lastPrepareTimings.spatialRebuilds=spatial.rebuilds; m_lastPrepareTimings.spatialQualityRebuilds=spatial.qualityRebuilds;
    m_lastPrepareTimings.spatialMs=std::chrono::duration<double,std::milli>(PrepareClock::now()-spatialStart).count();
    const auto completedJobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
    m_lastPrepareTimings.workerMs=double(completedJobs.workerNs-preparationJobs.workerNs)/1.e6;
    m_lastPrepareTimings.mainJobMs=double(completedJobs.callerNs-preparationJobs.callerNs)/1.e6;
    m_lastPrepareTimings.workerWaitMs=double(completedJobs.waitNs-preparationJobs.waitNs)/1.e6;
    m_lastPrepareTimings.workerQueueMs=double(completedJobs.queueNs-preparationJobs.queueNs)/1.e6;
    m_renderFramePrepared = true;
}

// ---------------------------------------------------------------------------
// Scene::Render
// ---------------------------------------------------------------------------

void Scene::Render(Engine::Graphics::IGraphicsContext* context, float aspect,
    Engine::Components::Camera* cameraOverride, bool includeEditorVisuals,
    uint32_t viewportWidth, uint32_t viewportHeight, bool shadowOnly,
    const NativeRenderPacket* nativePacket)
{
    const bool previousNativeRecording = m_recordingNativeCommands;
    m_recordingNativeCommands = nativePacket && IsOwnerThread();
    struct NativeRecordingGuard
    {
        bool& current;
        bool previous;
        ~NativeRecordingGuard() { current = previous; }
    } nativeRecordingGuard{m_recordingNativeCommands, previousNativeRecording};
    struct NativePass
    {
        Engine::Graphics::IGraphicsContext* context;
        Engine::Graphics::NativePhase previous;
        ~NativePass() { if(context) context->SetNativeTimingPhase(previous); }
    } nativePass{context,context ? context->GetNativeTimingPhase():Engine::Graphics::NativePhase::Other};
    if(context) context->SetNativeTimingPhase(shadowOnly ? Engine::Graphics::NativePhase::Shadow:Engine::Graphics::NativePhase::Camera);
    using RenderClock = std::chrono::steady_clock;
    m_lastRenderCpuTimings={};
    struct RenderMeasurement
    {
        RenderCpuTimings& metrics;
        const uint32_t& draws; const uint64_t& triangles;
        Engine::Rendering::RenderJobs::Statistics jobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
        RenderClock::time_point start=RenderClock::now();
        ~RenderMeasurement()
        {
            metrics.totalMs=std::chrono::duration<double,std::milli>(RenderClock::now()-start).count();
            const auto endJobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
            metrics.workerActiveMs=double(endJobs.workerNs-jobs.workerNs)/1.e6;
            metrics.mainJobMs=double(endJobs.callerNs-jobs.callerNs)/1.e6;
            metrics.workerWaitMs=double(endJobs.waitNs-jobs.waitNs)/1.e6;
            metrics.workerQueueMs=double(endJobs.queueNs-jobs.queueNs)/1.e6;
            metrics.workerDependencyMs=double(endJobs.dependencyNs-jobs.dependencyNs)/1.e6;
            metrics.configuredWorkers=endJobs.workers;
            metrics.workerJobs=endJobs.jobs-jobs.jobs; metrics.workerTasks=endJobs.workerTasks-jobs.workerTasks;
            metrics.otherMs=std::max(0.,metrics.totalMs-metrics.startupMs-metrics.terrainPreparationMs-
                metrics.cullingMs-metrics.sortingMs-metrics.occlusionMs-metrics.materialMs-
                metrics.batchingMs-metrics.uploadMs-metrics.submissionMs-metrics.retainedUiMs-metrics.portalMs);
            metrics.physicalDraws=draws; metrics.candidateTriangles=triangles; metrics.views=1;
        }
    } measurement{m_lastRenderCpuTimings,m_lastOrdinaryDrawCount,m_lastOrdinaryTriangleCount};
    const auto shadowCpuStart = std::chrono::steady_clock::now();
    if (shadowOnly)
        m_lastShadowCpuTiming = {};
    const auto shadowElapsedMs = [](auto start, auto end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    m_lastObjectDataUploadBytes = 0;
    m_lastOcclusionCulledCount = 0;
    m_lastTerrainOcclusionCulledCount = 0;
    m_lastParticlePortalDrawCount = 0;
    if (shadowOnly) m_shadowDebugSnapshot = {};
    else
    {
        m_shadowDebugSnapshot.bakedReceiverCount = 0;
        m_shadowDebugSnapshot.dynamicReceiverCount = 0;
    }
    if (!context)
    {
        return;
    }

    if (!m_graphicsProvider || !m_objectPipeline)
    {
        return;
    }

    if (!shadowOnly)
        Engine::Graphics::SetPostProcessSettings({
            std::exp2(std::clamp(settings.outputExposure, -16.f, 16.f)),
            static_cast<uint32_t>(settings.toneMapping) });

    if (m_nativeRenderPacketTaken && !nativePacket)
        throw std::logic_error("Native frame draw list was transferred to a render packet");
    if (!m_renderFramePrepared)
        PrepareRenderFrame();
    const auto& frameRenderItems = nativePacket ? nativePacket->items : m_frameRenderItems;
    uint32_t requiredObjectSlots=1;
    for(const auto& item:frameRenderItems)
    {
        if(item.objectSlot==UINT32_MAX)
            throw std::runtime_error("Render object slot exceeds buffer index range");
        requiredObjectSlots=std::max(requiredObjectSlots,item.objectSlot+1u);
    }
    // Persistent slots can be sparse after LOD/visibility changes. Size the
    // selected frame/view buffers by their highest addressed slot, not draw count.
    SelectRenderPassBuffers(requiredObjectSlots);
    // Validate on the owner before record-packing/cascade jobs can touch maps.
    const uint64_t shadowRecords=uint64_t(m_objectCapacity)*4u;
    if(requiredObjectSlots>m_objectCapacity || !m_objectDataMapped || !m_objectCBMapped ||
        !m_shadowObjectDataMapped || !m_shadowCBMapped ||
        m_objectDataBuffer->GetSize()<uint64_t(requiredObjectSlots)*sizeof(ObjectGPUData) ||
        m_objectConstantBuffer->GetSize()<uint64_t(requiredObjectSlots)*kCBStride ||
        m_shadowObjectDataBuffer->GetSize()<shadowRecords*sizeof(ObjectGPUData) ||
        m_shadowConstantBuffer->GetSize()<shadowRecords*kCBStride)
        throw std::runtime_error("Render pass upload buffers do not cover persistent object slots");
    m_lastOrdinaryDrawCount = 0;
    m_lastOrdinaryTriangleCount = 0;
    const uint32_t portalRenderSlotsPerView = m_objectCapacity + 2u;
    const uint32_t portalObjectSlotCount =
        kMaxPortalRenderViews * portalRenderSlotsPerView;

    // Scene View always uses its navigation camera. Game View supplies its
    // active scene camera explicitly, so hierarchy selection cannot hijack
    // either viewport.
    Engine::Components::Camera* cam = cameraOverride
        ? cameraOverride
        : editorCamera.GetComponent<Engine::Components::Camera>();
    if (!cam)
    {
        return;
    }

    const auto instanceSerial=m_graphicsProvider->GetContextFactory()->GetPreparedFrameSerial();
    const bool newInstanceFrame=m_instanceViewSerial!=instanceSerial;
    if(m_instanceViewSerial!=instanceSerial) { m_instanceViewSerial=instanceSerial; m_instanceViewUses.clear(); }
    const uint32_t instanceViewOrdinal=m_instanceViewUses[{cam,shadowOnly}]++;
    m_persistentInstances.BeginFrame(m_graphicsProvider);
    const auto poolBefore=m_persistentInstances.stats;
    m_lastRenderCpuTimings.instances.retirements=newInstanceFrame ? poolBefore.retirements:0;
    m_lastRenderCpuTimings.instances.residentBytes=m_persistentInstances.ResidentBytes();
    glm::mat4 authoredCameraWorld(1.f);
    bool hasAuthoredCameraWorld = false;
    bool cameraInsideInteriorOnlyWarp = false;
    if (cam->Owner)
    {
        authoredCameraWorld = cam->Owner->transform.GetWorldMatrix();
        if (cam->Owner->transform.matrixLayer.enabled)
        {
            authoredCameraWorld = cam->Owner->transform.matrixLayer.localToLayer *
                authoredCameraWorld;
        }
        hasAuthoredCameraWorld = true;
        const glm::vec3 authoredCameraPosition(authoredCameraWorld[3]);
        for(const auto* volume:m_frameSpatialManipulatorScratch)
            if(volume && volume->Owner && volume->Owner->IsEnabledInHierarchy() &&
                volume->enabled && volume->definesWarpVolume && volume->renderWarpInteriorOnly &&
                volume->ContainsWorldPoint(authoredCameraPosition))
            { cameraInsideInteriorOnlyWarp=true; break; }

    }

    glm::mat4 view = cam->GetViewMatrix();
    // Camera::GetViewMatrix uses the source chart for a camera already inside
    // a warp volume. The editor needs an explicit opt-in for that behavior so
    // its default view remains an accurate inspection of the embedded space.
    if (((!cameraOverride && !settings.sceneCameraWarpLookThrough) ||
        cameraInsideInteriorOnlyWarp) && !m_editorMode2D && cam->Owner)
    {
        glm::mat4 embeddedWorld = authoredCameraWorld;
        embeddedWorld = MapSpatialMatrix(embeddedWorld,
            { SpatialQueryDomain::Camera, cam->Owner });
        const glm::vec3 eye = glm::vec3(embeddedWorld[3]);
        if (!cam->useTransformRotation)
        {
            const glm::vec3 target = MapSpatialPoint(cam->target,
                { SpatialQueryDomain::Camera });
            view = glm::lookAtLH(eye, target, cam->up);
        }
        else
        {
            const glm::vec3 forward = glm::normalize(glm::vec3(embeddedWorld[2]));
            const glm::vec3 up = glm::normalize(glm::vec3(embeddedWorld[1]));
            view = glm::lookAtLH(eye, eye + forward, up);
        }
    }
    if (m_editorMode2D && cam->Owner)
    {
        const glm::vec3 cameraWorld = cam->Owner->transform.GetWorldPosition();
        const glm::vec3 eye(cameraWorld.x, cameraWorld.y, -10.f);
        view = glm::lookAtLH(eye, eye + glm::vec3(0.f, 0.f, 1.f),
            glm::vec3(0.f, 1.f, 0.f));
    }
    const glm::mat4 proj = cam->GetProjectionMatrix(aspect, m_editorMode2D);
    const glm::vec3 cameraPosition = glm::vec3(glm::inverse(view)[3]);
    bool cameraUsesSourceWarpChart = false;
    if (hasAuthoredCameraWorld)
    {
        const bool sourceChartAllowed = cameraOverride ||
            settings.sceneCameraWarpLookThrough;
        cameraUsesSourceWarpChart = !cameraInsideInteriorOnlyWarp &&
            sourceChartAllowed && SampleSpatialPoint(
            glm::vec3(authoredCameraWorld[3]),
            { SpatialQueryDomain::Camera, cam->Owner }).affectedByWarpVolume;

        // A normal volume uses the source chart for game-camera look-through
        // (and for opted-in editor views). An interior-only volume also uses
        // physical geometry from every outside view, including Scene view,
        // then switches to its compensated chart once the camera enters.
        if (!cameraUsesSourceWarpChart && !cameraInsideInteriorOnlyWarp &&
            !m_editorMode2D)
        {
            const auto volumeIntersectsView = [&](const Engine::Components::SpatialManipulator& volume)
            {
                if (!volume.Owner || !volume.definesWarpVolume || !volume.enabled)
                    return false;
                const auto shape = static_cast<Engine::Components::SpatialManipulator::
                    WarpVolumeShape>(volume.warpVolumeShape);
                if (shape == Engine::Components::SpatialManipulator::WarpVolumeShape::Infinite)
                    return true;
                // Chart selection used to test only the center camera ray.
                // A volume entering from a side of the viewport consequently
                // stayed in the wrong chart until it crossed screen center.
                // Conservatively test the volume's local bounds against all
                // six homogeneous frustum planes instead. False positives are
                // harmless (they retain the physical chart slightly longer),
                // while false negatives cause visible chart popping.
                const float radius = std::max(0.001f,
                    std::abs(volume.warpVolumeRadius));
                const glm::vec3 halfSize = shape == Engine::Components::
                        SpatialManipulator::WarpVolumeShape::Sphere
                    ? glm::vec3(radius)
                    : glm::max(glm::abs(volume.warpVolumeSize) * 0.5f,
                        glm::vec3(0.0001f));
                const glm::mat4 localToClip = proj * view *
                    volume.Owner->transform.GetWorldMatrix();
                std::array<glm::vec4, 8> corners{};
                size_t cornerIndex = 0;
                for (int z = 0; z < 2; ++z)
                    for (int y = 0; y < 2; ++y)
                        for (int x = 0; x < 2; ++x)
                            corners[cornerIndex++] = localToClip * glm::vec4(
                                x ? halfSize.x : -halfSize.x,
                                y ? halfSize.y : -halfSize.y,
                                z ? halfSize.z : -halfSize.z, 1.f);
                const auto allOutside = [&](const auto& predicate)
                {
                    return std::all_of(corners.begin(), corners.end(), predicate);
                };
                return !(allOutside([](const glm::vec4& point)
                        { return point.x < -point.w; }) ||
                    allOutside([](const glm::vec4& point)
                        { return point.x > point.w; }) ||
                    allOutside([](const glm::vec4& point)
                        { return point.y < -point.w; }) ||
                    allOutside([](const glm::vec4& point)
                        { return point.y > point.w; }) ||
                    allOutside([](const glm::vec4& point)
                        { return point.z < 0.f; }) ||
                    allOutside([](const glm::vec4& point)
                        { return point.z > point.w; }));
            };
            for(const auto* volume:m_frameSpatialManipulatorScratch)
                if(volume && volume->Owner && volume->Owner->IsEnabledInHierarchy() &&
                    (sourceChartAllowed || volume->renderWarpInteriorOnly) && volumeIntersectsView(*volume))
                { cameraUsesSourceWarpChart=true; break; }

        }
    }
    // A source-chart view continues through a finite nonlinear volume instead
    // of projecting its already embedded bend with a straight raster camera.
    // Editor views stay embedded unless Spatial Debug explicitly enables the
    // Scene Camera Warp Look-Through option.
    const auto sourceChartWorld = [](const FrameRenderItem& item)
    {
        return item.authoredWorld;
    };
    const auto useSourceChartMesh = [&](const FrameRenderItem& item)
    {
        return cameraUsesSourceWarpChart && item.mesh && !item.sprite &&
            glm::all(glm::equal(item.traversalClipPlane, glm::vec4(0.f)));
    };
    // Light data is view-chart-specific. Rebuild it for every view so an
    // inside-volume game camera cannot leave source-chart lights bound when
    // the editor view subsequently renders the embedded chart (or vice versa).
    Engine::Model::RealtimeShadowSelection shadowSelection{};
    std::array<std::string, 4> cookiePaths{};
    std::array<std::string, 4> iesPaths{};
    m_frameLightCount = m_realtimeLightingPipeline.CollectLights(*this,
        static_cast<Engine::Model::LightData*>(m_lightDataMapped),
        std::min(kMaxLights,
            m_distanceLightingSettings.maximumRealtimeLights),
        !cameraUsesSourceWarpChart, &m_realtimeShadowSettings,
        &shadowSelection, &cameraPosition, &cookiePaths, &iesPaths);
    if (shadowOnly && !shadowSelection.valid)
    {
        m_shadowDebugSnapshot.enabled = m_realtimeShadowSettings.enabled;
        m_lastShadowCpuTiming.scenePreparationMs = shadowElapsedMs(
            shadowCpuStart, std::chrono::steady_clock::now());
        m_lastShadowCpuTiming.totalMs =
            m_lastShadowCpuTiming.scenePreparationMs;
        return;
    }
    // Terrain needs one scene-wide sun regardless of object distance. Select
    // it once per view instead of walking the light list for every pixel.
    glm::vec3 terrainSunDirection(0.f, 1.f, 0.f);
    glm::vec3 terrainSunRadiance(0.f);
    int terrainSunIndex = -1;
    const auto* frameLights = static_cast<const Engine::Model::LightData*>(
        m_lightDataMapped);
    for (uint32_t index = 0; !shadowOnly && index < m_frameLightCount; ++index)
    {
        const auto& light = frameLights[index];
        if (std::abs(light.params.y - 1.f) >= 0.25f) continue;
        const glm::vec3 radiance = glm::vec3(light.colorIntensity) *
            light.colorIntensity.w;
        if (terrainSunIndex >= 0 &&
            glm::length(radiance) <= glm::length(terrainSunRadiance)) continue;
        terrainSunDirection = glm::normalize(glm::vec3(light.positionRange));
        terrainSunRadiance = radiance;
        terrainSunIndex = static_cast<int>(index);
    }
    std::array<std::shared_ptr<Engine::Components::Texture>, 8> photometryTextures{};
    for (size_t index = 0; !shadowOnly && index < cookiePaths.size(); ++index)
    {
        if (!cookiePaths[index].empty())
        {
            photometryTextures[index] = Engine::Components::Texture::Acquire(
                cookiePaths[index], false);
            photometryTextures[index]->Prepare(m_graphicsProvider);
        }
        if (!iesPaths[index].empty())
        {
            photometryTextures[index + 4u] = Engine::Components::Texture::Acquire(
                iesPaths[index], false);
            photometryTextures[index + 4u]->Prepare(m_graphicsProvider);
        }
    }
    for (uint32_t index = 0;
        !shadowOnly && index < photometryTextures.size(); ++index)
        context->SetTexture(16u + index, photometryTextures[index]
            ? photometryTextures[index]->GetGraphicsTexture() : nullptr);
    m_shadowDebugSnapshot.enabled = m_realtimeShadowSettings.enabled;

    const uint32_t clusterTileSize = std::clamp(
        m_distanceLightingSettings.clusterTileSize, 16u, 256u);
    const uint32_t clusterTilesX = viewportWidth > 0u
        ? (viewportWidth + clusterTileSize - 1u) / clusterTileSize : 0u;
    const uint32_t clusterTilesY = viewportHeight > 0u
        ? (viewportHeight + clusterTileSize - 1u) / clusterTileSize : 0u;
    const uint32_t clusterDepthSlices = std::clamp(
        m_distanceLightingSettings.clusterDepthSlices, 1u, 64u);
    const uint32_t maximumLightsPerCluster = std::clamp(
        m_distanceLightingSettings.maximumLightsPerCluster, 1u,
        Engine::Model::MaxRealtimeLights);
    const uint64_t clusterCount64 = static_cast<uint64_t>(clusterTilesX) *
        clusterTilesY * clusterDepthSlices;
    const bool clusteredLightingEnabled = !shadowOnly &&
        m_distanceLightingSettings.clusteredLighting &&
        m_frameLightCount > 0u && clusterCount64 > 0u &&
        clusterCount64 <= std::numeric_limits<uint32_t>::max();
    const float clusterNear = std::max(0.001f, cam->nearPlane);
    const float clusterFar = std::max(clusterNear + 0.001f, cam->farPlane);
    const float clusterLogScale = static_cast<float>(clusterDepthSlices) /
        std::max(std::log2(clusterFar / clusterNear), 0.0001f);
    if (clusteredLightingEnabled)
    {
        const auto clusterStart=RenderClock::now();
        const uint32_t clusterCount = static_cast<uint32_t>(clusterCount64);
        const uint64_t indexCount64 = static_cast<uint64_t>(clusterCount) *
            maximumLightsPerCluster;
        if (indexCount64 > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Forward+ cluster index capacity overflow");
        const uint32_t indexCount = static_cast<uint32_t>(indexCount64);
        auto* bufferFactory = m_graphicsProvider->GetBufferFactory();
        const auto ensureClusterBuffer = [&](std::unique_ptr<IGraphicsBuffer>& buffer,
            void*& mapped, uint32_t& capacity, uint32_t required,
            uint32_t stride, const char* description)
        {
            if (capacity >= required && buffer && mapped)
                return;
            const uint32_t expanded = ExpandedCapacity(capacity, required);
            auto replacement = bufferFactory->CreateBuffer(
                Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
                Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                static_cast<uint64_t>(expanded) * stride, nullptr, stride);
            void* replacementMapped = replacement ? replacement->Map() : nullptr;
            if (!replacementMapped)
                throw std::runtime_error(std::string("Failed to allocate ") +
                    description);
            if (buffer && mapped)
                buffer->Unmap();
            if (buffer)
                m_retiredRenderBuffers.push_back(std::move(buffer));
            buffer = std::move(replacement);
            mapped = replacementMapped;
            capacity = expanded;
            m_directionalClusterLayouts.clear();
        };
        ensureClusterBuffer(m_clusterHeaderBuffer, m_clusterHeaderMapped,
            m_clusterHeaderCapacity, clusterCount, sizeof(glm::uvec2),
            "Forward+ cluster headers");
        ensureClusterBuffer(m_clusterLightIndexBuffer, m_clusterLightIndexMapped,
            m_clusterLightIndexCapacity, indexCount, sizeof(uint32_t),
            "Forward+ cluster light indices");

        const auto bufferKey=std::make_pair(static_cast<const void*>(m_clusterHeaderBuffer.get()),
            static_cast<const void*>(m_clusterLightIndexBuffer.get()));
        const std::array<uint32_t,5> layout{clusterTilesX,clusterTilesY,clusterDepthSlices,
            maximumLightsPerCluster,m_frameLightCount};
        const bool directionalOnly=std::all_of(frameLights,frameLights+m_frameLightCount,
            [](const auto& light) { return std::abs(light.params.y-1.f)<.25f; });
        const auto cached=m_directionalClusterLayouts.find(bufferKey);
        const bool reuse=directionalOnly && cached!=m_directionalClusterLayouts.end() && cached->second==layout;
        if(reuse) ++m_lastRenderCpuTimings.clusterCacheHits;
        else
        {
            // A mixed-light frame overwrites these buffers. Invalidate before the
            // writes so returning to directional-only lighting rebuilds the grid.
            m_directionalClusterLayouts.erase(bufferKey);

            m_clusterHeaderScratch.assign(clusterCount, glm::uvec2(0u));
            m_clusterLightIndexScratch.assign(indexCount, 0u);
            std::vector<glm::uvec2>& headers = m_clusterHeaderScratch;
            std::vector<uint32_t>& indices = m_clusterLightIndexScratch;
            for (uint32_t cluster = 0; cluster < clusterCount; ++cluster)
                headers[cluster].x = cluster * maximumLightsPerCluster;
            const auto depthSlice = [&](float depth)
            {
                const float normalized = std::log2(std::max(depth, clusterNear) /
                    clusterNear) * clusterLogScale;
                return std::min(clusterDepthSlices - 1u,
                    static_cast<uint32_t>(std::max(0.f, normalized)));
            };
            const auto appendLight = [&](uint32_t cluster, uint32_t light)
            {
                glm::uvec2& header = headers[cluster];
                if (header.y < maximumLightsPerCluster)
                    indices[header.x + header.y++] = light;
            };
            const auto* lights = static_cast<const Engine::Model::LightData*>(
                m_lightDataMapped);
            for (uint32_t lightIndex = 0; lightIndex < m_frameLightCount; ++lightIndex)
            {
                const auto& light = lights[lightIndex];
                if (std::abs(light.params.y - 1.f) < 0.25f)
                {
                    for (uint32_t cluster = 0; cluster < clusterCount; ++cluster)
                        appendLight(cluster, lightIndex);
                    continue;
                }
                const glm::vec4 viewPosition = view * glm::vec4(
                    glm::vec3(light.positionRange), 1.f);
                const float range = std::max(0.f, light.positionRange.w);
                if (viewPosition.z + range < clusterNear ||
                    viewPosition.z - range > clusterFar)
                    continue;
                const glm::vec4 clip = proj * viewPosition;
                if (std::abs(clip.w) < 0.0001f)
                    continue;
                const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                const float radiusPixels = std::abs(proj[1][1]) * range /
                    std::max(viewPosition.z, clusterNear) *
                    static_cast<float>(viewportHeight) * 0.5f;
                const float centerX = (ndc.x * 0.5f + 0.5f) * viewportWidth;
                const float centerY = (1.f - (ndc.y * 0.5f + 0.5f)) * viewportHeight;
                const int32_t minTileX = std::clamp(static_cast<int32_t>(
                    std::floor((centerX - radiusPixels) / clusterTileSize)),
                    0, static_cast<int32_t>(clusterTilesX) - 1);
                const int32_t maxTileX = std::clamp(static_cast<int32_t>(
                    std::floor((centerX + radiusPixels) / clusterTileSize)),
                    0, static_cast<int32_t>(clusterTilesX) - 1);
                const int32_t minTileY = std::clamp(static_cast<int32_t>(
                    std::floor((centerY - radiusPixels) / clusterTileSize)),
                    0, static_cast<int32_t>(clusterTilesY) - 1);
                const int32_t maxTileY = std::clamp(static_cast<int32_t>(
                    std::floor((centerY + radiusPixels) / clusterTileSize)),
                    0, static_cast<int32_t>(clusterTilesY) - 1);
                const uint32_t minSlice = depthSlice(viewPosition.z - range);
                const uint32_t maxSlice = depthSlice(viewPosition.z + range);
                for (uint32_t slice = minSlice; slice <= maxSlice; ++slice)
                for (int32_t tileY = minTileY; tileY <= maxTileY; ++tileY)
                for (int32_t tileX = minTileX; tileX <= maxTileX; ++tileX)
                {
                    const uint32_t cluster = slice * clusterTilesX * clusterTilesY +
                        static_cast<uint32_t>(tileY) * clusterTilesX +
                        static_cast<uint32_t>(tileX);
                    appendLight(cluster, lightIndex);
                }
            }
            std::memcpy(m_clusterHeaderMapped, headers.data(),
                headers.size() * sizeof(glm::uvec2));
            std::memcpy(m_clusterLightIndexMapped, indices.data(),
                indices.size() * sizeof(uint32_t));
            m_clusterHeaderBuffer->FlushMappedWrites(0,
                headers.size() * sizeof(glm::uvec2));
            m_clusterLightIndexBuffer->FlushMappedWrites(0,
                indices.size() * sizeof(uint32_t));
            m_lastRenderCpuTimings.clusterUploadBytes=headers.size()*sizeof(glm::uvec2)+indices.size()*sizeof(uint32_t);
            if(directionalOnly) m_directionalClusterLayouts[bufferKey]=layout;
        }
        m_lastRenderCpuTimings.clusterPreparationMs=std::chrono::duration<double,std::milli>(RenderClock::now()-clusterStart).count();
    }

    if (shadowSelection.valid)
    {
        const uint32_t resolution = std::clamp(static_cast<uint32_t>(
            static_cast<float>(m_realtimeShadowSettings.directionalResolution) *
            shadowSelection.resolutionScale), 256u, 8192u);
        if (!m_directionalShadowMap ||
            m_directionalShadowResolution != resolution)
        {
            auto* textureFactory = m_graphicsProvider->GetTextureFactory();
            m_directionalShadowMap = textureFactory
                ? textureFactory->CreateDepthTexture2D(resolution, resolution)
                : nullptr;
            m_directionalShadowResolution = m_directionalShadowMap
                ? resolution : 0u;
            if (m_directionalShadowMap)
                ++m_directionalShadowAllocationGeneration;
        }
        if (!m_directionalShadowMap)
        {
            auto* lights = static_cast<Engine::Model::LightData*>(
                m_lightDataMapped);
            lights[shadowSelection.lightIndex].params.z = -1.f;
            lights[shadowSelection.lightIndex].params.w = 0.f;
            shadowSelection.valid = false;
        }
    }
    m_lightDataBuffer->FlushMappedWrites();
    m_shadowDebugSnapshot.hasSelectedLight = shadowSelection.valid;
    m_shadowDebugSnapshot.selectedLight = shadowSelection.stableLightKey;
    m_shadowDebugSnapshot.directionToLight =
        shadowSelection.directionToLight;
    m_shadowDebugSnapshot.atlasResolution = shadowSelection.valid
        ? m_directionalShadowResolution : 0u;
    m_shadowDebugSnapshot.atlasAllocationGeneration =
        m_directionalShadowAllocationGeneration;
        const bool wireframeMode =
            settings.renderMode == Engine::Model::SceneRenderMode::Wireframe;
        const bool forceUnlitMode =
            settings.renderMode == Engine::Model::SceneRenderMode::Unlit;

    struct ShadowCascadeSet
    {
        std::array<glm::mat4, 4> viewProjections{
            glm::mat4(1.f), glm::mat4(1.f), glm::mat4(1.f), glm::mat4(1.f) };
        glm::vec4 splits { 0.f };
        glm::vec4 data { 0.f };
        glm::vec4 cameraData { 0.f };
        std::array<glm::vec3, 4> centers{};
        std::array<float, 4> radii{};
        std::array<Engine::Rendering::ShadowReceiverCulling,4> receivers{};
        uint32_t count = 1u;
    };
    const auto buildShadowCascades = [&](const glm::mat4& cascadeView,
        uint32_t atlasResolution,
        uint32_t requestedCascadeCount)
    {
        ShadowCascadeSet result{};
        const float shadowDistance = std::clamp(
            m_realtimeShadowSettings.directionalDistance, 1.f, 100000.f);
        result.count = std::clamp(requestedCascadeCount, 1u, 4u);
        const float cameraNear = std::max(0.001f, cam->nearPlane);
        const float cameraFar = std::max(cameraNear + 0.001f, cam->farPlane);
        const float cascadeFar = std::min(cameraFar,
            std::max(cameraNear + 0.001f, shadowDistance));
        const float splitLambda = std::clamp(
            m_realtimeShadowSettings.cascadeSplitLambda, 0.f, 1.f);
        for (uint32_t cascade = 0; cascade < result.count; ++cascade)
        {
            const float fraction = static_cast<float>(cascade + 1u) /
                static_cast<float>(result.count);
            const float logarithmic = cameraNear * std::pow(
                cascadeFar / cameraNear, fraction);
            const float uniform = cameraNear +
                (cascadeFar - cameraNear) * fraction;
            result.splits[cascade] = glm::mix(
                uniform, logarithmic, splitLambda);
        }
        for (uint32_t cascade = result.count; cascade < 4u; ++cascade)
            result.splits[cascade] = cascadeFar;

        const glm::vec3 directionToLight = glm::normalize(
            shadowSelection.directionToLight);
        const glm::vec3 up = std::abs(directionToLight.y) > 0.98f
            ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);
        const glm::mat4 lightOrientation = glm::lookAtLH(
            glm::vec3(0.f), -directionToLight, up);
        // Fit in camera-local space. Inverting float projection*view and then
        // measuring world-space corners makes the radius depend on camera
        // rotation/translation roundoff, defeating texel-grid stabilization.
        const auto inverseProjection=glm::inverse(glm::dmat4(proj));
        const auto inverseCascadeView=glm::inverse(glm::dmat4(cascadeView));
        std::array<glm::dvec3,4> rayOrigins{},rayDirections{};
        for (uint32_t corner = 0; corner < 4u; ++corner)
        {
            const double x=(corner & 1u) ? 1.0:-1.0;
            const double y=(corner & 2u) ? 1.0:-1.0;
            const auto nearPoint=inverseProjection*glm::dvec4(x,y,0,1);
            // An interior depth avoids the ill-conditioned far-plane divide
            // for large far/near ratios. Extend this ray to exact view depths.
            const auto middlePoint=inverseProjection*glm::dvec4(x,y,.5,1);
            rayOrigins[corner]=glm::dvec3(nearPoint)/nearPoint.w;
            rayDirections[corner]=glm::dvec3(middlePoint)/middlePoint.w-rayOrigins[corner];
        }

        float previousSplit = cameraNear;
        float previousNear = cameraNear;
        const uint32_t tileResolution = result.count > 1u
            ? std::max(1u, atlasResolution / 2u) : atlasResolution;
        for (uint32_t cascade = 0; cascade < result.count; ++cascade)
        {
            const float split = result.splits[cascade];
            const double nearDepth=std::max(cameraNear,
                previousSplit-(previousSplit-previousNear)*std::clamp(m_realtimeShadowSettings.cascadeTransitionFraction,0.f,0.3f));
            std::array<glm::dvec3,8> localCorners{};
            std::array<glm::vec3, 8> corners{};
            glm::dvec3 localCenter(0);
            for (uint32_t corner = 0; corner < 4u; ++corner)
            {
                const auto& origin=rayOrigins[corner];
                const auto& ray=rayDirections[corner];
                localCorners[corner]=origin+ray*((nearDepth-origin.z)/ray.z);
                localCorners[corner+4]=origin+ray*((double(split)-origin.z)/ray.z);
                localCenter+=localCorners[corner]+localCorners[corner+4];
                corners[corner]=glm::vec3(inverseCascadeView*glm::dvec4(localCorners[corner],1));
                corners[corner+4]=glm::vec3(inverseCascadeView*glm::dvec4(localCorners[corner+4],1));
            }
            localCenter/=8.0;
            const auto center=glm::vec3(inverseCascadeView*glm::dvec4(localCenter,1));
            double localRadius=.001;
            for(const auto& corner:localCorners)
                localRadius=std::max(localRadius,glm::length(corner-localCenter));
            const float radius=float(std::ceil(localRadius*16.0)/16.0);
            glm::vec3 snappedCenter = glm::vec3(
                lightOrientation * glm::vec4(center, 1.f));
            const float worldUnitsPerTexel = (radius * 2.f) /
                static_cast<float>(tileResolution);
            snappedCenter.x = std::round(snappedCenter.x / worldUnitsPerTexel) *
                worldUnitsPerTexel;
            snappedCenter.y = std::round(snappedCenter.y / worldUnitsPerTexel) *
                worldUnitsPerTexel;
            const glm::vec3 stabilizedCenter = glm::vec3(
                glm::inverse(lightOrientation) * glm::vec4(snappedCenter, 1.f));
            result.centers[cascade] = stabilizedCenter;
            result.radii[cascade] = radius;
            const glm::vec3 lightEye = stabilizedCenter +
                directionToLight * shadowDistance;
            const glm::mat4 lightView = glm::lookAtLH(
                lightEye, stabilizedCenter, up);
            const glm::mat4 lightProjection = glm::orthoLH_ZO(
                -radius, radius, -radius, radius,
                0.1f, shadowDistance * 2.f);
            result.viewProjections[cascade] = lightProjection * lightView;
            result.receivers[cascade].Build(corners,lightOrientation,
                worldUnitsPerTexel*float(m_realtimeShadowSettings.pcfRadius+2)+0.01f);
            previousNear=previousSplit;
            previousSplit = split;
        }
        result.data = {
            static_cast<float>(result.count), result.count > 1u ? 0.5f : 1.f,
            std::clamp(m_realtimeShadowSettings.cascadeTransitionFraction,
                0.f, 0.3f), result.splits[result.count - 1u] };
        result.cameraData = glm::vec4(glm::normalize(glm::vec3(
            glm::inverse(cascadeView)[2])), cameraNear);
        return result;
    };

    ShadowCascadeSet mainShadowCascades{};
    bool renderMainDirectionalShadow = shadowSelection.valid;
    std::array<uint8_t, 4> directionalShadowDirtyMask{};
    bool preserveDirectionalShadowAtlas = false;
    if (shadowSelection.valid)
    {
        ++m_directionalShadowFrameCounter;
        const ShadowCascadeSet requestedShadowCascades = buildShadowCascades(
            view, m_directionalShadowResolution,
            m_realtimeShadowSettings.directionalCascadeCount);
        mainShadowCascades = requestedShadowCascades;
        const bool cacheReusable = m_directionalShadowCacheValid &&
            m_directionalShadowCachedResolution ==
                m_directionalShadowResolution &&
            m_directionalShadowCachedLightKey ==
                shadowSelection.stableLightKey &&
            m_directionalShadowCachedCascadeCount ==
                requestedShadowCascades.count &&
            glm::dot(glm::normalize(shadowSelection.directionToLight),
                glm::normalize(m_directionalShadowCachedDirection)) > 0.9995f;
        if (shadowOnly)
        {
            auto copyCachedCascades = [&]()
            {
                mainShadowCascades.viewProjections =
                    m_directionalShadowCachedViewProjections;
                mainShadowCascades.splits =
                    m_directionalShadowCachedCascadeSplits;
                mainShadowCascades.data =
                    m_directionalShadowCachedCascadeData;
                mainShadowCascades.cameraData =
                    m_directionalShadowCachedCameraData;
                mainShadowCascades.centers =
                    m_directionalShadowCachedCascadeCenters;
                mainShadowCascades.radii =
                    m_directionalShadowCachedCascadeRadii;
                mainShadowCascades.count =
                    m_directionalShadowCachedCascadeCount;
            };
            if (!cacheReusable)
            {
                for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                    directionalShadowDirtyMask[cascade] = 1u;
            }
            else
            {
                const bool globalCascadeDataChanged =
                    glm::length(requestedShadowCascades.splits -
                        m_directionalShadowCachedCascadeSplits) > 1e-4f ||
                    glm::length(requestedShadowCascades.data -
                        m_directionalShadowCachedCascadeData) > 1e-4f ||
                    glm::length(requestedShadowCascades.cameraData -
                        m_directionalShadowCachedCameraData) > 1e-4f;
                if (globalCascadeDataChanged)
                {
                    for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                        directionalShadowDirtyMask[cascade] = 1u;
                }
                else
                {
                    const auto matrixDelta = [](const glm::mat4& first,
                        const glm::mat4& second)
                    {
                        float maxDelta = 0.f;
                        for (uint32_t row = 0; row < 4u; ++row)
                            for (uint32_t col = 0; col < 4u; ++col)
                                maxDelta = std::max(maxDelta,
                                    std::abs(first[row][col] - second[row][col]));
                        return maxDelta;
                    };
                    for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                    {
                        const float centerDelta = glm::length(
                            requestedShadowCascades.centers[cascade] -
                            m_directionalShadowCachedCascadeCenters[cascade]);
                        const float radiusDelta = std::abs(
                            requestedShadowCascades.radii[cascade] -
                            m_directionalShadowCachedCascadeRadii[cascade]);
                        const float projectionDelta = matrixDelta(
                            requestedShadowCascades.viewProjections[cascade],
                            m_directionalShadowCachedViewProjections[cascade]);
                        directionalShadowDirtyMask[cascade] =
                            (centerDelta > 0.05f || radiusDelta > 0.05f ||
                                projectionDelta > 1e-4f) ? 1u : 0u;
                    }
                    const uint32_t refreshCascade =
                        m_directionalShadowRefreshCursor %
                        requestedShadowCascades.count;
                    const bool temporalRefreshDue =
                        (m_directionalShadowFrameCounter -
                            m_directionalShadowCascadeLastRenderedFrame[refreshCascade]) >= 2u;
                    if (temporalRefreshDue)
                    {
                        directionalShadowDirtyMask[refreshCascade] = 1u;
                        m_directionalShadowRefreshCursor =
                            (refreshCascade + 1u) %
                            requestedShadowCascades.count;
                    }
                }
            }
            uint32_t dirtyCascadeCount = 0;
            for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                dirtyCascadeCount += directionalShadowDirtyMask[cascade] ? 1u : 0u;
            renderMainDirectionalShadow = dirtyCascadeCount > 0u;
            if (!renderMainDirectionalShadow && cacheReusable)
                copyCachedCascades();
            preserveDirectionalShadowAtlas = cacheReusable &&
                renderMainDirectionalShadow &&
                dirtyCascadeCount < requestedShadowCascades.count;
            if (preserveDirectionalShadowAtlas)
                for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                    if (!directionalShadowDirtyMask[cascade])
                    {
                        mainShadowCascades.viewProjections[cascade] =
                            m_directionalShadowCachedViewProjections[cascade];
                        mainShadowCascades.centers[cascade] =
                            m_directionalShadowCachedCascadeCenters[cascade];
                        mainShadowCascades.radii[cascade] =
                            m_directionalShadowCachedCascadeRadii[cascade];
                    }
        }
        else if (m_directionalShadowCacheValid)
        {
            mainShadowCascades.viewProjections =
                m_directionalShadowCachedViewProjections;
            mainShadowCascades.splits =
                m_directionalShadowCachedCascadeSplits;
            mainShadowCascades.data =
                m_directionalShadowCachedCascadeData;
            mainShadowCascades.cameraData =
                m_directionalShadowCachedCameraData;
            mainShadowCascades.centers =
                m_directionalShadowCachedCascadeCenters;
            mainShadowCascades.radii =
                m_directionalShadowCachedCascadeRadii;
            mainShadowCascades.count =
                m_directionalShadowCachedCascadeCount;
        }
    }
    const auto& shadowViewProjections = mainShadowCascades.viewProjections;
    const glm::vec4 shadowCascadeSplits = mainShadowCascades.splits;
    const uint32_t shadowCascadeCount = mainShadowCascades.count;
    if (shadowSelection.valid)
    {
        m_shadowDebugSnapshot.atlasResolution =
            m_directionalShadowResolution;
        m_shadowDebugSnapshot.cascadeCount = shadowCascadeCount;
        m_shadowDebugSnapshot.cascadeSplits = shadowCascadeSplits;
        m_shadowDebugSnapshot.cascadeCenters = mainShadowCascades.centers;
        m_shadowDebugSnapshot.cascadeRadii = mainShadowCascades.radii;
    }

    const Engine::Components::Texture* skybox = shadowOnly
        ? nullptr : ResolveSkyboxTexture();
    if (!shadowOnly)
        UpdateEnvironmentLighting(skybox);
    if (!shadowOnly && skybox && skybox->GetGraphicsTexture() && m_skyboxPipeline)
    {
        SkyboxCBData skyboxData{};
        // A skybox represents direction only. Excluding camera translation
        // prevents cancellation in the shader's farPoint - nearPoint math
        // when the editor camera is far from the world origin.
        const glm::mat4 skyboxView = glm::mat4(glm::mat3(view));
        skyboxData.invVP = glm::inverse(proj * skyboxView);
        skyboxData.displayParams = {
            m_editorMode2D ? 1.f : 0.f,
            std::exp2(settings.hdriExposure),
            glm::radians(settings.hdriRotation), 0.f };
        memcpy(m_skyboxCBMapped, &skyboxData, sizeof(skyboxData));
        context->SetPipeline(m_skyboxPipeline.get());
        context->SetConstantBuffer(0, m_skyboxConstantBuffer.get(), 0);
        context->SetTexture(0, skybox->GetGraphicsTexture());
        context->DrawInstanced(3, 1, 0, 0);
    }

    // Opaque and masked materials render first. Blended materials render
    // back-to-front with depth writes disabled. Build the complete view sort
    // key once so the comparator performs field comparisons only.
    const auto nearestLightingDistanceSquared = [&](const FrameRenderItem& item,
        const glm::mat4& itemWorld)
    {
        const glm::vec3 centerDelta = glm::vec3(itemWorld[3]) - cameraPosition;
        if (!item.mesh || item.sprite || !item.hasBounds)
            return glm::dot(centerDelta, centerDelta);

        const glm::vec3 minimum = item.boundsMinimum;
        const glm::vec3 maximum = item.boundsMaximum;
        const auto bounds=Engine::Rendering::TransformBounds(minimum,maximum,itemWorld);
        const glm::vec3 nearest = glm::clamp(cameraPosition,bounds.minimum,bounds.maximum);
        const glm::vec3 delta = nearest - cameraPosition;
        return glm::dot(delta, delta);
    };
    const glm::mat4 viewProjection = proj * view;
    const auto outsideViewFrustum = [&](const FrameRenderItem& item,
        const glm::mat4& itemWorld)
    {
        if (!item.mesh || item.sprite || item.warpedVertexBuffer ||
            !item.hasBounds || item.traversalChartPortal)
            return false;
        const glm::vec3 minimum = item.boundsMinimum;
        const glm::vec3 maximum = item.boundsMaximum;
        const glm::mat4 localToClip = viewProjection * itemWorld;
        std::array<glm::vec4, 8> corners{};
        size_t cornerIndex = 0;
        for (int z = 0; z < 2; ++z)
        {
            for (int y = 0; y < 2; ++y)
            {
                for (int x = 0; x < 2; ++x)
                {
                    corners[cornerIndex++] = localToClip * glm::vec4(
                        x ? maximum.x : minimum.x,
                        y ? maximum.y : minimum.y,
                        z ? maximum.z : minimum.z, 1.f);
                }
            }
        }
        const auto allOutside = [&](const auto& predicate)
        {
            return std::all_of(corners.begin(), corners.end(), predicate);
        };
        return allOutside([](const glm::vec4& point)
            { return point.x < -point.w; }) ||
            allOutside([](const glm::vec4& point)
            { return point.x > point.w; }) ||
            allOutside([](const glm::vec4& point)
            { return point.y < -point.w; }) ||
            allOutside([](const glm::vec4& point)
            { return point.y > point.w; }) ||
            allOutside([](const glm::vec4& point)
            { return point.z < 0.f; }) ||
            allOutside([](const glm::vec4& point)
            { return point.z > point.w; });
    };
    const auto resolvePortalTarget = [&](Engine::Components::SpatialManipulator* source)
        -> Engine::Components::SpatialManipulator*
    {
        if (!source)
            return nullptr;
        if (Engine::Components::SpatialManipulator* target = source->ResolveTarget())
            return target;
        // Support older and edit-mode scenes whose serialized link exists on
        // only one endpoint. Connections belong to manipulators, not meshes,
        // so inspect every scene object rather than the render draw list.
        for (Engine::Components::SpatialManipulator* candidate :
            m_frameSpatialManipulatorScratch)
        {
            if (!candidate || candidate->Owner == source->Owner)
                continue;
            if (candidate->ResolveTarget() == source)
                return candidate;
        }
        return nullptr;
    };
    const bool hasPortalViews = std::any_of(
        m_frameSpatialManipulatorScratch.begin(),
        m_frameSpatialManipulatorScratch.end(),
        [&](Engine::Components::SpatialManipulator* portal)
        {
            if (!portal || !portal->Owner ||
                !portal->Owner->IsEnabledInHierarchy() || !portal->enabled)
                return false;
            const auto mode = static_cast<Engine::Components::
                SpatialManipulator::ConnectionMode>(portal->connectionMode);
            if (mode != Engine::Components::SpatialManipulator::
                    ConnectionMode::Portal &&
                mode != Engine::Components::SpatialManipulator::
                    ConnectionMode::LinkedPortal)
                return false;
            const auto* target = resolvePortalTarget(portal);
            return target && target->enabled && target->Owner &&
                portal->HasCompatiblePortalShapeWith(*target);
        });
    // A terrain cell covers its rectangular footprint. Bound the entry and
    // exit of each sightline through that surface; a draw is hidden only when
    // every sightline crosses a nearer cell. Rebuild for each camera position.
    constexpr float terrainHorizonCircle = 6.28318530718f;
    struct TerrainFootprint
    {
        float minX, maxX, minZ, maxZ, minY, maxY;
        float nearDistance, farDistance;
        float angleStart, angleEnd;
    };
    std::vector<TerrainFootprint> terrainBlockers;
    const bool terrainWarpActive = std::any_of(
        m_frameSpatialManipulatorScratch.begin(),
        m_frameSpatialManipulatorScratch.end(),
        [](const Engine::Components::SpatialManipulator* manipulator)
        {
            return manipulator && manipulator->enabled &&
                manipulator->definesWarpVolume && manipulator->Owner &&
                manipulator->Owner->IsEnabledInHierarchy();
        });
    const bool useTerrainOcclusion = !shadowOnly && !includeEditorVisuals &&
        !hasPortalViews && !m_editorMode2D && !terrainWarpActive;
    std::future<std::vector<uint8_t>> cameraVisibilityJob;
    struct CameraVisibilityFence
    {
        std::future<std::vector<uint8_t>>& job;
        ~CameraVisibilityFence() { if (job.valid()) job.wait(); }
    } cameraVisibilityFence{cameraVisibilityJob};
    if (useTerrainOcclusion)
        cameraVisibilityJob = Engine::Rendering::RenderJobs::Get().Submit(
            [this, viewProjection] {
                return m_renderSpatialIndex.QueryUncached(viewProjection);
            });
    const auto terrainFootprint = [&](const FrameRenderItem& item)
        -> std::optional<TerrainFootprint>
    {
        if (!item.mesh || !item.hasBounds || item.sprite ||
            item.warpedVertexBuffer || item.traversalChartPortal ||
            item.blended)
            return std::nullopt;
        // A warped or rotated footprint may have gaps inside its XZ bounds.
        const glm::mat4& world = item.world;
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row)
                if (std::abs(world[column][row] -
                    (column == row ? 1.f : 0.f)) > 0.0001f)
                    return std::nullopt;
        const glm::vec3 minimum = item.boundsMinimum +
            glm::vec3(world[3]);
        const glm::vec3 maximum = item.boundsMaximum +
            glm::vec3(world[3]);
        if (minimum.x >= maximum.x || minimum.z >= maximum.z ||
            (cameraPosition.x >= minimum.x &&
                cameraPosition.x <= maximum.x &&
                cameraPosition.z >= minimum.z &&
                cameraPosition.z <= maximum.z))
            return std::nullopt;
        const float nearX = std::clamp(cameraPosition.x,
            minimum.x, maximum.x) - cameraPosition.x;
        const float nearZ = std::clamp(cameraPosition.z,
            minimum.z, maximum.z) - cameraPosition.z;
        const float nearDistance = std::hypot(nearX, nearZ);
        std::array<float, 4> angles{};
        float farDistance = 0.f;
        int corner = 0;
        for (float z : { minimum.z, maximum.z })
            for (float x : { minimum.x, maximum.x })
            {
                const float dx = x - cameraPosition.x;
                const float dz = z - cameraPosition.z;
                float angle = std::atan2(dz, dx);
                if (angle < 0.f)
                    angle += terrainHorizonCircle;
                angles[corner++] = angle;
                farDistance = std::max(farDistance, std::hypot(dx, dz));
            }
        std::sort(angles.begin(), angles.end());
        int gapAfter = 3;
        float largestGap = angles[0] + terrainHorizonCircle - angles[3];
        for (int i = 0; i < 3; ++i)
        {
            const float gap = angles[i + 1] - angles[i];
            if (gap > largestGap)
            {
                largestGap = gap;
                gapAfter = i;
            }
        }
        const float start = angles[(gapAfter + 1) % 4];
        return TerrainFootprint{ minimum.x, maximum.x, minimum.z,
            maximum.z, minimum.y, maximum.y, nearDistance, farDistance,
            start, start + terrainHorizonCircle - largestGap };
    };
    const auto rayDistances = [&](const TerrainFootprint& footprint,
        float angle)
    {
        const float dx = std::cos(angle);
        const float dz = std::sin(angle);
        const auto slab = [](float direction, float low, float high)
        {
            if (direction == 0.f)
                return std::pair<float, float>{
                    -std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::infinity() };
            const float a = low / direction;
            const float b = high / direction;
            return std::pair<float, float>{ std::min(a, b), std::max(a, b) };
        };
        const auto x = slab(dx, footprint.minX - cameraPosition.x,
            footprint.maxX - cameraPosition.x);
        const auto z = slab(dz, footprint.minZ - cameraPosition.z,
            footprint.maxZ - cameraPosition.z);
        return std::pair<float, float>{ std::max(x.first, z.first),
            std::min(x.second, z.second) };
    };
    const auto terrainPreparationStart=RenderClock::now();
    m_lastRenderCpuTimings.startupMs=std::chrono::duration<double,std::milli>(terrainPreparationStart-measurement.start).count();
    if (useTerrainOcclusion)
    {
        terrainBlockers.reserve(frameRenderItems.size());
        for (const FrameRenderItem& item : frameRenderItems)
        {
            if (!item.object || item.belongsToPreview ||
                (m_previewObject && IsObjectOrDescendant(
                    item.object, m_previewObject)))
                continue;
            if (!item.terrainSurface ||
                (!item.completeTerrainFootprint && !item.terrainCell))
                continue;
            const auto footprint = terrainFootprint(item);
            if (footprint)
                terrainBlockers.push_back(*footprint);
        }
    }
    m_lastRenderCpuTimings.terrainPreparationMs=std::chrono::duration<double,std::milli>(RenderClock::now()-terrainPreparationStart).count();
    const auto hiddenByTerrain = [&](const FrameRenderItem& item)
    {
        if (!useTerrainOcclusion || !item.terrainSurface)
            return false;
        const auto footprint = terrainFootprint(item);
        if (!footprint)
            return false;
        const float height = footprint->maxY - cameraPosition.y;
        const float targetSlope = height / (height >= 0.f
            ? footprint->nearDistance : footprint->farDistance);
        const float lowHeight = footprint->minY - cameraPosition.y;
        const float targetLowSlope = lowHeight / (lowHeight >= 0.f
            ? footprint->farDistance : footprint->nearDistance);
        for (const TerrainFootprint& blocker : terrainBlockers)
        {
            if (blocker.farDistance + 0.01f >= footprint->nearDistance)
                continue;
            float start = footprint->angleStart;
            float end = footprint->angleEnd;
            if (start < blocker.angleStart)
            {
                start += terrainHorizonCircle;
                end += terrainHorizonCircle;
            }
            if (start < blocker.angleStart + 0.00001f ||
                end > blocker.angleEnd - 0.00001f)
                continue;
            std::array<float, 14> angles{};
            size_t angleCount = 0;
            angles[angleCount++] = start;
            angles[angleCount++] = end;
            // Rectangle entry and exit switch sides at corner angles; their
            // extrema between corners occur at cardinal directions.
            for (float z : { blocker.minZ, blocker.maxZ })
                for (float x : { blocker.minX, blocker.maxX })
                {
                    float angle = std::atan2(z - cameraPosition.z,
                        x - cameraPosition.x);
                    if (angle < 0.f)
                        angle += terrainHorizonCircle;
                    if (angle < start)
                        angle += terrainHorizonCircle;
                    if (angle > start && angle < end)
                        angles[angleCount++] = angle;
                }
            for (int quadrant = 0; quadrant <= 8; ++quadrant)
            {
                const float angle = quadrant * terrainHorizonCircle * 0.25f;
                if (angle > start && angle < end)
                    angles[angleCount++] = angle;
            }
            float entryMin = std::numeric_limits<float>::max();
            float entryMax = 0.f;
            float exitMin = std::numeric_limits<float>::max();
            float exitMax = 0.f;
            for (size_t i = 0; i < angleCount; ++i)
            {
                const auto [entry, leave] = rayDistances(blocker, angles[i]);
                entryMin = std::min(entryMin, entry);
                entryMax = std::max(entryMax, entry);
                exitMin = std::min(exitMin, leave);
                exitMax = std::max(exitMax, leave);
            }
            const float highHeight = blocker.maxY - cameraPosition.y;
            const float lowBlockerHeight = blocker.minY - cameraPosition.y;
            const float entrySlope = highHeight /
                (highHeight >= 0.f ? entryMin : entryMax);
            const float exitSlope = lowBlockerHeight /
                (lowBlockerHeight >= 0.f ? exitMax : exitMin);
            if (targetLowSlope > entrySlope + 0.001f &&
                targetSlope + 0.001f < exitSlope)
                return true;
        }
        return false;
    };
    const auto cullingStart = RenderClock::now();
    static const std::vector<uint8_t> emptyVisibility;
    std::vector<uint8_t> preparedCameraVisibility;
    const bool hasPreparedCameraVisibility = cameraVisibilityJob.valid();
    if (hasPreparedCameraVisibility)
        preparedCameraVisibility = cameraVisibilityJob.get();
    const auto& spatialVisible = hasPreparedCameraVisibility
        ? preparedCameraVisibility
        : (!shadowOnly && !hasPortalViews && !m_editorMode2D
            ? m_renderSpatialIndex.Query(viewProjection) : emptyVisibility);
    std::vector<ViewRenderItem>& renderObjects = m_viewRenderItemScratch;
    renderObjects.clear();
    renderObjects.reserve(frameRenderItems.size());
    std::vector<ViewRenderItem> classified(frameRenderItems.size());
    std::vector<uint8_t> accepted(frameRenderItems.size(),0), terrainRejected(frameRenderItems.size(),0);
    Engine::Rendering::RenderJobs::Get().For(frameRenderItems.size(),cameraUsesSourceWarpChart ? SIZE_MAX:128,[&](size_t index)
    {
        const auto& item=frameRenderItems[index];
        if(shadowOnly && (!item.mesh || !item.castsShadows)) return;
        if(!spatialVisible.empty() && !spatialVisible[item.objectSlot]) return;
        if(item.belongsToPreview && !includeEditorVisuals) return;
        const auto world=useSourceChartMesh(item) ? sourceChartWorld(item):item.world;
        if(!shadowOnly && !hasPortalViews && spatialVisible.empty() && outsideViewFrustum(item,world)) return;
        if(hiddenByTerrain(item)) { terrainRejected[index]=1; return; }
        const auto delta=glm::vec3(world[3])-cameraPosition;
        classified[index]={&item,glm::dot(delta,delta),shadowOnly ? 0.f:nearestLightingDistanceSquared(item,world),
            world[3].z,item.sortingLayer,item.blended && !(includeEditorVisuals && item.mesh==m_editorWeightPaintMesh)};
        accepted[index]=1;
    });
    for(size_t index=0;index<classified.size();++index)
    { if(accepted[index]) renderObjects.push_back(classified[index]); m_lastTerrainOcclusionCulledCount+=terrainRejected[index]; }
    const auto sortingStart = RenderClock::now();
    m_lastRenderCpuTimings.cullingMs = std::chrono::duration<double,std::milli>(sortingStart-cullingStart).count();
    // The accepted view list owns its sort keys for the whole job. Scene
    // components are not consulted by the comparator.
    const auto sortViewItems = [&renderObjects, editorMode2D = m_editorMode2D]()
    {
        std::stable_sort(renderObjects.begin(), renderObjects.end(),
            [editorMode2D](const ViewRenderItem& first, const ViewRenderItem& second)
            {
                if (editorMode2D)
                {
                    if (first.sortingLayer != second.sortingLayer)
                        return first.sortingLayer < second.sortingLayer;
                    return first.worldDepth < second.worldDepth;
                }
                if (first.blended != second.blended)
                    return !first.blended;
                return first.blended
                    ? first.cameraDistanceSquared > second.cameraDistanceSquared
                    : first.cameraDistanceSquared < second.cameraDistanceSquared;
            });
    };
    std::future<void> sortJob;
    if (renderObjects.size() >= 128)
        sortJob = Engine::Rendering::RenderJobs::Get().Submit(sortViewItems);
    else
        sortViewItems();
    bool hasSceneLightProbeContributors = false;
    for (auto* group : GetComponents<Engine::Components::LightProbeGroup>())
        hasSceneLightProbeContributors |= group->valid && group->Owner->IsEnabledInHierarchy();
    for (auto* probe : GetComponents<Engine::Components::LightProbe>())
        hasSceneLightProbeContributors |= probe->valid && probe->Owner->IsEnabledInHierarchy();
    if (sortJob.valid()) sortJob.get();

    m_lastRenderCpuTimings.sortingMs = std::chrono::duration<double,std::milli>(RenderClock::now()-sortingStart).count();
    const auto occlusionStart=RenderClock::now();
    // Occlusion results are deliberately tied to the exact camera and scene
    // transforms that produced them. Any movement restores all objects to the
    // visible state; the backend consumes only completed GPU query results and
    // never stalls this frame waiting for an answer.
    uint64_t occlusionSignature = 1469598103934665603ull;
    const auto hashOcclusionBytes = [&](const void* source, size_t byteCount)
    {
        const auto* bytes = static_cast<const uint8_t*>(source);
        for (size_t index = 0; index < byteCount; ++index)
        {
            occlusionSignature ^= bytes[index];
            occlusionSignature *= 1099511628211ull;
        }
    };
    hashOcclusionBytes(&viewProjection, sizeof(viewProjection));
    for (const ViewRenderItem& visible : renderObjects)
    {
        hashOcclusionBytes(&visible.source->object,
            sizeof(visible.source->object));
        hashOcclusionBytes(&visible.blended, sizeof(visible.blended));
        const glm::mat4 objectWorld = useSourceChartMesh(*visible.source)
            ? sourceChartWorld(*visible.source) : visible.source->world;
        hashOcclusionBytes(&objectWorld, sizeof(objectWorld));
        if (visible.source->mesh)
        {
            const uint64_t revision =
                visible.source->mesh->GetConfigurationRevision();
            hashOcclusionBytes(&revision, sizeof(revision));
            const auto fadeRevision = visible.source->mesh->GetRuntimeFadeRevision();
            hashOcclusionBytes(&fadeRevision, sizeof(fadeRevision));
        }
    }
    const uint64_t occlusionViewId =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)) ^
        (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cam)) << 1u);
    const bool occlusionQueriesEnabled = context->BeginOcclusionFrame(
        occlusionViewId, occlusionSignature);

    m_lastRenderCpuTimings.occlusionMs=std::chrono::duration<double,std::milli>(RenderClock::now()-occlusionStart).count();
    struct PreparedDraw
    {
        Engine::Core::Object* object = nullptr;
        Engine::Components::Mesh* mesh = nullptr;
        Engine::Components::Mesh* shadowMesh = nullptr;
        Engine::Components::Material* material = nullptr;
        Engine::Graphics::IGraphicsBuffer* vertexBuffer = nullptr;
        Engine::Graphics::IGraphicsBuffer* indexBuffer = nullptr;
        Engine::Graphics::IGraphicsBuffer* shadowVertexBuffer = nullptr;
        Engine::Graphics::IGraphicsBuffer* shadowIndexBuffer = nullptr;
        Engine::Graphics::IGraphicsBuffer* morphDeltaBuffer = nullptr;
        Engine::Graphics::IGraphicsBuffer* morphWeightBuffer = nullptr;
        Engine::Graphics::IPipelineState* pipeline = nullptr;
        std::array<const Engine::Graphics::IGraphicsTexture*, 30> textures{};
        uint32_t textureSlotCount = 12;
        UINT64 constantBufferOffset = 0;
        DrawCBData drawData{};
        ObjectGPUData objectData{};
        uint32_t vertexStride = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t shadowVertexStride = 0;
        uint32_t shadowVertexCount = 0;
        uint32_t shadowIndexCount = 0;
        int gpuLod = -1, gpuMaximumLod = 0;
        glm::vec2 gpuLodDistances{0};
        uint32_t automaticGroup = UINT32_MAX;
        uint32_t frameItemIndex = 0;

        const Engine::Components::SpatialManipulator* traversalChartPortal = nullptr;
        const Engine::Components::SpatialManipulator* spatialManipulator = nullptr;
        bool preview = false;
        bool terrain = false;
        bool terrainCell = false;
        bool blended = false;
        bool pmxOutline = false;
        bool castsShadow = false;
        bool doubleSided = false;
        bool shadowDoubleSided = false;
        bool occlusionCandidate = false;
        uint64_t occlusionId = 0;
    };
    std::vector<PreparedDraw> preparedDraws;
    preparedDraws.reserve(renderObjects.size());
    // All streamed grass tiles share the same foliage asset and a small set
    // of base pipeline variants. Resolve each variant once per view.
    std::unordered_map<IPipelineState*,
        std::pair<IPipelineState*, std::string>> foliagePipelines;
    std::shared_ptr<Engine::Components::Texture> measurementGridTexture;
    bool measurementGridReady = false;

    struct DirtyRange { uint32_t first=UINT32_MAX,last=0; double elapsedMs=0; };
    struct BufferedRecords
    {
        std::vector<std::future<DirtyRange>> jobs;
        ~BufferedRecords() { for(auto& job:jobs) if(job.valid()) job.wait(); }
    };
    static thread_local std::array<BufferedRecords,2> recordBuffers;
    static thread_local unsigned recordBufferIndex=0;
    auto& recordSnapshot=recordBuffers[recordBufferIndex++ % recordBuffers.size()];
    struct RecordFence { BufferedRecords& snapshot; ~RecordFence() { for(auto& job:snapshot.jobs) if(job.valid()) job.wait(); } } recordFence{recordSnapshot};
    recordSnapshot.jobs.clear();
    uint32_t firstDirtyObject=UINT32_MAX, lastDirtyObject=0;
    const auto materialStart = RenderClock::now();
    UINT slot = 0;
    for (const ViewRenderItem& sortedItem : renderObjects)
    {
        const FrameRenderItem* renderItem = sortedItem.source;
        slot=renderItem->objectSlot;
        Engine::Core::Object* obj = renderItem->object;
        Engine::Components::Mesh* mesh = renderItem->mesh;
        Engine::Components::Sprite* sprite = renderItem->sprite;
        Engine::Components::Material* mat = renderItem->material;
        const Engine::Rendering::BakedLightingData* bakedLighting =
            renderItem->bakedLighting;
        const bool belongsToPreview = renderItem->belongsToPreview;
        const bool isPreview = belongsToPreview;
        const Engine::Model::DistanceLightingBand* lightingBand = nullptr;
        bool beyondLightingDistance = false;
        if (m_distanceLightingSettings.enabled &&
            !m_distanceLightingSettings.bands.empty())
        {
            const float distance = std::sqrt(
                sortedItem.lightingDistanceSquared);
            beyondLightingDistance = distance >
                m_distanceLightingSettings.maximumDistance;
            for (const auto& candidate : m_distanceLightingSettings.bands)
            {
                lightingBand = &candidate;
                if (distance <= candidate.endDistance)
                    break;
            }
        }
        const bool allowNormalMapping = !beyondLightingDistance &&
            (!lightingBand || lightingBand->normalMapping);
        const bool allowParallaxMapping = !beyondLightingDistance &&
            (!lightingBand || lightingBand->parallaxMapping);
        const bool allowSubsurfaceScattering = !beyondLightingDistance &&
            (!lightingBand || lightingBand->subsurfaceScattering);
        const bool allowEnvironmentDiffuse = !beyondLightingDistance &&
            (!lightingBand || lightingBand->environmentDiffuse);
        const bool allowReflections = !beyondLightingDistance &&
            (!lightingBand || lightingBand->reflections);
        const float minimumProjectedPixels = !beyondLightingDistance &&
            lightingBand ? std::max(0.f,
                lightingBand->minimumProjectedPixels) : 0.f;
        if (!shadowOnly && minimumProjectedPixels > 0.f && mesh &&
            mesh->HasBounds() && !sprite && !belongsToPreview)
        {
            const auto bounds = Engine::Rendering::TransformBounds(
                mesh->GetBoundsMin(), mesh->GetBoundsMax(), renderItem->world);
            const glm::vec3 extent = (bounds.maximum - bounds.minimum) * 0.5f;
            const float radius = glm::length(extent);
            if (radius > 0.f)
            {
                const float centerDistance = std::sqrt(
                    std::max(0.f, sortedItem.cameraDistanceSquared));
                const float distance = std::max(0.001f,
                    centerDistance - radius);
                float projectedDiameter = 0.f;
                if (cam->orthographic)
                {
                    const float halfHeight = std::max(0.01f,
                        cam->orthographicSize);
                    projectedDiameter = 2.f * radius *
                        static_cast<float>(viewportHeight) /
                        (2.f * halfHeight);
                }
                else
                {
                    const float halfFov = glm::radians(
                        std::clamp(cam->fov, 1.f, 179.f)) * 0.5f;
                    const float tanHalfFov = std::tan(halfFov);
                    if (tanHalfFov > 0.f)
                        projectedDiameter = 2.f * radius *
                            static_cast<float>(viewportHeight) /
                            (2.f * tanHalfFov * distance);
                }
                if (projectedDiameter > 0.f &&
                    projectedDiameter < minimumProjectedPixels)
                    continue;
            }
        }
        // Mixed-lighting contract: a valid bake owns shadowing on a static
        // receiver. It still participates in the caster pass below, allowing
        // dynamic receivers to retain realtime shadows from static geometry.
        const bool bakedStaticReceiver = bakedLighting && bakedLighting->valid;
        const bool allowRealtimeShadows = !bakedStaticReceiver &&
            !beyondLightingDistance &&
            (!lightingBand || lightingBand->realtimeShadows);
        if (bakedStaticReceiver)
            ++m_shadowDebugSnapshot.bakedReceiverCount;
        else
            ++m_shadowDebugSnapshot.dynamicReceiverCount;
        PreparedDraw preparedDraw{};
        preparedDraw.morphDeltaBuffer = m_emptyMorphDeltaBuffer.get();
        preparedDraw.morphWeightBuffer = m_emptyMorphWeightBuffer.get();
        preparedDraw.textures[8] = m_brdfIntegrationLut.get();
        preparedDraw.frameItemIndex=static_cast<uint32_t>(renderItem-frameRenderItems.data());
        preparedDraw.gpuLod=renderItem->gpuLod;
        preparedDraw.gpuMaximumLod=renderItem->gpuMaximumLod;
        preparedDraw.gpuLodDistances=renderItem->gpuLodDistances;
        if (mesh && mesh->prefabLodBand>=0)
        {
            preparedDraw.gpuLod=mesh->prefabLodBand;
            preparedDraw.gpuMaximumLod=2;
            preparedDraw.gpuLodDistances=mesh->prefabLodDistances;
        }
        preparedDraw.object = obj;
        preparedDraw.mesh = mesh;
        preparedDraw.material = sprite ? nullptr : mat;
        if (!shadowOnly && mat) mat->shaderError.clear();
        preparedDraw.spatialManipulator = renderItem->spatialManipulator;
        preparedDraw.preview = isPreview;
        preparedDraw.terrain = mesh && mesh->UsesTerrainVertexFormat();
        preparedDraw.terrainCell = mesh &&
            (obj->GetComponent<Engine::Components::Terrain>() != nullptr ||
                obj->GetComponent<Engine::Components::TerrainBatchAppearance>() != nullptr);
        preparedDraw.blended = sortedItem.blended;
        preparedDraw.pmxOutline = mat && mat->edgeEnabled &&
            mat->edgeWidth > 0.f && mat->edgeColor.a > 0.f &&
            !preparedDraw.terrain && !isPreview;
        preparedDraw.castsShadow = shadowSelection.valid && mesh && !sprite &&
            !sortedItem.blended && !belongsToPreview &&
            (!mat || mat->castsShadows);
        glm::vec2 casterFade=renderItem->shadowCasterFade;
        if(mesh && mesh->grassShadowCaster)
        {
            const float budget=std::max(0.f,m_realtimeShadowSettings.grassCasterDistance);
            if(budget<=0.f) preparedDraw.castsShadow=false;
            else if(casterFade.x<=0.f || budget<casterFade.x) casterFade.x=budget;
        }
        else if(mesh && mesh->foliageWindStrength>0.f)
        {
            const float budget=std::max(0.f,m_realtimeShadowSettings.foliageCasterDistance);
            if(budget<=0.f) preparedDraw.castsShadow=false;
            else if(casterFade.x<=0.f || budget<casterFade.x) casterFade.x=budget;
        }
        if(preparedDraw.castsShadow && casterFade.x>0.f && mesh->HasBounds())
        {
            const auto bounds=Engine::Rendering::TransformBounds(mesh->GetBoundsMin(),mesh->GetBoundsMax(),renderItem->world);
            const auto delta=glm::clamp(cameraPosition,bounds.minimum,bounds.maximum)-cameraPosition;
            preparedDraw.castsShadow=glm::dot(delta,delta)<=casterFade.x*casterFade.x;
        }
        if (shadowOnly)
        {
            if (!preparedDraw.castsShadow)
                continue;
            const bool useSourceChart = useSourceChartMesh(*renderItem);
            const glm::mat4 world = useSourceChart
                ? sourceChartWorld(*renderItem) : renderItem->world;
            const bool useSoftSurface = !m_editorMeshEditPose &&
                mesh->GetPosedVertexBuffer() &&
                !mesh->IsPortalPoseSnapshot() &&
                (useSourceChart || !renderItem->warpedVertexBuffer);
            preparedDraw.objectData.geometryFade=mesh->prefabLodBand>=0 ? glm::vec4(float(mesh->prefabLodBand),mesh->prefabLodDistances.x,mesh->prefabLodDistances.y,4.f) : (hasPortalViews && renderItem->geometryFade.w>=2.f ? mesh->GetRuntimeGeometryFade():renderItem->geometryFade);
            preparedDraw.objectData.shadowCasterFade={casterFade.x,std::min(casterFade.x,casterFade.y),1.f,0.f};
            preparedDraw.objectData.grassAnimation={0.f,0.f,0.f,0.f};
            preparedDraw.objectData.viewPositionAlphaCutoff=glm::vec4(cameraPosition,.001f);
            preparedDraw.objectData.alphaLodParams = mat && mat->overrideTextureLod
                ? mat->textureLod.AlphaGpuParameters() : settings.textureLod.AlphaGpuParameters();
            preparedDraw.objectData.textureLodParams = mat && mat->overrideTextureLod
                ? mat->textureLod.GpuParameters() : settings.textureLod.GpuParameters();
            preparedDraw.objectData.world = world;
            preparedDraw.objectData.viewProjection = proj * view;
            preparedDraw.objectData.traversalClipPlane =
                renderItem->traversalClipPlane;
            preparedDraw.objectData.spriteUvRect = { 0.f, 0.f, 1.f, 1.f };
            if (renderItem->skinJointCount > 0 && !useSoftSurface)
                preparedDraw.objectData.skinParams = {
                    static_cast<float>(renderItem->skinPaletteOffset),
                    static_cast<float>(renderItem->skinJointCount), 0.f, 0.f };
            if (!useSoftSurface && !m_editorMeshEditPose &&
                !mesh->IsPortalPoseSnapshot() && mesh->HasMorphTargets() &&
                mesh->GetMorphDeltaBuffer() && mesh->GetMorphWeightBuffer() &&
                (useSourceChart || !renderItem->warpedVertexBuffer))
            {
                preparedDraw.morphDeltaBuffer = mesh->GetMorphDeltaBuffer();
                preparedDraw.morphWeightBuffer = mesh->GetMorphWeightBuffer();
                preparedDraw.objectData.morphParams = {
                    static_cast<float>(mesh->GetActiveMorphTargetCount()),
                    static_cast<float>(mesh->GetVertexCount()), 0.f,
                    static_cast<float>(mesh->GetMorphDeltaStrideFloat4()) };
            }
            if (mat && mat->GetAlphaMode() ==
                Engine::Components::MaterialAlphaMode::Mask &&
                !preparedDraw.terrainCell)
            {
                uint32_t flags = 32u;
                if (!mat->useVertexColors) flags |= 4096u;
                preparedDraw.textures[0] = mat->baseColorTexture
                    ? mat->baseColorTexture->GetGraphicsTexture() : nullptr;
                if (preparedDraw.textures[0]) flags |= 1u;
                preparedDraw.objectData.materialParams.w =
                    static_cast<float>(flags);
                preparedDraw.objectData.baseColor.a = mat->baseColorAlpha;
                preparedDraw.objectData.pmxTextureTint.a =
                    mat->baseColorTextureTintAlpha;
                preparedDraw.objectData.textureUvSets0.x =
                    static_cast<float>(mat->baseColorUvSet);
                preparedDraw.objectData.viewPositionAlphaCutoff.w =
                    mat->alphaCutoff;
            }
            preparedDraw.doubleSided = !preparedDraw.terrainCell &&
                (mat && mat->doubleSided);
            // Independent prefab cards carry their authored caster sidedness.
            if (mesh->prefabLodBand>=0)
                preparedDraw.shadowDoubleSided=preparedDraw.doubleSided;
            preparedDraw.drawData = { slot, 0u, mesh->IsInstanced() ? (0x20000000u | (renderItem->geometryFade.w>=2.f && !hasPortalViews ? 0x00100000u:0u)):0u, 0u };
            preparedDraw.traversalChartPortal =
                renderItem->traversalChartPortal;
            preparedDraw.vertexBuffer = !useSourceChart &&
                renderItem->warpedVertexBuffer
                ? renderItem->warpedVertexBuffer
                : (useSoftSurface ? mesh->GetPosedVertexBuffer()
                    : mesh->GetGraphicsBuffer());
            preparedDraw.vertexStride = mesh->GetVertexStride();
            preparedDraw.vertexCount = mesh->GetVertexCount();
            preparedDraw.shadowMesh = mesh;
            if (!mesh->IsInstanced() && preparedDraw.gpuLod > 0 &&
                mesh->gpuLodSelection)
            {
                if (auto* runtimeShadowMesh =
                    mesh->GetRuntimeLodMesh(preparedDraw.gpuLod))
                    preparedDraw.shadowMesh = runtimeShadowMesh;
            }
            preparedDraw.shadowVertexBuffer = preparedDraw.shadowMesh->GetGraphicsBuffer();
            preparedDraw.shadowVertexStride = preparedDraw.shadowMesh->GetVertexStride();
            preparedDraw.shadowVertexCount = preparedDraw.shadowMesh->GetVertexCount();
            if (!useSourceChart && !renderItem->warpedVertexBuffer &&
                mesh->GetIndexBuffer())
            {
                preparedDraw.indexBuffer = mesh->GetIndexBuffer();
                preparedDraw.indexCount = mesh->GetIndexCount();
            }
            if (!useSourceChart && !renderItem->warpedVertexBuffer &&
                preparedDraw.shadowMesh->GetIndexBuffer())
            {
                preparedDraw.shadowIndexBuffer = preparedDraw.shadowMesh->GetIndexBuffer();
                preparedDraw.shadowIndexCount = preparedDraw.shadowMesh->GetIndexCount();
            }
            preparedDraws.push_back(std::move(preparedDraw));
            ++slot;
            continue;
        }
        preparedDraw.occlusionCandidate = occlusionQueriesEnabled &&
            !wireframeMode &&
            !sortedItem.blended && !sprite &&
            renderItem->geometryFade.w < .5f &&
            !renderItem->warpedVertexBuffer &&
            !renderItem->traversalChartPortal;
        preparedDraw.occlusionId = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(obj));
        // Version 3 and later bake lighting into generated material assets.
        // Keep the component values for inspection, but do not add them again
        // at runtime or the baked result would be double-lit.
        const bool usesLegacyProbeBake = bakedLighting && bakedLighting->valid &&
            bakedLighting->version < 3 && !(mesh&&mesh->useLightProbes);
        const bool usesInterpolatedProbes=mesh&&mesh->useLightProbes;
        const bool evaluateProbeLighting = hasSceneLightProbeContributors &&
            (usesInterpolatedProbes||!bakedLighting||!bakedLighting->valid);
        const auto resolveProbeLighting = [&]()
        {
            AssertOwnerThread();
            return evaluateProbeLighting
                ? Engine::Components::SampleLightProbes(*this,
                    glm::vec3(renderItem->world[3]))
                : Engine::Components::ProbeLightingSample{};
        };
        const Engine::Components::ProbeLightingSample probeLighting = resolveProbeLighting();
        const glm::vec3 bakedIrradiance =
            usesLegacyProbeBake
                ? bakedLighting->irradiance
                : (probeLighting.valid
                    ? probeLighting.irradiance : glm::vec3(0.f));
        const bool useSourceChart = useSourceChartMesh(*renderItem);
        glm::mat4 world = useSourceChart
            ? sourceChartWorld(*renderItem) : renderItem->world;
        if (sprite)
        {
            if (m_editorMode2D)
                world[3].z = 0.f;
            world = world * glm::scale(glm::mat4(1.f),
                glm::vec3(renderItem->spriteWorldSize, 1.f));
        }
        UINT64 offset = static_cast<UINT64>(slot) * kCBStride;

        ObjectGPUData objectData{};
        objectData.lightingChannels.x = mat
            ? static_cast<uint32_t>(mat->lightingChannels) : 0xffffffffu;
        objectData.lightingClusterParams = glm::vec4(
            static_cast<float>(clusterTileSize),
            static_cast<float>(clusterTilesX),
            static_cast<float>(clusterTilesY),
            static_cast<float>(clusterDepthSlices));
        objectData.lightingCameraData = glm::vec4(glm::normalize(glm::vec3(
            glm::inverse(view)[2])), clusterLogScale);
        objectData.shadowCameraData.w = clusterNear;
        if (settings.hdriLightingEnabled && skybox &&
            (allowEnvironmentDiffuse || allowReflections))
        {
            objectData.environmentParams = {
                std::max(0.f, settings.hdriIntensity) *
                    std::exp2(std::clamp(settings.hdriExposure, -16.f, 16.f)),
                glm::radians(settings.hdriRotation), 1.f, 1.f };
            std::copy(m_environmentSH.begin(), m_environmentSH.end(),
                objectData.environmentSH);
            preparedDraw.textures[6] = m_environmentMap
                ? m_environmentMap->GetSpecularTexture() : nullptr;
            if (preparedDraw.textures[6])
                objectData.reflectionEnvironmentParams.w = 1.f;
        }
        objectData.viewProjection = proj * view;
        objectData.sphereView = view;
        objectData.world = world;
        objectData.traversalClipPlane = renderItem->traversalClipPlane;
        objectData.spriteUvRect = { 0.f, 0.f, 1.f, 1.f };
        const bool useSoftSurface = !m_editorMeshEditPose && mesh &&
            mesh->GetPosedVertexBuffer() &&
            !mesh->IsPortalPoseSnapshot() &&
            (useSourceChart || !renderItem->warpedVertexBuffer);
        if (renderItem->skinJointCount > 0 && !useSoftSurface)
        {
            objectData.skinParams = {
                static_cast<float>(renderItem->skinPaletteOffset),
                static_cast<float>(renderItem->skinJointCount), 0.f, 0.f };
        }
        const bool useGpuMorphs = !useSoftSurface && !m_editorMeshEditPose && mesh && !sprite &&
            !mesh->IsPortalPoseSnapshot() && mesh->HasMorphTargets() &&
            mesh->GetMorphDeltaBuffer() && mesh->GetMorphWeightBuffer() &&
            (useSourceChart || !renderItem->warpedVertexBuffer);
        if (useGpuMorphs)
        {
            preparedDraw.morphDeltaBuffer = mesh->GetMorphDeltaBuffer();
            preparedDraw.morphWeightBuffer = mesh->GetMorphWeightBuffer();
            objectData.morphParams = {
                static_cast<float>(mesh->GetActiveMorphTargetCount()),
                static_cast<float>(mesh->GetVertexCount()), 0.f,
                static_cast<float>(mesh->GetMorphDeltaStrideFloat4()) };
        }
        Engine::Components::MaterialAlphaMode alphaMode = Engine::Components::MaterialAlphaMode::Opaque;
        bool doubleSided = false;

        if (sprite)
        {
            const Engine::Components::Texture* texture = renderItem->spriteTexture;
            const Engine::Graphics::IGraphicsTexture* graphicsTexture = texture
                ? texture->GetGraphicsTexture() : nullptr;
            preparedDraw.textures[0] = graphicsTexture;
            objectData.baseColor = glm::vec4(sprite->tint,
                std::clamp(sprite->alpha, 0.f, 1.f));
            objectData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
            objectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
            // Sprites use the base-colour texture and alpha testing in addition
            // to blending. Discarding empty atlas pixels prevents their black
            // RGB values from ever reaching the render target, while the low
            // cutoff preserves anti-aliased translucent edge pixels.
            constexpr uint32_t kBaseColorTextureFlag = 1u;
            constexpr uint32_t kAlphaMaskFlag = 32u;
            const uint32_t spriteTextureFlags = graphicsTexture
                ? kBaseColorTextureFlag | kAlphaMaskFlag
                : kAlphaMaskFlag;
            objectData.materialParams = { 0.f, 1.f, 1.f,
                static_cast<float>(spriteTextureFlags) };
            objectData.viewPositionAlphaCutoff = glm::vec4(cameraPosition, 0.01f);
            objectData.spriteUvRect = renderItem->spriteUvRect;
            alphaMode = Engine::Components::MaterialAlphaMode::Blend;
            doubleSided = true;
        }
        else if (mat)
        {
            const ObjectGPUData environmentSeed = objectData;
            const auto resolveMaterialRecord = [&]() -> ResolvedMaterialRecord
            {
                AssertOwnerThread();
                // Seed view environment values before applying material overrides.
                ObjectGPUData objectData{};
                auto textures = preparedDraw.textures;
                bool doubleSided = false;
                auto alphaMode = Engine::Components::MaterialAlphaMode::Opaque;
                objectData.environmentParams = environmentSeed.environmentParams;
                objectData.reflectionEnvironmentParams = environmentSeed.reflectionEnvironmentParams;
                const uint32_t materialVariant=(allowNormalMapping?1u:0u)|(allowParallaxMapping?2u:0u)|
                    (allowEnvironmentDiffuse?4u:0u)|(allowReflections?8u:0u)|
                    (allowSubsurfaceScattering?16u:0u);
                auto& cachedMaterial=m_renderMaterialCache[mat][materialVariant];
                if(cachedMaterial.frame==m_materialPreparationSerial && cachedMaterial.revision==mat->GetConfigurationRevision() && cachedMaterial.data.size()==sizeof(ObjectGPUData))
                {
                    ObjectGPUData cachedData;
                    std::memcpy(&cachedData,cachedMaterial.data.data(),sizeof(cachedData));
                    objectData = cachedData;
                    textures=cachedMaterial.textures;
                    doubleSided=cachedMaterial.doubleSided;
                    alphaMode=static_cast<Engine::Components::MaterialAlphaMode>(cachedMaterial.alphaMode);
                    objectData.viewPositionAlphaCutoff=glm::vec4(cameraPosition,objectData.viewPositionAlphaCutoff.w);
                }
                else
                {

                alphaMode = mat->GetAlphaMode();
                doubleSided = mat->doubleSided;
                uint32_t textureFlags = 0;
                if (!mat->useVertexColors) textureFlags |= 4096u;
                auto prepareTexture = [&](uint32_t textureSlot,
                                          const std::shared_ptr<Engine::Components::Texture>& texture,
                                          uint32_t flag)
                {
                    const Engine::Graphics::IGraphicsTexture* graphicsTexture =
                        texture ? texture->GetGraphicsTexture() : nullptr;
                    textures[textureSlot] = graphicsTexture;
                    if (graphicsTexture)
                        textureFlags |= flag;
                };
                prepareTexture(0, mat->baseColorTexture, 1u);
                prepareTexture(10, mat->sphereTexture, 256u);
                prepareTexture(11, mat->toonTexture, 512u);
                prepareTexture(1, mat->metallicRoughnessTexture, 2u);
                if (allowNormalMapping)
                    prepareTexture(2, mat->normalTexture, 4u);
                prepareTexture(3, mat->occlusionTexture, 8u);
                prepareTexture(4, mat->emissiveTexture, 16u);
                if (allowSubsurfaceScattering &&
                    mat->subsurfaceEnabled && mat->subsurfaceWeight > 0.f &&
                    !mat->unlit && alphaMode != Engine::Components::MaterialAlphaMode::Blend)
                    prepareTexture(9, mat->subsurfaceThicknessTexture, 128u);
                if (allowParallaxMapping)
                    prepareTexture(5, mat->heightTexture, 64u);
                if (alphaMode == Engine::Components::MaterialAlphaMode::Mask)
                    textureFlags |= 32u;

                objectData.baseColor = glm::vec4(mat->diffuseColor, mat->baseColorAlpha);
                objectData.pmxTextureTint = glm::vec4(
                    mat->baseColorTextureTint, mat->baseColorTextureTintAlpha);
                objectData.pmxSphereTint = mat->sphereTint;
                objectData.pmxToonTint = mat->toonTint;
                objectData.pmxEdgeColor = mat->edgeColor;
                objectData.pmxParams = {
                    mat->edgeWidth, static_cast<float>(mat->sphereMode),
                    static_cast<float>(mat->builtinToon), mat->edgeEnabled ? 1.f : 0.f };
                objectData.ambientUnlit = glm::vec4(
                    settings.ambientColor + mat->ambientColor,
                    mat->unlit ? 1.f : 0.f);
                objectData.emissiveOcclusion = glm::vec4(
                    mat->emissiveColor, mat->occlusionStrength);
                objectData.materialParams = {
                    mat->metallicFactor, mat->roughnessFactor, mat->normalScale,
                    static_cast<float>(textureFlags)
                };
                if (mat->specularModel == "BlinnPhong")
                    objectData.blinnPhongSpecularShininess = glm::vec4(
                        mat->specularColor, std::max(mat->shininess, 0.f) + 1.f);
                // Blended and unlit materials opt out. A metallic-roughness map can
                // still suppress scattering per pixel in the shader.
                if (allowSubsurfaceScattering &&
                    mat->subsurfaceEnabled && !mat->unlit &&
                    alphaMode != Engine::Components::MaterialAlphaMode::Blend)
                {
                    objectData.subsurfaceColorWeight = glm::vec4(
                        mat->subsurfaceColor, mat->subsurfaceWeight);
                    objectData.subsurfaceRadiusThickness = glm::vec4(
                        mat->subsurfaceRadius, mat->subsurfaceThickness);
                    objectData.subsurfaceTransmission.x = mat->subsurfaceTransmission;
                }
                objectData.viewPositionAlphaCutoff = glm::vec4(
                    cameraPosition, mat->alphaCutoff);
                objectData.parallaxParams = {
                    mat->heightScale, mat->heightMinSteps,
                    mat->heightMaxSteps, 0.f
                };
                objectData.environmentParams.z = allowEnvironmentDiffuse
                    ? mat->environmentDiffuseStrength : 0.f;
                objectData.environmentParams.w = allowReflections
                    ? mat->reflectionStrength : 0.f;
                if (allowReflections)
                if (const auto reflectionEnvironment = ResolveReflectionEnvironment(*mat))
                {
                    objectData.reflectionEnvironmentParams = {
                        std::exp2(std::clamp(mat->reflectionEnvironmentExposure,
                            -16.f, 16.f)),
                        glm::radians(mat->reflectionEnvironmentRotation), 1.f, 0.f };
                    const auto& reflectionSH = reflectionEnvironment->GetRadianceSH();
                    std::copy(reflectionSH.begin(), reflectionSH.end(),
                        objectData.reflectionEnvironmentSH);
                    if (reflectionEnvironment->GetSpecularTexture())
                    {
                        textures[6] =
                            reflectionEnvironment->GetSpecularTexture();
                        objectData.reflectionEnvironmentParams.w = 1.f;
                    }
                }
                objectData.textureUvSets0 = { static_cast<float>(mat->baseColorUvSet),
                    static_cast<float>(mat->metallicRoughnessUvSet), static_cast<float>(mat->normalUvSet),
                    static_cast<float>(mat->occlusionUvSet) };
                objectData.textureUvSets1 = { static_cast<float>(mat->emissiveUvSet),
                    static_cast<float>(mat->heightUvSet), 0.f, 0.f };

                    cachedMaterial.data.resize(sizeof(ObjectGPUData));
                    std::memcpy(cachedMaterial.data.data(),&objectData,sizeof(objectData));
                    cachedMaterial.textures=textures;
                    cachedMaterial.doubleSided=doubleSided; cachedMaterial.alphaMode=uint32_t(alphaMode);
                    cachedMaterial.revision=mat->GetConfigurationRevision();
                    cachedMaterial.frame=m_materialPreparationSerial;
                }
                return { objectData, textures, doubleSided, alphaMode };
            };
            const ResolvedMaterialRecord resolvedMaterial = resolveMaterialRecord();
            ApplyResolvedMaterialRecord(resolvedMaterial, cameraPosition, bakedIrradiance, objectData);
            preparedDraw.textures = resolvedMaterial.textures;
            doubleSided = resolvedMaterial.doubleSided;
            alphaMode = resolvedMaterial.alphaMode;
        }
        else
        {
            objectData.baseColor = { 0.8f, 0.8f, 0.8f, 1.f };
            objectData.ambientUnlit = glm::vec4(
                settings.ambientColor + bakedIrradiance, 0.f);
            objectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
            objectData.materialParams = { 0.f, 1.f, 1.f, 0.f };
            objectData.viewPositionAlphaCutoff = glm::vec4(
                cameraPosition, 0.5f);
        }
        if (mesh && !mesh->UsesTerrainVertexFormat())
        {
            auto* terrain = obj->GetComponent<Engine::Components::Terrain>();
            auto* batch = obj->GetComponent<Engine::Components::TerrainBatchAppearance>();
            if (terrain || batch)
            {
                const auto& materials = terrain ? terrain->GetLayerMaterials() :
                    batch->materials;
                bool textured = false;
                preparedDraw.textures[0] = nullptr;
                preparedDraw.textures[1] = nullptr;
                preparedDraw.textures[2] = nullptr;
                preparedDraw.textures[3] = nullptr;
                preparedDraw.textures[4] = nullptr;
                preparedDraw.textures[5] = nullptr;
                for (size_t layer = 0; layer < std::min<size_t>(materials.size(), 6); ++layer)
                {
                    const auto* material = materials[layer].get();
                    const auto* texture = material && material->baseColorTexture
                        ? material->baseColorTexture->GetGraphicsTexture() : nullptr;
                    preparedDraw.textures[layer] = texture;
                    textured |= texture != nullptr;
                    if (texture) objectData.terrainMapMasks.w |= 1u << layer;
                    objectData.terrainLayerParams[layer] = {
                        material ? material->roughnessFactor : 1.f,
                        material ? material->normalScale : 1.f,
                        material ? material->terrainHeightBlendStrength : 0.f,
                        0.f };
                    if (material)
                        objectData.terrainMacroColors[layer] = glm::vec4(
                            glm::length(material->terrainMacroColor) > 0.f
                                ? material->terrainMacroColor
                                : material->diffuseColor, 0.f);
                    const auto* normalTexture = material && material->normalTexture
                        ? material->normalTexture->GetGraphicsTexture() : nullptr;
                    const auto* roughnessTexture = material &&
                        material->metallicRoughnessTexture
                        ? material->metallicRoughnessTexture->GetGraphicsTexture()
                        : nullptr;
                    const auto* heightTexture = material && material->heightTexture
                        ? material->heightTexture->GetGraphicsTexture() : nullptr;
                    preparedDraw.textures[12 + layer] = normalTexture;
                    preparedDraw.textures[18 + layer] = roughnessTexture;
                    preparedDraw.textures[24 + layer] = heightTexture;
                    if (normalTexture || roughnessTexture || heightTexture)
                        preparedDraw.textureSlotCount = 30;
                    if (normalTexture) objectData.terrainMapMasks.x |= 1u << layer;
                    if (roughnessTexture) objectData.terrainMapMasks.y |= 1u << layer;
                    if (heightTexture && material->terrainHeightBlendStrength > 0.f)
                        objectData.terrainMapMasks.z |= 1u << layer;
                    textured |= normalTexture || roughnessTexture ||
                        heightTexture;
                }
                objectData.baseColor = { 1.f, 1.f, 1.f, 1.f };
                objectData.materialParams.w = textured ? 2048.f : 0.f;
                // Opt-in static terrain keeps its painted texture blend while
                // using a cheaper directional diffuse pass during play.
                const auto* terrainManager = obj->GetComponentInParent<
                    Engine::Components::TerrainManager>();
                if (!includeEditorVisuals && !forceUnlitMode && terrainManager &&
                    terrainManager->fastRuntimeLighting)
                {
                    objectData.materialParams.w += batch && batch->distantVisual
                        ? 16384.f : 8192.f;
                    objectData.textureUvSets1.w =
                        static_cast<float>(std::max(terrainManager->loadedRadius,
                            terrainManager->farRadius)) *
                        std::max(terrainManager->cellsX * terrainManager->cellWidth,
                            terrainManager->cellsY * terrainManager->cellDepth);
                }
                objectData.textureUvSets1.z = terrain ? terrain->textureTiling :
                    batch->textureTiling;
                if (terrainManager && !includeEditorVisuals)
                    objectData.textureUvSets1.x =
                        std::max(terrainManager->cellsX * terrainManager->cellWidth,
                            terrainManager->cellsY * terrainManager->cellDepth);
                if (terrainManager && terrainManager->stochasticTerrainVariation)
                    objectData.materialParams.w += 65536.f;
                if (terrainManager && terrainManager->terrainMacroColorEnabled)
                    objectData.materialParams.w += 131072.f;
                if (terrainManager && terrainManager->measurementGrid)
                {
                    if (!measurementGridTexture)
                    {
                        measurementGridTexture = Engine::Components::Texture::Acquire(
                            Engine::Components::Mesh::ResolveFilePath(
                                "Assets/Textures/Terrain/measurement_grid.png"));
                        measurementGridReady = measurementGridTexture &&
                            measurementGridTexture->Prepare(m_graphicsProvider);
                    }
                    if (measurementGridReady)
                    {
                        preparedDraw.textures[0] =
                            measurementGridTexture->GetGraphicsTexture();
                        objectData.materialParams.w = 32768.f;
                        objectData.textureUvSets1.x =
                            terrainManager->cellsX * terrainManager->cellWidth;
                        objectData.textureUvSets1.y =
                            terrainManager->cellsY * terrainManager->cellDepth;
                    }
                }
            }
        }
        if (includeEditorVisuals && mesh == m_editorWeightPaintMesh &&
            m_editorWeightPaintBone >= 0)
        {
            // The visualization is transient GPU state. Authored materials,
            // textures, vertex colors, and mesh data remain untouched.
            preparedDraw.textures.fill(nullptr);
            objectData.baseColor = { 0.65f, 0.65f, 0.65f, 1.f };
            objectData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
            objectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
            objectData.materialParams = { 0.f, 1.f, 1.f, 0.f };
            objectData.morphParams.z = static_cast<float>(m_editorWeightPaintBone + 1);
            objectData.environmentParams.z = 0.f;
            objectData.environmentParams.w = 0.f;
            alphaMode = Engine::Components::MaterialAlphaMode::Opaque;
        }
        // Resolve after cached material assembly: scene settings must also affect
        // static objects and material-less meshes without rebuilding their assets.
        objectData.alphaLodParams = mat && mat->overrideTextureLod
            ? mat->textureLod.AlphaGpuParameters() : settings.textureLod.AlphaGpuParameters();
        objectData.textureLodParams = mat && mat->overrideTextureLod
            ? mat->textureLod.GpuParameters() : settings.textureLod.GpuParameters();
        bool terrainShadowDoubleSided = doubleSided;
        // Height fields have a single upward-facing surface, including cached
        // terrain and streamed batches. Backfaces do not contribute color or shadows.
        if (preparedDraw.terrainCell)
            doubleSided = terrainShadowDoubleSided = false;
        preparedDraw.doubleSided = doubleSided;
        preparedDraw.shadowDoubleSided = terrainShadowDoubleSided;
        if (shadowSelection.valid)
        {
            std::copy(shadowViewProjections.begin(),
                shadowViewProjections.end(), objectData.shadowViewProjections);
            objectData.shadowParams = {
                shadowSelection.depthBias,
                shadowSelection.normalBias,
                allowRealtimeShadows ? 1.f : 0.f,
                static_cast<float>(std::min(
                    std::min(m_realtimeShadowSettings.pcfRadius,
                        lightingBand
                            ? lightingBand->maximumShadowPcfRadius : 4u),
                    static_cast<uint32_t>(std::round(4.f *
                        shadowSelection.filterScale)))) };
            objectData.shadowCascadeSplits = shadowCascadeSplits;
            objectData.shadowCascadeData = mainShadowCascades.data;
            // Terrain patches are subdivisions of one continuous surface.
            // Applying an object-level lighting band to each patch creates
            // chunk-shaped shadow cutoffs even though the cascade already
            // performs a smooth per-pixel distance fade.
            objectData.shadowCascadeData.w *= preparedDraw.terrain
                ? 1.f
                : (lightingBand
                    ? std::clamp(lightingBand->shadowDistanceScale, 0.f, 1.f)
                    : 1.f);
            objectData.shadowCameraData = mainShadowCascades.cameraData;
            preparedDraw.textures[7] = m_directionalShadowMap.get();
        }
        ResolvedObjectRecord resolvedObject;
        resolvedObject.preview = isPreview;
        resolvedObject.forceUnlit = forceUnlitMode;
        if (usesLegacyProbeBake)
        {
            resolvedObject.directional = glm::vec4(
                bakedLighting->directionalIrradiance, 0.f);
            resolvedObject.direction = glm::vec4(bakedLighting->lightDirection, 1.f);
        }
        else if (probeLighting.valid)
        {
            resolvedObject.directional = glm::vec4(probeLighting.directionalIrradiance, 0.f);
            resolvedObject.direction = glm::vec4(probeLighting.direction, 1.f);
        }
        resolvedObject.data = objectData;
        objectData = AssembleObjectRecord(resolvedObject);

        // Static terrain's inexpensive directional pass must keep the sun
        // through the detailed-to-far-field join. The generic lighting
        // distance cap otherwise leaves an ambient-only dark ring before the
        // unlit distant terrain starts.
        const bool fastTerrainLighting =
            (static_cast<uint32_t>(objectData.materialParams.w) & 8192u) != 0u &&
            objectData.ambientUnlit.w < 0.5f;
        const bool farTerrainVertexLighting =
            (static_cast<uint32_t>(objectData.materialParams.w) & 16384u) != 0u;
        if (fastTerrainLighting || farTerrainVertexLighting)
        {
            objectData.bakedDirectional = glm::vec4(terrainSunRadiance,
                fastTerrainLighting && shadowSelection.valid && terrainSunIndex ==
                    static_cast<int>(shadowSelection.lightIndex)
                    ? static_cast<float>(terrainSunIndex + 1) : 0.f);
            objectData.bakedLightDirection = glm::vec4(terrainSunDirection, 0.f);
        }
        const uint32_t qualityLightCount = fastTerrainLighting
            ? 0u : beyondLightingDistance ? 0u :
            (lightingBand
                ? std::min(m_frameLightCount, lightingBand->maxRealtimeLights)
                : m_frameLightCount);
        const DrawCBData drawData{ slot,
            forceUnlitMode ? 0u : qualityLightCount,
            (clusteredLightingEnabled ? kClusteredLightingFlag : 0u) |
                (mesh && mesh->IsInstanced() ? (0x20000000u | (renderItem->geometryFade.w>=2.f && !hasPortalViews ? 0x00100000u:0u)):0u), 0u };
        preparedDraw.drawData = drawData;
        objectData.geometryFade = mesh && mesh->prefabLodBand>=0 ? glm::vec4(float(mesh->prefabLodBand),mesh->prefabLodDistances.x,mesh->prefabLodDistances.y,4.f) : (hasPortalViews && renderItem->geometryFade.w>=2.f ? mesh->GetRuntimeGeometryFade():renderItem->geometryFade);
        objectData.shadowCasterFade={casterFade.x,std::min(casterFade.x,casterFade.y),0.f,0.f};
        objectData.grassAnimation={mesh && (mesh->grassShadowCaster || mesh->foliageWindStrength>0.f) ? static_cast<float>(GetElapsedTime()):0.f,
            mesh && (mesh->grassShadowCaster || mesh->foliageWindStrength>0.f) ? 1.f : 0.f,
            mesh && mesh->foliageWindStrength>0.f ? std::clamp(mesh->foliageWindStrength,0.f,1.f):1.f, 0.f};
        preparedDraw.objectData = objectData;
        preparedDraw.traversalChartPortal = renderItem->traversalChartPortal;

        Engine::Graphics::IPipelineState* materialPipeline = nullptr;
        if (isPreview)
                materialPipeline = wireframeMode
                    ? (doubleSided
                        ? m_objectPreviewWireDoubleSidedPipeline.get()
                        : m_objectPreviewWirePipeline.get())
                    : (doubleSided
                        ? m_objectPreviewDoubleSidedPipeline.get()
                        : m_objectPreviewPipeline.get());
        else if (alphaMode == Engine::Components::MaterialAlphaMode::Blend)
                materialPipeline = wireframeMode
                    ? (doubleSided
                        ? m_objectBlendWireDoubleSidedPipeline.get()
                        : m_objectBlendWirePipeline.get())
                    : (doubleSided
                        ? m_objectBlendDoubleSidedPipeline.get()
                        : m_objectBlendPipeline.get());
        else
                materialPipeline = wireframeMode
                    ? (doubleSided
                        ? m_objectWireDoubleSidedPipeline.get()
                        : m_objectWirePipeline.get())
                    : (doubleSided
                        ? m_objectDoubleSidedPipeline.get()
                        : m_objectPipeline.get());
        if (!shadowOnly && !isPreview && !wireframeMode && !sprite && mesh &&
            !preparedDraw.terrain && !preparedDraw.terrainCell &&
            !mesh->HasMorphTargets() && !mesh->HasSurfaceOffsets() &&
            !renderItem->skinJointCount && IsFastFoliageSurface(mat))
        {
            auto [entry, inserted] = foliagePipelines.try_emplace(materialPipeline);
            if (inserted)
                entry->second.first = ResolveMaterialShaderPipeline(
                    ResolveMaterialShaderSource(*mat,"Assets/Shaders/Foliage.hlsl"),
                    materialPipeline, false, false, "Auto",
                    entry->second.second);
            if (entry->second.first) materialPipeline = entry->second.first;
            else mat->shaderError = entry->second.second;
        }
        preparedDraw.pipeline = materialPipeline;
        preparedDraw.constantBufferOffset = offset;
        preparedDraw.vertexBuffer = sprite
            ? renderItem->spriteVertexBuffer
            : (!useSourceChart && renderItem->warpedVertexBuffer
                ? renderItem->warpedVertexBuffer
                : (mesh ? (useSoftSurface ? mesh->GetPosedVertexBuffer()
                    : mesh->GetGraphicsBuffer()) : nullptr));
        preparedDraw.vertexStride = sprite
            ? sprite->GetVertexStride() : mesh->GetVertexStride();
        preparedDraw.vertexCount = sprite
            ? sprite->GetVertexCount() : mesh->GetVertexCount();
        preparedDraw.shadowMesh = mesh;
        if (!sprite && mesh && !mesh->IsInstanced() &&
            preparedDraw.gpuLod > 0 && mesh->gpuLodSelection)
        {
            if (auto* runtimeShadowMesh =
                mesh->GetRuntimeLodMesh(preparedDraw.gpuLod))
                preparedDraw.shadowMesh = runtimeShadowMesh;
        }
        preparedDraw.shadowVertexBuffer = sprite
            ? preparedDraw.vertexBuffer
            : (preparedDraw.shadowMesh
                ? preparedDraw.shadowMesh->GetGraphicsBuffer()
                : preparedDraw.vertexBuffer);
        preparedDraw.shadowVertexStride = sprite
            ? preparedDraw.vertexStride
            : (preparedDraw.shadowMesh
                ? preparedDraw.shadowMesh->GetVertexStride()
                : preparedDraw.vertexStride);
        preparedDraw.shadowVertexCount = sprite
            ? preparedDraw.vertexCount
            : (preparedDraw.shadowMesh
                ? preparedDraw.shadowMesh->GetVertexCount()
                : preparedDraw.vertexCount);
        const bool usesDirectMeshBuffer = !sprite && !useSourceChart &&
            !renderItem->warpedVertexBuffer;
        if (usesDirectMeshBuffer && mesh && mesh->GetIndexBuffer())
        {
            preparedDraw.indexBuffer = mesh->GetIndexBuffer();
            preparedDraw.indexCount = mesh->GetIndexCount();
        }
        if (usesDirectMeshBuffer && preparedDraw.shadowMesh &&
            preparedDraw.shadowMesh->GetIndexBuffer())
        {
            preparedDraw.shadowIndexBuffer =
                preparedDraw.shadowMesh->GetIndexBuffer();
            preparedDraw.shadowIndexCount =
                preparedDraw.shadowMesh->GetIndexCount();
        }
        preparedDraws.push_back(preparedDraw);

        ++slot;
    }
    if(!shadowOnly)
    {
        const size_t count=preparedDraws.size();
        auto* objectBytes=static_cast<uint8_t*>(m_objectDataMapped);
        auto* constantBytes=static_cast<uint8_t*>(m_objectCBMapped);
        const size_t tasks=std::min<size_t>(3,(count+63)/64);
        for(size_t task=0;task<tasks;++task)
        {
            const size_t first=count*task/tasks,last=count*(task+1)/tasks;
            recordSnapshot.jobs.push_back(Engine::Rendering::RenderJobs::Get().Submit([&preparedDraws,objectBytes,constantBytes,first,last] {
                const auto started=RenderClock::now(); DirtyRange dirty;
                for(size_t i=first;i<last;++i)
                {
                    const auto& draw=preparedDraws[i];
                    const uint32_t slot=draw.drawData.objectIndex;
                    std::memcpy(constantBytes+size_t(slot)*kCBStride,&draw.drawData,sizeof(draw.drawData));
                    auto* destination=objectBytes+size_t(slot)*sizeof(ObjectGPUData);
                    if(std::memcmp(destination,&draw.objectData,sizeof(draw.objectData))!=0)
                    { std::memcpy(destination,&draw.objectData,sizeof(draw.objectData)); dirty.first=std::min(dirty.first,slot); dirty.last=std::max(dirty.last,slot); }
                }
                dirty.elapsedMs=std::chrono::duration<double,std::milli>(RenderClock::now()-started).count(); return dirty;
            }));
        }
    }
    m_lastRenderCpuTimings.materialMs = std::chrono::duration<double,std::milli>(RenderClock::now()-materialStart).count();
    m_lastRenderCpuTimings.candidateDraws=preparedDraws.size();
    const auto batchingStart = RenderClock::now();
    using BatchPreparation=Engine::Rendering::DrawBatchPreparation;
    using AutomaticGroup=BatchPreparation::Group;
    using AutomaticKey=Engine::Rendering::DrawBatchKey;
    std::vector<BatchPreparation::Input> batchInput;
    batchInput.reserve(preparedDraws.size());
    for (uint32_t index=0;index<preparedDraws.size();++index)
    {
        auto& draw=preparedDraws[index];
        const bool custom = draw.material &&
            !IsFastFoliageSurface(draw.material) &&
            std::any_of(draw.material->shaders.begin(),draw.material->shaders.end(),
                [](const auto& shader) { return shader.enabled; });
        const bool selected=includeEditorVisuals && (draw.object==m_selectedObject || draw.mesh==m_editorSelectedMesh || draw.mesh==m_editorWeightPaintMesh);
        if ((!automaticInstancing && !(draw.drawData.flags & 0x00100000u)) || m_editorMode2D || hasPortalViews || !draw.mesh || draw.blended ||
            draw.preview || draw.pmxOutline || draw.spatialManipulator || draw.traversalChartPortal ||
            draw.objectData.skinParams.y>0 || draw.mesh->HasMorphTargets() || draw.mesh->HasSurfaceOffsets() ||
            draw.vertexBuffer!=draw.mesh->GetGraphicsBuffer() || custom || selected || !draw.mesh->HasBounds()) continue;
        AutomaticKey key{draw.vertexBuffer,draw.indexBuffer,draw.pipeline,
            draw.textures,draw.vertexStride,draw.vertexCount,draw.indexCount,draw.drawData.lightCount,
            draw.drawData.flags | (draw.terrain ? 0x08000000u : 0u) | (draw.doubleSided ? 0x04000000u : 0u),draw.castsShadow,draw.gpuLod,{},{},automaticInstancing ? nullptr:draw.object};
        if(draw.mesh->IsInstanced())
        {
            for(const auto& range:draw.mesh->GetInstanceRanges()) std::get<11>(key).emplace_back(range.firstVertex,range.vertexCount);
            if(draw.drawData.flags & 0x00100000u) for(int lod=0;lod<=draw.gpuMaximumLod;++lod)
                std::get<12>(key)[lod]=draw.mesh->GetRuntimeLodMesh(lod)->GetGraphicsBuffer();
        }
        batchInput.push_back({std::move(key),draw.drawData.objectIndex,index});
    }
    m_lastRenderCpuTimings.batchEligibleDraws=batchInput.size();
    m_lastRenderCpuTimings.batchSnapshotMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchingStart).count();
    auto& batchCache=m_drawBatchPreparation[shadowOnly ? 1:0];
    BatchPreparation::Result batchResult;
    std::future<BatchPreparation::Result> batchJob;
    struct BatchFence { std::future<BatchPreparation::Result>& job; ~BatchFence() { if(job.valid()) job.wait(); } } batchFence{batchJob};
    auto& batchScheduler=Engine::Rendering::RenderJobs::Get();
    // Small sets stay on the owner. Large copied snapshots can overlap the
    // draw statistics below; workers never inspect live meshes or materials.
    if(batchInput.size()>=256 && batchScheduler.WorkerCount() && batchScheduler.ParallelForEnabled())
    {
        m_lastRenderCpuTimings.batchWorkerJobs=1;
        batchJob=batchScheduler.Submit([&batchCache,input=std::move(batchInput)]() mutable {
            return batchCache.Prepare(std::move(input));
        });
    }
    else batchResult=batchCache.Prepare(std::move(batchInput));
    m_lastOrdinaryDrawCount = 0;
    m_lastOrdinaryTriangleCount = 0;
    for (const PreparedDraw& draw : preparedDraws)
    {
        if (draw.mesh && draw.mesh->IsInstanced())
        {
            for (const auto& range : draw.mesh->GetInstanceRanges())
                if (range.instanceCount && range.vertexCount)
                {
                    ++m_lastOrdinaryDrawCount;
                    m_lastOrdinaryTriangleCount += uint64_t(range.vertexCount / 3) * range.instanceCount;
                }
        }
        else
        {
            ++m_lastOrdinaryDrawCount;
            m_lastOrdinaryTriangleCount += (draw.indexBuffer ? draw.indexCount : draw.vertexCount) / 3;
        }
    }
    if(shadowOnly)
    {
        std::unordered_set<const Engine::Core::Object*> eligibleCasters;
        for(const auto& draw:preparedDraws)
            if(draw.castsShadow && draw.vertexBuffer && !(draw.spatialManipulator && draw.spatialManipulator->enabled))
                eligibleCasters.insert(draw.object);
        m_shadowDebugSnapshot.casterCount=static_cast<uint32_t>(eligibleCasters.size());
    }
    if (shadowOnly) m_shadowDebugSnapshot.submittedCasterDrawCount = 0;

    const auto batchWaitStart=RenderClock::now();
    if(batchJob.valid()) batchResult=batchJob.get();
    m_lastRenderCpuTimings.batchWaitMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchWaitStart).count();
    m_lastRenderCpuTimings.batchBuildMs=batchResult.buildMs;
    m_lastRenderCpuTimings.batchCacheHits=batchResult.reused ? 1:0;
    m_lastRenderCpuTimings.batchReusedKeys=batchResult.reusedKeys;
    auto automaticGroups=std::move(batchResult.groups);
    for (uint32_t group=0;group<automaticGroups.size();++group)
        if (automaticGroups[group].members.size()>1 || preparedDraws[automaticGroups[group].members.front()].gpuLod>=0 || (preparedDraws[automaticGroups[group].members.front()].drawData.flags & 0x00100000u))
            for(auto index:automaticGroups[group].members) preparedDraws[index].automaticGroup=group;
    m_lastRenderCpuTimings.batchingMs=std::chrono::duration<double,std::milli>(RenderClock::now()-batchingStart).count();

    // Register resident placements once per source/view, not once per species,
    // LOD and cascade. Camera changes are compute constants, not placement edits.
    const auto tilePrepareStart=RenderClock::now();
    using TilePreparation=Engine::Rendering::InstanceTilePreparation;
    std::vector<TilePreparation::Input> tileInputs;
    std::vector<size_t> tileDrawIndexes;
    std::unordered_map<const void*,uint32_t> placementBases;
    const auto scaledShadowLodDistances = [&](const glm::vec2 distances)
    {
        if(!shadowOnly) return distances;
        constexpr float shadowLodDistanceScale = .6f;
        const float lod1 = std::max(0.f, distances.x * shadowLodDistanceScale);
        const float lod2 = std::max(lod1, distances.y * shadowLodDistanceScale);
        return glm::vec2(lod1, lod2);
    };
    for(size_t index=0;index<preparedDraws.size();++index)
    {
        const auto& draw=preparedDraws[index];
        if(draw.automaticGroup==UINT32_MAX || !draw.mesh->IsInstanced()) continue;
        const auto& data=draw.mesh->GetInstanceData();
        auto [base,inserted]=placementBases.try_emplace(data.get(),0);
        if(inserted)
            base->second=m_persistentInstances.Register(data,data->revision,data->transforms.data(),
                uint32_t(data->transforms.size()),data->stableId,data->placementIds);
        TilePreparation::Input input;
        input.slot=draw.drawData.objectIndex;
        input.base=base->second;
        input.maximumLod=uint32_t(draw.gpuMaximumLod);
        input.world=draw.objectData.world;
        input.minimum=draw.mesh->GetInstanceSource()->GetBoundsMin();
        input.maximum=draw.mesh->GetInstanceSource()->GetBoundsMax();
        if(draw.drawData.flags & 0x00100000u) for(int lod=1;lod<=draw.gpuMaximumLod;++lod)
        {
            const auto* variant=draw.mesh->GetRuntimeLodMesh(lod)->GetInstanceSource();
            input.minimum=glm::min(input.minimum,variant->GetBoundsMin());
            input.maximum=glm::max(input.maximum,variant->GetBoundsMax());
        }
        if (draw.mesh->grassShadowCaster)
        {
            // Expanded in source space before placement/world transforms so
            // individual GPU-culled patches include the full sway envelope.
            const float padding = 1.5f * std::max(0.f, input.maximum.y);
            input.minimum -= glm::vec3(padding);
            input.maximum += glm::vec3(padding);
        }
        float shadowRange = draw.objectData.shadowCasterFade.x;
        if (shadowOnly && draw.gpuLod >= 0 && draw.mesh->prefabLodBand<0)
        {
            if (draw.gpuLod == 1) shadowRange *= .7f;
            else if (draw.gpuLod >= 2) shadowRange *= .4f;
        }
        input.lod={draw.mesh->prefabLodBand>=0 ? draw.gpuLodDistances : scaledShadowLodDistances(draw.gpuLodDistances),shadowRange,
            std::min(shadowRange, draw.objectData.shadowCasterFade.y)};
        for(const auto& range:draw.mesh->GetInstanceRanges())
            input.ranges.emplace_back(range.firstInstance,range.instanceCount);
        tileInputs.push_back(std::move(input));
        tileDrawIndexes.push_back(index);
    }
    const auto tileResult=m_instanceTilePreparation.Prepare(tileInputs,
        m_graphicsProvider->GetContextFactory()->GetPreparedFrameSerial());
    std::vector<TilePreparation::RecordsOwner> instanceTiles(preparedDraws.size());
    for(size_t i=0;i<tileDrawIndexes.size();++i)
        instanceTiles[tileDrawIndexes[i]]=tileResult.records[i];
    m_lastRenderCpuTimings.instanceTileReused=tileResult.reused;
    m_lastRenderCpuTimings.instanceTileBuilt=tileResult.built;
    m_lastRenderCpuTimings.instanceTileWorkerPasses=tileResult.workerBatches;
    m_lastRenderCpuTimings.instanceTilePrepareMs=std::chrono::duration<double,std::milli>(RenderClock::now()-tilePrepareStart).count();
    Engine::Graphics::IGraphicsBuffer* residentPlacementPool=nullptr;
    if(!tileInputs.empty())
    {
        const auto uploadStart=RenderClock::now();
        const auto allocations=m_persistentInstances.stats.allocations;
        residentPlacementPool=m_persistentInstances.Upload(m_graphicsProvider);
        m_lastRenderCpuTimings.instances.allocations+=m_persistentInstances.stats.allocations-allocations;
        m_lastRenderCpuTimings.uploadMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-uploadStart).count();
    }

    bool recordsUploaded=shadowOnly;
    const auto ensureRecordsUploaded=[&]() {
        if(recordsUploaded) return;
        const auto recordWaitStart=RenderClock::now();
        for(auto& job:recordSnapshot.jobs)
        {
            const auto dirty=job.get(); firstDirtyObject=std::min(firstDirtyObject,dirty.first); lastDirtyObject=std::max(lastDirtyObject,dirty.last);
            m_lastRenderCpuTimings.recordPackingMs+=dirty.elapsedMs;
        }
        recordSnapshot.jobs.clear();
        m_lastRenderCpuTimings.recordWaitMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-recordWaitStart).count();
        const auto uploadStart=RenderClock::now();
        if(firstDirtyObject!=UINT32_MAX)
        {
            const uint64_t bytes=uint64_t(lastDirtyObject-firstDirtyObject+1)*sizeof(ObjectGPUData);
            m_objectDataBuffer->FlushMappedWrites(uint64_t(firstDirtyObject)*sizeof(ObjectGPUData),bytes);
            m_lastObjectDataUploadBytes+=bytes; m_lastRenderCpuTimings.changedUploadBytes+=bytes;
        }
        m_lastRenderCpuTimings.uploadMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-uploadStart).count();
        recordsUploaded=true;
    };
    if(!shadowOnly && !preparedDraws.empty())
    {
        context->SetStructuredBuffer(6,m_lightDataBuffer.get()); context->SetStructuredBuffer(7,m_objectDataBuffer.get());
        context->SetStructuredBuffer(8,m_boneDataBuffer.get()); context->SetStructuredBuffer(10,m_emptyMorphDeltaBuffer.get());
        context->SetStructuredBuffer(11,m_emptyMorphWeightBuffer.get());
        if(clusteredLightingEnabled) { context->SetStructuredBuffer(14,m_clusterHeaderBuffer.get()); context->SetStructuredBuffer(15,m_clusterLightIndexBuffer.get()); }
    }

    const auto isSpatialManipulatorCarrierDraw = [&](const PreparedDraw& draw)
    {
        return draw.spatialManipulator && draw.spatialManipulator->enabled;
    };

    const auto geometryPipeline = [&](const PreparedDraw& draw,
        Engine::Graphics::IPipelineState* base)
    {
        if (!draw.terrain)
            return base;
        const auto terrain = m_terrainPipelineByBase.find(base);
        return terrain != m_terrainPipelineByBase.end()
            ? terrain->second.get() : base;
    };

    const auto submitGeometry = [&](const PreparedDraw& draw,
        bool shadowGeometry = false)
    {
        auto* geometryMesh = shadowGeometry && draw.shadowMesh
            ? draw.shadowMesh : draw.mesh;
        auto* geometryVertexBuffer = shadowGeometry && draw.shadowVertexBuffer
            ? draw.shadowVertexBuffer : draw.vertexBuffer;
        auto* geometryIndexBuffer = shadowGeometry && draw.shadowIndexBuffer
            ? draw.shadowIndexBuffer : draw.indexBuffer;
        const uint32_t geometryVertexStride = shadowGeometry &&
            draw.shadowVertexStride > 0
            ? draw.shadowVertexStride : draw.vertexStride;
        const uint32_t geometryVertexCount = shadowGeometry &&
            draw.shadowVertexCount > 0
            ? draw.shadowVertexCount : draw.vertexCount;
        const uint32_t geometryIndexCount = shadowGeometry &&
            draw.shadowIndexCount > 0
            ? draw.shadowIndexCount : draw.indexCount;
        if(geometryMesh && geometryMesh->IsInstanced())
        {
            auto& instances=*geometryMesh->GetInstanceData();
            if(instances.slicesRevision!=instances.revision)
            {
                for(auto& slice:instances.slices) m_persistentInstances.Retire(std::move(slice.buffer));
                instances.slices.clear(); instances.slicesRevision=instances.revision;
            }
            for(const auto& range:geometryMesh->GetInstanceRanges())
                if(range.instanceCount && !geometryMesh->GetInstanceBuffer(range.firstInstance))
                {
                    Engine::Components::Mesh::InstanceData::Slice slice;
                    slice.firstInstance=range.firstInstance; slice.instanceCount=range.instanceCount;
                    slice.buffer=m_graphicsProvider->GetBufferFactory()->CreateStaticShaderResource(
                        size_t(range.instanceCount)*sizeof(Engine::Components::Mesh::InstanceTransform),
                        instances.transforms.data()+range.firstInstance,sizeof(Engine::Components::Mesh::InstanceTransform));
                    ++m_lastRenderCpuTimings.instances.allocations;
                    m_lastRenderCpuTimings.instances.fallbackBytes+=slice.buffer->GetSize();
                    instances.slices.push_back(std::move(slice));
                }
        }
        context->SetStructuredBuffer(10, draw.morphDeltaBuffer);
        context->SetStructuredBuffer(11, draw.morphWeightBuffer);
        context->SetStructuredBuffer(45, geometryMesh && geometryMesh->IsInstanced()
            ? geometryMesh->GetInstanceBuffer() : m_emptyMorphDeltaBuffer.get());
        context->SetStructuredBuffer(46,m_emptyMorphDeltaBuffer.get());
        context->SetVertexBuffer(0, geometryVertexBuffer, geometryVertexStride, 0);
        if (geometryMesh && geometryMesh->IsInstanced())
        {
            for (const auto& range : geometryMesh->GetInstanceRanges())
                if (range.instanceCount && range.vertexCount)
                {
                    // SV_InstanceID begins at zero on D3D. Bind the compact
                    // species slice rather than relying on base-instance IDs.
                    context->SetStructuredBuffer(45, geometryMesh->GetInstanceBuffer(range.firstInstance));
                    context->DrawInstanced(range.vertexCount, range.instanceCount,
                        range.firstVertex, 0);
                }
        }
        else if (geometryIndexBuffer && geometryIndexCount > 0u)
        {
            context->SetIndexBuffer(geometryIndexBuffer, geometryIndexCount, 0);
            context->DrawIndexedInstanced(geometryIndexCount, 1, 0, 0, 0);
        }
        else
            context->DrawInstanced(geometryVertexCount, 1, 0, 0);
    };

    const auto submitMaterialGeometry = [&](const PreparedDraw& draw,
        Engine::Graphics::IPipelineState* base,
        const std::function<void()>& restoreState = {})
    {
        bool submitted = false;
        if (draw.material)
            for (const auto& shader : draw.material->shaders)
            {
                if (!shader.enabled) continue;
                std::string error;
                auto* pipeline = ResolveMaterialShaderPipeline(
                    ResolveMaterialShaderSource(*draw.material,shader.path), base,
                    draw.terrain, submitted, shader.blendMode, error);
                if (!error.empty() && draw.material->shaderError.find(error) == std::string::npos)
                {
                    if (!draw.material->shaderError.empty()) draw.material->shaderError += "\n";
                    draw.material->shaderError += error;
                }
                // An invalid surface falls back to the built-in surface;
                // invalid overlays are skipped without hiding the mesh.
                if (!pipeline && submitted) continue;
                context->SetPipeline(pipeline ? pipeline : base);
                if (restoreState) restoreState();
                submitGeometry(draw);
                submitted = true;
            }
        if (!submitted)
        {
            context->SetPipeline(base);
            if (restoreState) restoreState();
            submitGeometry(draw);
        }
    };

    std::vector<uint8_t> selectedLods(preparedDraws.size(),1);
    Engine::Rendering::RenderJobs::Get().For(preparedDraws.size(),512,[&](size_t index) {
        const auto& draw=preparedDraws[index];
        if(draw.gpuLod<0 || draw.mesh->prefabLodBand>=0) return;
        const glm::vec2 lodDistances=scaledShadowLodDistances(draw.gpuLodDistances);
        const float distance=glm::length(glm::vec3(draw.objectData.world[3])-cameraPosition);
        const int lod=distance<lodDistances.x ? 0 : distance<lodDistances.y ? 1 : 2;
        if(draw.mesh->IsInstanced())
        {
            const float width=std::max(.001f,std::min(.5f,(lodDistances.y-lodDistances.x)*.25f));
            selectedLods[index]=draw.gpuLod==0 ? distance<=lodDistances.x+width:
                draw.gpuLod==1 ? distance>=lodDistances.x-width && distance<=lodDistances.y+width:
                distance>=lodDistances.y-width;
        }
        else selectedLods[index]=std::min(lod,draw.gpuMaximumLod)==draw.gpuLod;
    });
    const auto lodSelected = [&](const PreparedDraw& draw) {
        return selectedLods[size_t(&draw-preparedDraws.data())]!=0;
    };
    const auto submitAutomaticGroup = [&](const AutomaticGroup& group,
        const glm::mat4& cullViewProjection, uint32_t objectBase,
        Engine::Graphics::IPipelineState* pipeline, const std::vector<uint8_t>* visibility)->uint32_t
    {
        const auto& representative=preparedDraws[group.members.front()];
        if(representative.mesh->IsInstanced())
        {
            uint32_t submitted=0;
            const bool gpuLod=(representative.drawData.flags & 0x00100000u)!=0;
            const int lodCount=gpuLod ? representative.gpuMaximumLod+1:1;
            for(int residentLod=0;residentLod<lodCount;++residentLod)
            {
            auto* sourceMesh=gpuLod ? representative.mesh->GetRuntimeLodMesh(residentLod):representative.mesh;
            for(size_t rangeIndex=0;rangeIndex<sourceMesh->GetInstanceRanges().size();++rangeIndex)
            {
                const auto& sourceRange=sourceMesh->GetInstanceRanges()[rangeIndex];
                if(!sourceRange.vertexCount) continue;
                std::vector<Engine::Rendering::InstanceTileRecord> tiles;
                uint32_t count=0;
                for(auto index:group.members)
                {
                    if(visibility && !(*visibility)[index]) continue;
                    const auto& tile=(*instanceTiles[index])[rangeIndex];
                    if(!tile.range.y) continue;
                    tiles.push_back(tile); count+=tile.range.y;
                }
                if(!count) continue;
                // Camera sorting must not invalidate unchanged tile metadata.
                std::sort(tiles.begin(),tiles.end(),[](const auto& first,const auto& second) {
                    return std::tie(first.range.z,first.range.x)<std::tie(second.range.z,second.range.x);
                });
                bool gpu=gpuInstanceCulling && context->SupportsIndirectDraws() && (count>=32 || gpuLod || representative.mesh->prefabLodBand>=0);
                if(gpu && !m_instanceCullAttempted)
                {
                    m_instanceCullAttempted=true;
                    auto shader=m_graphicsProvider->GetShaderCompiler()->CompileFromFile(
                        EngineShaderPath("InstanceCull.hlsl"),"CSMain",Engine::Graphics::IShaderCompiler::CompileProfile::CS_5_0);
                    if(shader) m_instanceCullPipeline=m_graphicsProvider->GetPipelineStateFactory()->CreateComputePipeline(shader.get());
                }
                gpu=gpu && m_instanceCullPipeline;
                if(gpu) gpu=context->BeginComputePass();
                DrawCBData constants=representative.drawData;
                constants.objectIndex=objectBase; constants.flags|=(uint32_t(residentLod)<<21)|0x00080000u;
                if(shadowOnly) { constants.lightCount=0; constants.flags&=~kClusteredLightingFlag; }
                Engine::Rendering::PersistentInstanceBatches::Key key{
                    uintptr_t(cam),uintptr_t(shadowOnly),instanceViewOrdinal,objectBase/m_objectCapacity,
                    uintptr_t(sourceMesh->GetGraphicsBuffer()),uintptr_t(pipeline),sourceRange.firstVertex,sourceRange.vertexCount,
                    uintptr_t(residentLod) | (uintptr_t(representative.mesh->prefabLodBand+1)<<8),representative.drawData.flags,representative.drawData.lightCount,uintptr_t(representative.doubleSided),uintptr_t(representative.castsShadow)};
                for(auto* texture:representative.textures) key.push_back(uintptr_t(texture));
                if(!automaticInstancing) key.push_back(uintptr_t(representative.object));
                const auto uploadStart=RenderClock::now();
                auto* pool=residentPlacementPool;
                auto& buffers=m_persistentBatches.Acquire(m_graphicsProvider,std::move(key),gpu,tiles,&constants,m_persistentInstances,m_lastRenderCpuTimings.changedUploadBytes);
                m_lastRenderCpuTimings.uploadMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-uploadStart).count();
                m_lastRenderCpuTimings.grassBatchCacheHits+=!buffers.metadataChanged;
                auto& storage=m_lastRenderCpuTimings.instances;
                storage.metadataBytes+=buffers.metadataUploadBytes; storage.visibleIdBytes+=buffers.idUploadBytes;
                storage.allocations+=buffers.allocations;
                if(buffers.cold) ++storage.cacheCold;
                else if(buffers.modeChanged) ++storage.cacheMode;
                else if(buffers.membershipChanged) ++storage.cacheMembership;
                else if(buffers.metadataChanged) ++storage.cacheMetadata;
                if(gpu)
                {
                    ++m_lastRenderCpuTimings.gpuGrassBatches;
                    if(shadowOnly) ++m_lastShadowCpuTiming.gpuGrassBatches;
                    context->SetStructuredBuffer(45,m_emptyMorphDeltaBuffer.get());
                    context->SetComputePipeline(m_instanceCullPipeline.get());
                    Engine::Rendering::InstanceCullConstants cull;
                    cull.viewProjection=cullViewProjection; cull.cameraPosition=glm::vec4(cameraPosition,1);
                    cull.counts={uint32_t(tiles.size()),sourceRange.vertexCount,sourceRange.firstVertex,1};
                    cull.selection={gpuLod ? residentLod:representative.gpuLod,2,representative.gpuMaximumLod,
                        (shadowOnly ? 1:0) | (representative.mesh->prefabLodBand>=0 ? 2:0)};
                    context->SetComputeReadBuffer(buffers.input.get()); context->SetComputeReadBuffer(pool,1); context->SetComputeWriteBuffer(buffers.output.get());
                    context->SetComputeConstants(&cull,sizeof(cull)); context->DispatchCompute(1); context->StorageBarrier(buffers.output.get());
                    cull.counts.w=0;
                    context->SetComputePipeline(m_instanceCullPipeline.get());
                    context->SetComputeReadBuffer(buffers.input.get()); context->SetComputeReadBuffer(pool,1); context->SetComputeWriteBuffer(buffers.output.get());
                    context->SetComputeConstants(&cull,sizeof(cull)); context->DispatchCompute(uint32_t(tiles.size()));
                    context->StorageBarrier(buffers.output.get()); context->EndComputePass(); ++m_lastRenderCpuTimings.gpuBatches;
                }
                ensureRecordsUploaded();
                context->SetPipeline(pipeline); context->SetConstantBuffer(0,buffers.draw.get());
                context->SetStructuredBuffer(45,buffers.output.get()); context->SetStructuredBuffer(46,pool);
                context->SetStructuredBuffer(10,m_emptyMorphDeltaBuffer.get()); context->SetStructuredBuffer(11,m_emptyMorphWeightBuffer.get());
                context->SetVertexBuffer(0,sourceMesh->GetGraphicsBuffer(),sourceMesh->GetVertexStride(),0);
                if(gpu) context->DrawInstancesIndirect(buffers.output.get(),0,false);
                else context->DrawInstanced(sourceRange.vertexCount,buffers.candidates,sourceRange.firstVertex,0);
                ++submitted; ++m_lastRenderCpuTimings.automaticBatches;
                m_lastRenderCpuTimings.automaticInstances+=buffers.candidates;
            }
            }
            const auto& stats=m_persistentInstances.stats;
            auto& storage=m_lastRenderCpuTimings.instances;
            storage.newBytes=stats.newBytes-poolBefore.newBytes;
            storage.editedBytes=stats.editedBytes-poolBefore.editedBytes;
            storage.growthBytes=stats.growthBytes-poolBefore.growthBytes;
            storage.residentBytes=m_persistentInstances.ResidentBytes();
            storage.preparedRecords=stats.preparedRecords-poolBefore.preparedRecords;
            storage.preparationWorkerPasses=stats.workerPasses-poolBefore.workerPasses;
            m_lastRenderCpuTimings.grassPlacementUploadBytes=stats.newBytes+stats.editedBytes+stats.growthBytes-
                poolBefore.newBytes-poolBefore.editedBytes-poolBefore.growthBytes;
            return submitted;
        }
        bool gpu=gpuInstanceCulling && context->SupportsIndirectDraws() &&
            (group.members.size()>=32 || representative.gpuLod>=0);
        if (gpu && !m_instanceCullAttempted)
        {
            m_instanceCullAttempted=true;
            auto shader=m_graphicsProvider->GetShaderCompiler()->CompileFromFile(
                EngineShaderPath("InstanceCull.hlsl"),"CSMain",Engine::Graphics::IShaderCompiler::CompileProfile::CS_5_0);
            if(shader) m_instanceCullPipeline=m_graphicsProvider->GetPipelineStateFactory()->CreateComputePipeline(shader.get());
        }
        gpu=gpu && m_instanceCullPipeline;
        if(gpu) gpu=context->BeginComputePass();
        auto& instances=m_automaticInstanceCache.scratch;
        instances.clear();
        for(auto index:group.members)
        {
            const auto& draw=preparedDraws[index];
            if(visibility && !(*visibility)[index]) continue;
            if(!gpu && !lodSelected(draw)) continue;
            const auto bounds=Engine::Rendering::TransformBounds(draw.mesh->GetBoundsMin(),draw.mesh->GetBoundsMax(),draw.objectData.world);
            Engine::Rendering::AutomaticInstance instance;
            instance.sphere=glm::vec4((bounds.minimum+bounds.maximum)*.5f,glm::length((bounds.maximum-bounds.minimum)*.5f));
            const auto origin=glm::vec3(draw.objectData.world[3]);
            instance.meta={draw.drawData.objectIndex,uint32_t(draw.gpuMaximumLod),
                glm::floatBitsToUint(origin.x),glm::floatBitsToUint(origin.y)};
            instance.lod={draw.gpuLodDistances,origin.z,0}; instances.push_back(instance);
        }
        if(instances.empty()) { if(gpu) context->EndComputePass(); return 0; }
        DrawCBData constants=representative.drawData;
        constants.objectIndex=objectBase; constants.flags|=0x10000000u;
        if(shadowOnly) { constants.lightCount=0; constants.flags&=~kClusteredLightingFlag; }
        const auto batchUploadStart=RenderClock::now();
        auto& buffers=m_automaticInstanceCache.Acquire(m_graphicsProvider,gpu,instances,&constants,m_lastRenderCpuTimings.changedUploadBytes);
        m_lastRenderCpuTimings.uploadMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-batchUploadStart).count();
        if(gpu)
        {
            context->SetStructuredBuffer(45,m_emptyMorphDeltaBuffer.get());
            context->SetComputePipeline(m_instanceCullPipeline.get());
            Engine::Rendering::InstanceCullConstants cull;
            cull.viewProjection=cullViewProjection; cull.cameraPosition=glm::vec4(cameraPosition,1);
            cull.counts={uint32_t(instances.size()),representative.indexBuffer ? representative.indexCount : representative.vertexCount,
                representative.indexBuffer ? 1u:0u,1u}; cull.selection.x=representative.gpuLod;
            context->SetComputeReadBuffer(buffers.input.get()); context->SetComputeWriteBuffer(buffers.output.get());
            context->SetComputeConstants(&cull,sizeof(cull)); context->DispatchCompute(1); context->StorageBarrier(buffers.output.get());
            cull.counts.w=0;
            context->SetComputePipeline(m_instanceCullPipeline.get());
            context->SetComputeWriteBuffer(buffers.output.get()); context->SetComputeReadBuffer(buffers.input.get());
            context->SetComputeConstants(&cull,sizeof(cull)); context->DispatchCompute((uint32_t(instances.size())+63)/64);
            context->StorageBarrier(buffers.output.get()); context->EndComputePass(); ++m_lastRenderCpuTimings.gpuBatches;
        }
        ensureRecordsUploaded();
        context->SetPipeline(pipeline);
        context->SetConstantBuffer(0,buffers.draw.get());
        context->SetStructuredBuffer(45,buffers.output.get());
        context->SetStructuredBuffer(46,m_emptyMorphDeltaBuffer.get());
        context->SetStructuredBuffer(10,m_emptyMorphDeltaBuffer.get());
        context->SetStructuredBuffer(11,m_emptyMorphWeightBuffer.get());
        context->SetVertexBuffer(0,representative.vertexBuffer,representative.vertexStride,0);
        if(representative.indexBuffer) context->SetIndexBuffer(representative.indexBuffer,representative.indexCount,0);
        if(gpu) context->DrawInstancesIndirect(buffers.output.get(),0,representative.indexBuffer!=nullptr);
        else if(representative.indexBuffer) context->DrawIndexedInstanced(representative.indexCount,uint32_t(instances.size()));
        else context->DrawInstanced(representative.vertexCount,uint32_t(instances.size()));
        ++m_lastRenderCpuTimings.automaticBatches; m_lastRenderCpuTimings.automaticInstances+=instances.size();
        return 1;
    };

    const auto outsideShadowCascade = [&](const PreparedDraw& draw,
        uint32_t cascade)
    {
        if (!draw.mesh || !draw.mesh->HasBounds() ||
            (!draw.terrainCell && !draw.mesh->useBoundsForShadowCulling)) return false;
        const glm::vec3 minimum = draw.mesh->GetBoundsMin();
        const glm::vec3 maximum = draw.mesh->GetBoundsMax();
        if(mainShadowCascades.receivers[cascade].Outside(minimum,maximum,draw.objectData.world)) return true;
        const glm::vec3 center = (minimum + maximum) * 0.5f;
        const glm::vec3 extent = (maximum - minimum) * 0.5f;
        const glm::mat4 localToClip = shadowViewProjections[cascade] *
            draw.objectData.world;
        const glm::vec4 xRow(localToClip[0][0], localToClip[1][0],
            localToClip[2][0], localToClip[3][0]);
        const glm::vec4 yRow(localToClip[0][1], localToClip[1][1],
            localToClip[2][1], localToClip[3][1]);
        const glm::vec4 zRow(localToClip[0][2], localToClip[1][2],
            localToClip[2][2], localToClip[3][2]);
        const glm::vec4 wRow(localToClip[0][3], localToClip[1][3],
            localToClip[2][3], localToClip[3][3]);
        const glm::vec4 homogeneousCenter(center, 1.f);
        // A clip-plane equation is linear over the local AABB. Its maximum
        // occurs at a corner selected by the signs of its XYZ coefficients;
        // evaluating that maximum avoids transforming eight corners per plane.
        const auto allOutside = [&](const glm::vec4& plane)
        {
            const float centerValue = glm::dot(plane, homogeneousCenter);
            const float radius = glm::dot(glm::abs(glm::vec3(plane)), extent);
            return centerValue + radius < -1e-4f;
        };
        return allOutside(wRow + xRow) || allOutside(wRow - xRow) ||
            allOutside(wRow + yRow) || allOutside(wRow - yRow) ||
            allOutside(zRow) || allOutside(wRow - zRow);
    };

    // Render the selected realtime directional light into its depth map during
    // the explicit pre-view phase. This keeps Vulkan render passes unnested and
    // lets DX12 bind a depth-only target without reconstructing prior OM state.
    const auto shadowPreparationEnd = std::chrono::steady_clock::now();
    if (shadowOnly)
        m_lastShadowCpuTiming.scenePreparationMs = shadowElapsedMs(
            shadowCpuStart, shadowPreparationEnd);
    if (shadowOnly && shadowSelection.valid && renderMainDirectionalShadow &&
        m_directionalShadowPipeline &&
        context->BeginDepthOnlyPass(m_directionalShadowMap.get(),
            preserveDirectionalShadowAtlas ? -1.f : 1.f))
    {
        context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::Shadow);
        std::array<std::vector<uint8_t>,4> cascadeVisible;
        // Each cascade owns its visibility and receiver results. The spatial
        // tree is immutable for this render, and uncached queries leave the
        // single-owner view cache untouched.
        Engine::Rendering::RenderJobs::Get().For(shadowCascadeCount,1,[&](size_t cascade)
        {
            const auto frameVisible=hasPortalViews ? std::vector<uint8_t>{} :
                m_renderSpatialIndex.QueryUncached(shadowViewProjections[cascade]);
            auto& visible=cascadeVisible[cascade];
            visible.resize(preparedDraws.size(),0);
            for(size_t drawIndex=0;drawIndex<preparedDraws.size();++drawIndex)
            {
                const PreparedDraw& draw=preparedDraws[drawIndex];
                if (!draw.castsShadow || !draw.shadowVertexBuffer ||
                    isSpatialManipulatorCarrierDraw(draw)) continue;
                if(!frameVisible.empty() &&
                    !frameVisible[frameRenderItems[draw.frameItemIndex].objectSlot]) continue;
                visible[drawIndex]=!outsideShadowCascade(draw,static_cast<uint32_t>(cascade));
            }
            const uint32_t cascadeCasterBudget =
                m_realtimeShadowSettings.directionalCascadeCasterBudget;
            if (cascadeCasterBudget > 0u)
            {
                std::vector<std::pair<float, size_t>> candidates;
                candidates.reserve(preparedDraws.size());
                const glm::vec3 cascadeCenter =
                    mainShadowCascades.centers[cascade];
                for (size_t drawIndex = 0; drawIndex < preparedDraws.size();
                    ++drawIndex)
                {
                    if (!visible[drawIndex])
                        continue;
                    const PreparedDraw& draw = preparedDraws[drawIndex];
                    const glm::vec3 origin =
                        glm::vec3(draw.objectData.world[3]);
                    const float distance =
                        glm::length(origin - cascadeCenter);
                    const float lodPenalty = draw.gpuLod >= 0
                        ? static_cast<float>(draw.gpuLod) * 8.f : 0.f;
                    const float grassPenalty =
                        draw.mesh && draw.mesh->grassShadowCaster ? 10.f : 0.f;
                    const float terrainPenalty = draw.terrainCell ? 4.f : 0.f;
                    candidates.emplace_back(distance + lodPenalty +
                        grassPenalty + terrainPenalty, drawIndex);
                }
                if (candidates.size() > cascadeCasterBudget)
                {
                    std::nth_element(candidates.begin(),
                        candidates.begin() + cascadeCasterBudget,
                        candidates.end(),
                        [](const auto& left, const auto& right)
                        {
                            return left.first == right.first
                                ? left.second < right.second
                                : left.first < right.first;
                        });
                    for (size_t index = cascadeCasterBudget;
                        index < candidates.size(); ++index)
                        visible[candidates[index].second] = 0;
                }
            }
        });
        std::vector<uint8_t> cascadeMasks(preparedDraws.size(), 0u);
        for(size_t drawIndex=0;drawIndex<preparedDraws.size();++drawIndex)
        {
            for (uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade)
                if (cascadeVisible[cascade][drawIndex])
                    cascadeMasks[drawIndex] |= static_cast<uint8_t>(1u << cascade);
        }
        const auto shadowCullingEnd = std::chrono::steady_clock::now();
        m_lastShadowCpuTiming.cascadeCullingMs = shadowElapsedMs(
            shadowPreparationEnd, shadowCullingEnd);
        std::array<uint32_t,4> firstDirtyByCascade, lastDirtyByCascade{};
        firstDirtyByCascade.fill(UINT32_MAX);
        Engine::Rendering::RenderJobs::Get().For(shadowCascadeCount,1,[&](size_t cascade)
        {
            for (size_t drawIndex = 0; drawIndex < preparedDraws.size(); ++drawIndex)
            {
                if (!(cascadeMasks[drawIndex] & (1u << cascade)))
                    continue;
                const PreparedDraw& draw = preparedDraws[drawIndex];
                const uint32_t recordIndex = static_cast<uint32_t>(cascade) * m_objectCapacity +
                    draw.drawData.objectIndex;
                ObjectGPUData shadowData = draw.objectData;
                shadowData.viewProjection = shadowViewProjections[cascade];
                shadowData.shadowCasterFade.z=1.f;
                shadowData.materialParams.w = static_cast<float>(
                    static_cast<uint32_t>(shadowData.materialParams.w) &
                    (1u | 32u));
                shadowData.ambientUnlit.w = 1.f;
                shadowData.environmentParams = glm::vec4(0.f);
                auto* destination=static_cast<uint8_t*>(m_shadowObjectDataMapped)+size_t(recordIndex)*sizeof(ObjectGPUData);
                if(std::memcmp(destination,&shadowData,sizeof(shadowData))!=0)
                {
                    std::memcpy(destination,&shadowData,sizeof(shadowData));
                    firstDirtyByCascade[cascade]=std::min(firstDirtyByCascade[cascade],recordIndex);
                    // Camera-distance sorting is independent of persistent
                    // slot order. A later draw may have a lower slot; every
                    // changed record must remain inside the upload range.
                    lastDirtyByCascade[cascade]=std::max(lastDirtyByCascade[cascade],recordIndex);
                }
                DrawCBData shadowDraw = draw.drawData;
                shadowDraw.objectIndex = recordIndex;
                shadowDraw.lightCount = 0;
                shadowDraw.flags &= ~kClusteredLightingFlag;
                std::memcpy(static_cast<uint8_t*>(m_shadowCBMapped) +
                    static_cast<uint64_t>(recordIndex) * kCBStride,
                    &shadowDraw, sizeof(shadowDraw));
            }
        });
        uint32_t firstDirtyShadow=UINT32_MAX,lastDirtyShadow=0;
        for (uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade)
        {
            firstDirtyShadow=std::min(firstDirtyShadow,firstDirtyByCascade[cascade]);
            lastDirtyShadow=std::max(lastDirtyShadow,lastDirtyByCascade[cascade]);
        }
        if(firstDirtyShadow!=UINT32_MAX)
        {
            const uint64_t bytes=uint64_t(lastDirtyShadow-firstDirtyShadow+1)*sizeof(ObjectGPUData);
            m_shadowObjectDataBuffer->FlushMappedWrites(uint64_t(firstDirtyShadow)*sizeof(ObjectGPUData),bytes);
            m_lastRenderCpuTimings.changedUploadBytes+=bytes;
        }
        context->SetStructuredBuffer(6, m_lightDataBuffer.get());
        context->SetStructuredBuffer(7, m_shadowObjectDataBuffer.get());
        context->SetStructuredBuffer(8, m_boneDataBuffer.get());
        context->PrepareConstantBuffer(m_shadowConstantBuffer.get(),
            static_cast<uint64_t>(shadowCascadeCount) * m_objectCapacity * kCBStride);
        const auto shadowRecordsEnd = std::chrono::steady_clock::now();
        m_lastShadowCpuTiming.recordUploadMs = shadowElapsedMs(
            shadowCullingEnd, shadowRecordsEnd);

        const auto threadCycles = []()->uint64_t {
#if defined(_WIN32)
            ULONG64 cycles=0; QueryThreadCycleTime(GetCurrentThread(),&cycles); return cycles;
#else
            return 0;
#endif
        };
        const auto submissionCyclesStart=m_updateProfilingEnabled ? threadCycles():0;
        const uint32_t tileResolution = shadowCascadeCount > 1u
            ? m_directionalShadowResolution / 2u
            : m_directionalShadowResolution;
        std::array<const Engine::Graphics::IGraphicsTexture*, 30> shadowTextures{};
        bool shadowTexturesBound = false;
        for (uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade)
        {
            if (!directionalShadowDirtyMask[cascade])
                continue;
            const uint32_t tileX = shadowCascadeCount > 1u
                ? cascade & 1u : 0u;
            const uint32_t tileY = shadowCascadeCount > 1u
                ? cascade >> 1u : 0u;
            context->SetViewport({
                static_cast<float>(tileX * tileResolution),
                static_cast<float>(tileY * tileResolution),
                static_cast<float>(tileResolution),
                static_cast<float>(tileResolution), 0.f, 1.f });
            context->SetScissorRect({
                static_cast<int32_t>(tileX * tileResolution),
                static_cast<int32_t>(tileY * tileResolution),
                static_cast<int32_t>((tileX + 1u) * tileResolution),
                static_cast<int32_t>((tileY + 1u) * tileResolution) });
            std::vector<uint8_t> automaticSubmitted(automaticGroups.size(),0);
            for (size_t drawIndex = 0; drawIndex < preparedDraws.size(); ++drawIndex)
            {
                if (!(cascadeMasks[drawIndex] & (1u << cascade)))
                    continue;
                const PreparedDraw& draw = preparedDraws[drawIndex];
                if(draw.automaticGroup!=UINT32_MAX && automaticSubmitted[draw.automaticGroup]) continue;
                if(draw.automaticGroup==UINT32_MAX && !lodSelected(draw)) continue;

                Engine::Graphics::IPipelineState* shadowPipeline =
                    draw.shadowDoubleSided
                        ? m_directionalShadowDoubleSidedPipeline.get()
                        : m_directionalShadowPipeline.get();
                const auto bindStarted = m_updateProfilingEnabled
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                context->SetPipeline(geometryPipeline(draw, shadowPipeline));
                const uint32_t recordIndex = cascade * m_objectCapacity +
                    draw.drawData.objectIndex;
                context->SetConstantBuffer(0, m_shadowConstantBuffer.get(),
                    static_cast<uint64_t>(recordIndex) * kCBStride);
                const auto afterConstants = m_updateProfilingEnabled
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                for (uint32_t textureSlot = 0;
                    textureSlot < 12; ++textureSlot)
                {
                    if (textureSlot != 7u && (!shadowTexturesBound ||
                        shadowTextures[textureSlot] != draw.textures[textureSlot]))
                        context->SetTexture(textureSlot, draw.textures[textureSlot]);
                }
                shadowTextures = draw.textures;
                shadowTexturesBound = true;
                const auto beforeGeometry = m_updateProfilingEnabled
                    ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                if(draw.automaticGroup!=UINT32_MAX)
                {
                    automaticSubmitted[draw.automaticGroup]=1;
                    const auto draws=submitAutomaticGroup(automaticGroups[draw.automaticGroup],
                        shadowViewProjections[cascade],cascade*m_objectCapacity,geometryPipeline(draw,shadowPipeline),&cascadeVisible[cascade]);
                    m_shadowDebugSnapshot.submittedCasterDrawCount+=draws;
                    m_lastShadowCpuTiming.automaticDraws+=draws;
                    if(m_updateProfilingEnabled)
                    {
                        m_lastShadowCpuTiming.constantBindingMs+=shadowElapsedMs(bindStarted,afterConstants);
                        m_lastShadowCpuTiming.textureBindingMs+=shadowElapsedMs(afterConstants,beforeGeometry);
                        const double batchMs=shadowElapsedMs(beforeGeometry,std::chrono::steady_clock::now());
                        m_lastShadowCpuTiming.geometryDrawMs+=batchMs;
                        m_lastShadowCpuTiming.maxBatchMs=std::max(m_lastShadowCpuTiming.maxBatchMs,batchMs);
                    }
                    continue;
                }
                submitGeometry(draw, true);
                if (m_updateProfilingEnabled)
                {
                    m_lastShadowCpuTiming.constantBindingMs += shadowElapsedMs(bindStarted, afterConstants);
                    m_lastShadowCpuTiming.textureBindingMs += shadowElapsedMs(afterConstants, beforeGeometry);
                    const double batchMs=shadowElapsedMs(beforeGeometry,std::chrono::steady_clock::now());
                    m_lastShadowCpuTiming.geometryDrawMs += batchMs;
                    m_lastShadowCpuTiming.maxBatchMs=std::max(m_lastShadowCpuTiming.maxBatchMs,batchMs);
                }
                if (draw.mesh && draw.mesh->IsInstanced())
                {
                    const auto draws=static_cast<uint32_t>(std::count_if(
                        draw.mesh->GetInstanceRanges().begin(), draw.mesh->GetInstanceRanges().end(),
                        [](const auto& range) { return range.instanceCount && range.vertexCount; }));
                    m_shadowDebugSnapshot.submittedCasterDrawCount+=draws;
                    m_lastShadowCpuTiming.explicitInstanceDraws+=draws;
                }
                else { ++m_shadowDebugSnapshot.submittedCasterDrawCount; ++m_lastShadowCpuTiming.ordinaryDraws; }
            }
        }
        if(m_updateProfilingEnabled)
        {
            m_lastShadowCpuTiming.submissionThreadCycles=threadCycles()-submissionCyclesStart;
            m_lastShadowCpuTiming.submissionWallMs=shadowElapsedMs(shadowRecordsEnd,std::chrono::steady_clock::now());
        }
        const auto restoreStarted = std::chrono::steady_clock::now();
        context->EndDepthOnlyPass();
        context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Shadow);
        m_lastShadowCpuTiming.passRestoreMs = shadowElapsedMs(
            restoreStarted, std::chrono::steady_clock::now());
        m_lastShadowCpuTiming.drawSubmissionMs = shadowElapsedMs(
            shadowRecordsEnd, std::chrono::steady_clock::now());
        m_directionalShadowCacheValid = true;
        m_directionalShadowLastRenderedFrame =
            m_directionalShadowFrameCounter;
        m_directionalShadowCachedResolution =
            m_directionalShadowResolution;
        m_directionalShadowCachedCascadeCount =
            mainShadowCascades.count;
        m_directionalShadowCachedLightKey =
            shadowSelection.stableLightKey;
        m_directionalShadowCachedCameraPosition = cameraPosition;
        m_directionalShadowCachedDirection =
            glm::normalize(shadowSelection.directionToLight);
        for (uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade)
            if (directionalShadowDirtyMask[cascade])
            {
                m_directionalShadowCachedViewProjections[cascade] =
                    mainShadowCascades.viewProjections[cascade];
                m_directionalShadowCachedCascadeCenters[cascade] =
                    mainShadowCascades.centers[cascade];
                m_directionalShadowCachedCascadeRadii[cascade] =
                    mainShadowCascades.radii[cascade];
                m_directionalShadowCascadeLastRenderedFrame[cascade] =
                    m_directionalShadowFrameCounter;
            }
        m_directionalShadowCachedCascadeSplits =
            mainShadowCascades.splits;
        m_directionalShadowCachedCascadeData =
            mainShadowCascades.data;
        m_directionalShadowCachedCameraData =
            mainShadowCascades.cameraData;

    }
    else if (shadowOnly && shadowSelection.valid &&
        !renderMainDirectionalShadow)
    {
        m_lastShadowCpuTiming.cascadeCullingMs = 0.0;
        m_lastShadowCpuTiming.recordUploadMs = 0.0;
        m_lastShadowCpuTiming.drawSubmissionMs = 0.0;
        m_lastShadowCpuTiming.passRestoreMs = 0.0;
    }

    const auto portalStart = std::chrono::steady_clock::now();
    if (shadowOnly && shadowSelection.valid &&
        m_realtimeShadowSettings.portalPolicy !=
            Engine::Model::PortalShadowPolicy::ReuseMain)
    {
        auto* bufferFactory = m_graphicsProvider->GetBufferFactory();
        auto* textureFactory = m_graphicsProvider->GetTextureFactory();
        const uint32_t portalResolution = std::clamp(static_cast<uint32_t>(
            static_cast<float>(m_realtimeShadowSettings.portalAtlasResolution) *
            shadowSelection.resolutionScale), 256u, 4096u);
        const uint32_t portalCascadeCount = std::clamp(
            m_realtimeShadowSettings.portalCascadeCount, 1u, 4u);
        for (PortalShadowCacheEntry& entry : m_portalShadowCache)
        {
            if (entry.ownerCamera != cam || !entry.targetChart ||
                !entry.selected)
                continue;
            if (!entry.atlas || entry.atlasResolution != portalResolution)
            {
                if (entry.atlas)
                    m_retiredShadowTextures.push_back(std::move(entry.atlas));
                entry.atlas = textureFactory
                    ? textureFactory->CreateDepthTexture2D(
                        portalResolution, portalResolution)
                    : nullptr;
                entry.atlasResolution = entry.atlas ? portalResolution : 0u;
                entry.rendered = false;
            }
            // A recorded native frame may still be reading these upload
            // buffers when the next frame prepares its portal shadows.
            if (entry.bufferCapacity != m_objectCapacity ||
                m_recordingNativeCommands)
            {
                if (entry.constantBuffer && entry.constantMapped)
                    entry.constantBuffer->Unmap();
                if (entry.objectBuffer && entry.objectMapped)
                    entry.objectBuffer->Unmap();
                if (entry.constantBuffer)
                    m_retiredRenderBuffers.push_back(
                        std::move(entry.constantBuffer));
                if (entry.objectBuffer)
                    m_retiredRenderBuffers.push_back(
                        std::move(entry.objectBuffer));
                entry.constantBuffer = bufferFactory->CreateBuffer(
                    Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
                    Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                    static_cast<uint64_t>(m_objectCapacity) * 4u * kCBStride,
                    nullptr, sizeof(DrawCBData));
                entry.objectBuffer = bufferFactory->CreateBuffer(
                    Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
                    Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                    static_cast<uint64_t>(m_objectCapacity) * 4u *
                        sizeof(ObjectGPUData), nullptr, sizeof(ObjectGPUData));
                entry.constantMapped = entry.constantBuffer
                    ? entry.constantBuffer->Map() : nullptr;
                entry.objectMapped = entry.objectBuffer
                    ? entry.objectBuffer->Map() : nullptr;
                entry.bufferCapacity = entry.constantMapped && entry.objectMapped
                    ? m_objectCapacity : 0u;
                entry.rendered = false;
            }
            if (!entry.atlas || !entry.constantMapped || !entry.objectMapped)
                continue;

            const ShadowCascadeSet portalCascades = buildShadowCascades(
                entry.view, portalResolution,
                portalCascadeCount);
            entry.viewProjections = portalCascades.viewProjections;
            entry.cascadeSplits = portalCascades.splits;
            entry.cascadeData = portalCascades.data;
            entry.cameraData = portalCascades.cameraData;
            for (uint32_t cascade = 0; cascade < portalCascades.count; ++cascade)
            {
                for (const PreparedDraw& draw : preparedDraws)
                {
                    const uint32_t recordIndex = cascade * m_objectCapacity +
                        draw.drawData.objectIndex;
                    ObjectGPUData shadowData = draw.objectData;
                    shadowData.viewProjection =
                        portalCascades.viewProjections[cascade];
                    shadowData.shadowCasterFade.z=1.f;
                    shadowData.materialParams.w = static_cast<float>(
                        static_cast<uint32_t>(shadowData.materialParams.w) &
                        (1u | 32u));
                    shadowData.ambientUnlit.w = 1.f;
                    shadowData.environmentParams = glm::vec4(0.f);
                    std::memcpy(static_cast<uint8_t*>(entry.objectMapped) +
                        static_cast<size_t>(recordIndex) * sizeof(ObjectGPUData),
                        &shadowData, sizeof(shadowData));
                    DrawCBData shadowDraw = draw.drawData;
                    shadowDraw.objectIndex = recordIndex;
                    shadowDraw.lightCount = 0;
                    shadowDraw.flags &= ~kClusteredLightingFlag;
                    std::memcpy(static_cast<uint8_t*>(entry.constantMapped) +
                        static_cast<uint64_t>(recordIndex) * kCBStride,
                        &shadowDraw, sizeof(shadowDraw));
                }
            }
            entry.objectBuffer->FlushMappedWrites(0,
                static_cast<uint64_t>(portalCascades.count) *
                    m_objectCapacity * sizeof(ObjectGPUData));
            if (!context->BeginDepthOnlyPass(entry.atlas.get(), 1.f))
                continue;
            context->SetStructuredBuffer(6, m_lightDataBuffer.get());
            context->SetStructuredBuffer(7, entry.objectBuffer.get());
            context->SetStructuredBuffer(8, m_boneDataBuffer.get());
            const uint32_t tileResolution = portalCascades.count > 1u
                ? portalResolution / 2u : portalResolution;
            for (uint32_t cascade = 0; cascade < portalCascades.count; ++cascade)
            {
                const uint32_t tileX = portalCascades.count > 1u
                    ? cascade & 1u : 0u;
                const uint32_t tileY = portalCascades.count > 1u
                    ? cascade >> 1u : 0u;
                context->SetViewport({ static_cast<float>(tileX * tileResolution),
                    static_cast<float>(tileY * tileResolution),
                    static_cast<float>(tileResolution),
                    static_cast<float>(tileResolution), 0.f, 1.f });
                context->SetScissorRect({
                    static_cast<int32_t>(tileX * tileResolution),
                    static_cast<int32_t>(tileY * tileResolution),
                    static_cast<int32_t>((tileX + 1u) * tileResolution),
                    static_cast<int32_t>((tileY + 1u) * tileResolution) });
                for (const PreparedDraw& draw : preparedDraws)
                {
                    if (!draw.castsShadow || !draw.vertexBuffer ||
                        isSpatialManipulatorCarrierDraw(draw) ||
                        !IsObjectInSpatialRegion(draw.object,
                            entry.contentScopeRoot) ||
                        !Engine::Rendering::Portal::
                            IsTraversalInstanceVisibleInConnectedChart(
                                draw.traversalChartPortal, entry.targetChart))
                        continue;
                    Engine::Graphics::IPipelineState* shadowPipeline =
                        draw.shadowDoubleSided
                            ? m_directionalShadowDoubleSidedPipeline.get()
                            : m_directionalShadowPipeline.get();
                    context->SetPipeline(geometryPipeline(draw, shadowPipeline));
                    const uint32_t recordIndex = cascade * m_objectCapacity +
                        draw.drawData.objectIndex;
                    context->SetConstantBuffer(0, entry.constantBuffer.get(),
                        static_cast<uint64_t>(recordIndex) * kCBStride);
                    for (uint32_t textureSlot = 0;
                        textureSlot < 12; ++textureSlot)
                        if (textureSlot != 7u)
                            context->SetTexture(
                                textureSlot, draw.textures[textureSlot]);
                    submitGeometry(draw);
                }
            }
            context->EndDepthOnlyPass();
            entry.rendered = true;
        }
    }

    const auto updatePortalShadowDebug = [&]()
    {
        m_shadowDebugSnapshot.portalAtlasCount = 0;
        m_shadowDebugSnapshot.renderedPortalAtlasCount = 0;
        for (const PortalShadowCacheEntry& entry : m_portalShadowCache)
        {
            if (entry.atlas)
                ++m_shadowDebugSnapshot.portalAtlasCount;
            if (entry.atlas && entry.rendered)
                ++m_shadowDebugSnapshot.renderedPortalAtlasCount;
        }
        m_shadowDebugSnapshot.portalAtlasResolution =
            m_realtimeShadowSettings.portalPolicy ==
                Engine::Model::PortalShadowPolicy::ReuseMain
                ? 0u : m_realtimeShadowSettings.portalAtlasResolution;
    };

    if (shadowOnly)
    {
        updatePortalShadowDebug();
        m_lastShadowCpuTiming.instances=m_lastRenderCpuTimings.instances;
        const auto shadowEnd = std::chrono::steady_clock::now();
        m_lastShadowCpuTiming.portalMs = shadowElapsedMs(portalStart, shadowEnd);
        m_lastShadowCpuTiming.totalMs = shadowElapsedMs(
            shadowCpuStart, shadowEnd);
        return;
    }

    std::optional<Engine::Graphics::GpuTimingStage> activeGpuStage;
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> boundTextures{};
    bool texturesBound = false;
    uint32_t boundTextureSlotCount = 12;
    const auto switchGpuStage = [&](Engine::Graphics::GpuTimingStage stage)
    {
        if (activeGpuStage && *activeGpuStage == stage)
            return;
        if (activeGpuStage)
            context->EndGpuTiming(*activeGpuStage);
        context->BeginGpuTiming(stage);
        activeGpuStage = stage;
    };
    m_lastOrdinaryDrawCount=0;
    const auto submissionStart=RenderClock::now();
    const double uploadBeforeSubmission=m_lastRenderCpuTimings.uploadMs;
    std::vector<uint8_t> submittedAutomaticGroups(automaticGroups.size(),0);
    for (const PreparedDraw& draw : preparedDraws)
    {
        if(draw.automaticGroup!=UINT32_MAX)
        {
            if(submittedAutomaticGroups[draw.automaticGroup]) continue;
            submittedAutomaticGroups[draw.automaticGroup]=1;
            switchGpuStage(draw.terrain || draw.terrainCell ? Engine::Graphics::GpuTimingStage::Terrain :
                draw.mesh && draw.mesh->grassShadowCaster ? Engine::Graphics::GpuTimingStage::Grass : Engine::Graphics::GpuTimingStage::Opaque);
            for(uint32_t textureSlot=0;textureSlot<std::max(draw.textureSlotCount,boundTextureSlotCount);++textureSlot)
                if(!texturesBound || boundTextures[textureSlot]!=draw.textures[textureSlot]) context->SetTexture(textureSlot,draw.textures[textureSlot]);
            boundTextures=draw.textures; boundTextureSlotCount=draw.textureSlotCount; texturesBound=true;
            m_lastOrdinaryDrawCount+=submitAutomaticGroup(automaticGroups[draw.automaticGroup],viewProjection,0,geometryPipeline(draw,draw.pipeline),nullptr);
            continue;
        }
        if(!lodSelected(draw)) continue;
        if (!draw.vertexBuffer)
            continue;
        // Spatial manipulators are logical scene components. Meshes or
        // materials accidentally attached to their carrier object never act
        // as runtime visualization for portals, matrix links, or warp volumes.
        if (isSpatialManipulatorCarrierDraw(draw))
            continue;
        if (draw.occlusionCandidate &&
            context->IsOccluded(draw.occlusionId))
        {
            ++m_lastOcclusionCulledCount;
            continue;
        }
        ensureRecordsUploaded();
        switchGpuStage((draw.terrain || draw.terrainCell)
            ? Engine::Graphics::GpuTimingStage::Terrain
            : (draw.mesh && draw.mesh->grassShadowCaster)
                ? Engine::Graphics::GpuTimingStage::Grass
            : (draw.blended
                ? Engine::Graphics::GpuTimingStage::Transparent
                : Engine::Graphics::GpuTimingStage::Opaque));
        if (draw.occlusionCandidate)
            context->BeginOcclusionQuery(draw.occlusionId);
        if (draw.pmxOutline && m_pmxOutlinePipeline)
        {
            context->SetPipeline(m_pmxOutlinePipeline.get());
            context->SetConstantBuffer(
                0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
            submitGeometry(draw);
        }
        context->SetPipeline(geometryPipeline(draw, draw.pipeline));
        context->SetConstantBuffer(
            0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
        for (uint32_t textureSlot = 0;
            textureSlot < std::max(draw.textureSlotCount,
                boundTextureSlotCount); ++textureSlot)
            if (!texturesBound || boundTextures[textureSlot] != draw.textures[textureSlot])
                context->SetTexture(textureSlot, draw.textures[textureSlot]);
        boundTextures = draw.textures;
        texturesBound = true;
        boundTextureSlotCount = draw.textureSlotCount;
        if(draw.mesh && draw.mesh->IsInstanced())
            for(const auto& range:draw.mesh->GetInstanceRanges()) m_lastOrdinaryDrawCount+=range.instanceCount && range.vertexCount;
        else ++m_lastOrdinaryDrawCount;
        submitMaterialGeometry(draw, geometryPipeline(draw, draw.pipeline));
        if (draw.occlusionCandidate)
            context->EndOcclusionQuery();

        // Draw selected object outline overlay. Structured buffers remain
        // bound across the pipeline change and do not need rebinding.
        if (includeEditorVisuals && !draw.preview &&
            draw.mesh != m_editorWeightPaintMesh &&
            (m_editorSelectedMesh ? draw.mesh == m_editorSelectedMesh
                : draw.object == m_selectedObject) && m_objectOutlinePipeline)
        {
            context->SetPipeline(geometryPipeline(
                draw, m_objectOutlinePipeline.get()));
            context->SetConstantBuffer(
                0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
            submitGeometry(draw);
        }
    }
    m_lastRenderCpuTimings.submissionMs=std::max(0.,std::chrono::duration<double,std::milli>(RenderClock::now()-submissionStart).count()-(m_lastRenderCpuTimings.uploadMs-uploadBeforeSubmission));
    if (activeGpuStage)
    {
        context->EndGpuTiming(*activeGpuStage);
        activeGpuStage.reset();
    }

    ensureRecordsUploaded();
    const auto portalWorkStart=RenderClock::now();
    struct PortalStencilPass
    {
        Engine::Components::SpatialManipulator* source = nullptr;
        Engine::Components::SpatialManipulator* target = nullptr;
        float viewDepth = 0.f;
        size_t drawOrder = 0;
        DrawCBData apertureDrawData{};
        ObjectGPUData apertureObjectData{};
        uint64_t apertureVertexOffset = 0;
        uint32_t apertureVertexCount = 0;
    };

    std::vector<PortalStencilPass> portalPasses;
    // Portal geometry and virtual cameras must live in the same coordinate
    // chart as the camera and ordinary meshes for this view. Game cameras
    // inside a finite warp volume intentionally render its physical/source
    // chart; editor inspection normally renders the embedded chart instead.
    // Mixing render-chart apertures with source-chart depth makes the visible
    // doorway receive no stencil mask (notably in the three-room matrix ring).
    const auto activePortalPoints = [&](const Engine::Components::
        SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart
            ? portal.GetWorldPortalShapePoints()
            : portal.GetRenderWorldPortalShapePoints();
    };
    const auto activePortalFrame = [&](const Engine::Components::
        SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart
            ? portal.GetPortalWorldFrame()
            : portal.GetRenderPortalWorldFrame();
    };
    const auto mapActivePortalPoint = [&](const Engine::Components::
        SpatialManipulator& source, const glm::vec3& point,
        const Engine::Components::SpatialManipulator& target)
    {
        return cameraUsesSourceWarpChart
            ? source.MapWorldPointThroughPortalShape(point, target)
            : source.MapRenderWorldPointThroughPortalShape(point, target);
    };
    // Sort overlapping apertures in the same chart as the view.  Asking the
    // Transform for GetWorldMatrixWithLayer() always applies the render-space
    // warp, even when this camera deliberately uses the physical/source chart;
    // its forward axis could therefore choose the wrong portal as the nearer
    // root.  The inverse view is already the authoritative camera transform
    // for both chart modes.
    const glm::mat4 activeCameraWorld = glm::inverse(view);
    const glm::vec3 cameraForward = glm::normalize(
        glm::vec3(activeCameraWorld[2]));
    size_t portalObjectOrder = 0;
    for (Engine::Components::SpatialManipulator* manipulator :
        m_frameSpatialManipulatorScratch)
    {
        const size_t objectOrder = portalObjectOrder++;
        Engine::Core::Object* object = manipulator ? manipulator->Owner : nullptr;
        if (!object || !object->IsEnabledInHierarchy())
            continue;
        if (!manipulator || !manipulator->enabled)
            continue;

        const auto mode = static_cast<Engine::Components::SpatialManipulator::ConnectionMode>(
            manipulator->connectionMode);
        if (mode != Engine::Components::SpatialManipulator::ConnectionMode::Portal &&
            mode != Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
            continue;

        Engine::Components::SpatialManipulator* target =
            resolvePortalTarget(manipulator);
        if (!target || !target->enabled || !target->Owner ||
            !manipulator->HasCompatiblePortalShapeWith(*target))
            continue;

        PortalStencilPass pass{};
        pass.source = manipulator;
        pass.target = target;
        const std::vector<glm::vec3> aperturePoints =
            activePortalPoints(*manipulator);
        glm::vec3 apertureCenter(0.f);
        for (const glm::vec3& point : aperturePoints)
            apertureCenter += point;
        if (!aperturePoints.empty())
            apertureCenter /= static_cast<float>(aperturePoints.size());
        else
            apertureCenter = object->transform.GetWorldPosition();
        pass.viewDepth = glm::dot(
            apertureCenter - cameraPosition, cameraForward);
        // A procedural aperture uses one transient object slot. It is restored
        // before ordinary scene rendering continues and does not require a
        // corresponding mesh/material draw.
        constexpr uint32_t kPortalObjectSlot = 0u;
        // Keep the stencil aperture rasterizable when it lies between the
        // camera and its near plane. The vertex shader clamps only this
        // procedural mask; connected scene geometry retains normal clipping.
        pass.apertureDrawData = {
            kPortalObjectSlot, 0u, kPortalApertureNearClampFlag, 0u };
        pass.apertureObjectData.world = glm::mat4(1.f);
        pass.apertureObjectData.viewProjection = proj * view;
        pass.apertureObjectData.baseColor = glm::vec4(1.f);
        pass.apertureObjectData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
        pass.apertureObjectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
        pass.apertureObjectData.materialParams = { 0.f, 1.f, 1.f, 0.f };
        pass.apertureObjectData.viewPositionAlphaCutoff =
            glm::vec4(cameraPosition, 0.001f);
        pass.drawOrder = objectOrder;
        portalPasses.push_back(pass);
    }

    // Draw far apertures first so nearer portals deterministically win where
    // their screen-space masks overlap. Scene draw order is the final stable
    // tie-breaker, independent of pointer values or hash iteration order.
    std::stable_sort(portalPasses.begin(), portalPasses.end(),
        [](const PortalStencilPass& left, const PortalStencilPass& right)
        {
            if (std::abs(left.viewDepth - right.viewDepth) > 1e-5f)
                return left.viewDepth > right.viewDepth;
            const std::string& leftName = left.source->Owner->name;
            const std::string& rightName = right.source->Owner->name;
            if (leftName != rightName)
                return leftName < rightName;
            return left.drawOrder < right.drawOrder;
        });

    auto* portalVertices = static_cast<Engine::Model::AnimationVertex*>(
        m_portalApertureMapped);
    uint32_t portalVertexCursor = 0;
    const uint32_t portalVertexCapacity =
        kMaxSpatialObjects * kMaxSpatialVerticesPerObject;
    for (PortalStencilPass& pass : portalPasses)
    {
        const std::vector<glm::vec3> points =
            activePortalPoints(*pass.source);
        if (points.size() < 3)
            continue;
        const uint32_t required = Engine::Rendering::Portal::
            TriangulatedApertureVertexCount(points.size());
        if (portalVertexCursor + required > portalVertexCapacity)
            break;

        pass.apertureVertexOffset = static_cast<uint64_t>(portalVertexCursor) *
            sizeof(Engine::Model::AnimationVertex);
        pass.apertureVertexCount = required;
        for (size_t pointIndex = 1; pointIndex + 1 < points.size(); ++pointIndex)
        {
            const glm::vec3 triangle[3] = {
                points[0], points[pointIndex], points[pointIndex + 1] };
            for (const glm::vec3& point : triangle)
            {
                Engine::Model::AnimationVertex& vertex = portalVertices[portalVertexCursor++];
                vertex = Engine::Model::AnimationVertex {};
                vertex.pos[0] = point.x;
                vertex.pos[1] = point.y;
                vertex.pos[2] = point.z;
            }
        }
    }

    struct SpatialDebugPass
    {
        uint64_t vertexOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t dataSlot = std::numeric_limits<uint32_t>::max();
        uint32_t constantBufferSlot = std::numeric_limits<uint32_t>::max();
        glm::vec4 color { 1.f };
    };
    std::vector<SpatialDebugPass> spatialDebugPasses;
    if (includeEditorVisuals && settings.portalDebugVisuals)
    {
        const auto appendDebugVertex = [&](const glm::vec3& worldPoint)
        {
            Engine::Model::AnimationVertex& vertex = portalVertices[portalVertexCursor++];
            vertex = Engine::Model::AnimationVertex {};
            vertex.pos[0] = worldPoint.x;
            vertex.pos[1] = worldPoint.y;
            vertex.pos[2] = worldPoint.z;
        };
        const auto appendDebugBox = [&, appendDebugVertex](const glm::mat4& world,
            const glm::vec3& halfSize, const glm::vec4& color)
        {
            constexpr uint32_t kBoxVertexCount = 36;
            if (portalVertexCursor + kBoxVertexCount > portalVertexCapacity)
                return;
            static constexpr uint8_t indices[kBoxVertexCount] = {
                0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7,
                0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2 };
            const glm::vec3 corners[8] = {
                {-halfSize.x, -halfSize.y, -halfSize.z},
                { halfSize.x, -halfSize.y, -halfSize.z},
                { halfSize.x,  halfSize.y, -halfSize.z},
                {-halfSize.x,  halfSize.y, -halfSize.z},
                {-halfSize.x, -halfSize.y,  halfSize.z},
                { halfSize.x, -halfSize.y,  halfSize.z},
                { halfSize.x,  halfSize.y,  halfSize.z},
                {-halfSize.x,  halfSize.y,  halfSize.z} };
            SpatialDebugPass pass{};
            pass.vertexOffset = static_cast<uint64_t>(portalVertexCursor) *
                sizeof(Engine::Model::AnimationVertex);
            pass.vertexCount = kBoxVertexCount;
            pass.color = color;
            for (uint8_t index : indices)
                appendDebugVertex(glm::vec3(world * glm::vec4(corners[index], 1.f)));
            spatialDebugPasses.push_back(pass);
        };
        const auto appendDebugSphere = [&, appendDebugVertex](const glm::mat4& world,
            float radius, const glm::vec4& color)
        {
            constexpr uint32_t kSphereVertexCount = 24;
            if (portalVertexCursor + kSphereVertexCount > portalVertexCapacity)
                return;
            const glm::vec3 points[6] = {
                { radius, 0.f, 0.f }, { -radius, 0.f, 0.f },
                { 0.f, radius, 0.f }, { 0.f, -radius, 0.f },
                { 0.f, 0.f, radius }, { 0.f, 0.f, -radius } };
            static constexpr uint8_t indices[kSphereVertexCount] = {
                2, 0, 4, 2, 4, 1, 2, 1, 5, 2, 5, 0,
                3, 4, 0, 3, 1, 4, 3, 5, 1, 3, 0, 5 };
            SpatialDebugPass pass{};
            pass.vertexOffset = static_cast<uint64_t>(portalVertexCursor) *
                sizeof(Engine::Model::AnimationVertex);
            pass.vertexCount = kSphereVertexCount;
            pass.color = color;
            for (uint8_t index : indices)
                appendDebugVertex(glm::vec3(world * glm::vec4(points[index], 1.f)));
            spatialDebugPasses.push_back(pass);
        };
        const auto appendDebugSegment = [&, appendDebugBox](
            const glm::vec3& start, const glm::vec3& end, float thickness,
            const glm::vec4& color)
        {
            const glm::vec3 delta = end - start;
            const float length = glm::length(delta);
            if (length <= 1e-5f)
                return;
            const glm::vec3 xAxis = delta / length;
            const glm::vec3 helper = std::abs(glm::dot(xAxis,
                glm::vec3(0.f, 1.f, 0.f))) > 0.95f
                ? glm::vec3(0.f, 0.f, 1.f)
                : glm::vec3(0.f, 1.f, 0.f);
            const glm::vec3 zAxis = glm::normalize(glm::cross(xAxis, helper));
            const glm::vec3 yAxis = glm::normalize(glm::cross(zAxis, xAxis));
            glm::mat4 world(1.f);
            world[0] = glm::vec4(xAxis, 0.f);
            world[1] = glm::vec4(yAxis, 0.f);
            world[2] = glm::vec4(zAxis, 0.f);
            world[3] = glm::vec4((start + end) * 0.5f, 1.f);
            appendDebugBox(world,
                glm::vec3(length * 0.5f, thickness, thickness), color);
        };

        // Portal connectivity is an editor-only diagnostic. The highlighted
        // geometry is generated transiently from the logical aperture points:
        // yellow markers identify paired points and yellow/gold bars trace
        // aperture edges, point mappings, and normals. None of it is runtime
        // portal geometry or a portal surface.
        using DebugPortalPair = std::pair<
            const Engine::Components::SpatialManipulator*,
            const Engine::Components::SpatialManipulator*>;
        std::vector<DebugPortalPair> highlightedPortalPairs;
        for (const PortalStencilPass& portalPass : portalPasses)
        {
            const bool alreadyHighlighted = std::any_of(
                highlightedPortalPairs.begin(), highlightedPortalPairs.end(),
                [&](const DebugPortalPair& pair)
                {
                    return (pair.first == portalPass.source &&
                            pair.second == portalPass.target) ||
                           (pair.first == portalPass.target &&
                            pair.second == portalPass.source);
                });
            if (alreadyHighlighted)
                continue;
            highlightedPortalPairs.emplace_back(
                portalPass.source, portalPass.target);

            const std::vector<glm::vec3> sourcePoints =
                activePortalPoints(*portalPass.source);
            const std::vector<glm::vec3> targetPoints =
                activePortalPoints(*portalPass.target);
            const size_t pointCount = std::min(
                sourcePoints.size(), targetPoints.size());
            if (pointCount < 3)
                continue;

            float averageEdgeLength = 0.f;
            for (size_t pointIndex = 0; pointIndex < pointCount; ++pointIndex)
            {
                const size_t next = (pointIndex + 1u) % pointCount;
                averageEdgeLength += glm::length(
                    sourcePoints[next] - sourcePoints[pointIndex]);
                averageEdgeLength += glm::length(
                    targetPoints[next] - targetPoints[pointIndex]);
            }
            averageEdgeLength /= static_cast<float>(pointCount * 2u);
            const float markerRadius = std::clamp(
                averageEdgeLength * 0.045f, 0.015f, 0.2f);
            const float edgeThickness = markerRadius * 0.32f;
            const glm::vec4 pointColor { 1.f, 0.92f, 0.10f, 1.f };
            const glm::vec4 edgeColor { 1.f, 0.76f, 0.05f, 1.f };
            const glm::vec4 connectionColor { 1.f, 0.62f, 0.02f, 1.f };
            const glm::vec4 normalColor { 1.f, 0.84f, 0.18f, 1.f };

            glm::vec3 sourceCenter(0.f);
            glm::vec3 targetCenter(0.f);
            for (size_t pointIndex = 0; pointIndex < pointCount; ++pointIndex)
            {
                const size_t next = (pointIndex + 1u) % pointCount;
                sourceCenter += sourcePoints[pointIndex];
                targetCenter += targetPoints[pointIndex];
                appendDebugSphere(glm::translate(glm::mat4(1.f),
                    sourcePoints[pointIndex]), markerRadius, pointColor);
                appendDebugSphere(glm::translate(glm::mat4(1.f),
                    targetPoints[pointIndex]), markerRadius, pointColor);
                appendDebugSegment(sourcePoints[pointIndex], sourcePoints[next],
                    edgeThickness, edgeColor);
                appendDebugSegment(targetPoints[pointIndex], targetPoints[next],
                    edgeThickness, edgeColor);
                const glm::vec3 mappedSourcePoint = mapActivePortalPoint(
                    *portalPass.source, sourcePoints[pointIndex],
                    *portalPass.target);
                // Point indices describe each aperture's authored winding;
                // they are not necessarily the connected correspondence. The
                // portal half-turn reverses local X, so index-to-index bars
                // falsely appeared crossed even while the view transform used
                // the opposite target corner. Draw the transform itself.
                appendDebugSegment(sourcePoints[pointIndex],
                    mappedSourcePoint, edgeThickness * 0.55f,
                    connectionColor);
            }
            sourceCenter /= static_cast<float>(pointCount);
            targetCenter /= static_cast<float>(pointCount);
            glm::vec3 sourceNormal = glm::cross(
                sourcePoints[1] - sourcePoints[0],
                sourcePoints[2] - sourcePoints[0]);
            glm::vec3 targetNormal = glm::cross(
                targetPoints[1] - targetPoints[0],
                targetPoints[2] - targetPoints[0]);
            sourceNormal = glm::dot(sourceNormal, sourceNormal) > 1e-8f
                ? glm::normalize(sourceNormal) : glm::vec3(0.f, 0.f, 1.f);
            targetNormal = glm::dot(targetNormal, targetNormal) > 1e-8f
                ? glm::normalize(targetNormal) : glm::vec3(0.f, 0.f, 1.f);
            const float normalLength = std::max(
                averageEdgeLength * 0.35f, markerRadius * 3.f);
            appendDebugSegment(sourceCenter,
                sourceCenter + sourceNormal * normalLength,
                edgeThickness, normalColor);
            appendDebugSegment(targetCenter,
                targetCenter + targetNormal * normalLength,
                edgeThickness, normalColor);
        }

        for (Engine::Components::SpatialManipulator* manipulator :
            m_frameSpatialManipulatorScratch)
        {
            Engine::Core::Object* sceneObject = manipulator
                ? manipulator->Owner : nullptr;
            if (!sceneObject || !sceneObject->IsEnabledInHierarchy())
                continue;
            if (!manipulator || !manipulator->enabled)
                continue;
            const auto mode = static_cast<
                Engine::Components::SpatialManipulator::ConnectionMode>(
                    manipulator->connectionMode);
            if (mode == Engine::Components::SpatialManipulator::ConnectionMode::Portal ||
                mode == Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
                continue;

            const glm::mat4 ownerWorld =
                sceneObject->transform.GetWorldMatrix();
            const glm::vec3 center(ownerWorld[3]);
            const glm::vec4 warpDebugColor { 1.f, 0.78f, 0.08f, 1.f };
            const float markerRadius = 0.08f;
            if (manipulator->definesWarpVolume)
            {
                const auto shape = static_cast<
                    Engine::Components::SpatialManipulator::WarpVolumeShape>(
                        manipulator->warpVolumeShape);
                if (shape == Engine::Components::SpatialManipulator::WarpVolumeShape::Sphere)
                {
                    appendDebugSphere(ownerWorld,
                        std::max(0.001f, std::abs(manipulator->warpVolumeRadius)),
                        warpDebugColor);
                }
                else
                {
                    const glm::vec3 halfSize = shape ==
                        Engine::Components::SpatialManipulator::WarpVolumeShape::Box
                        ? glm::max(glm::abs(manipulator->warpVolumeSize) * 0.5f,
                            glm::vec3(0.001f))
                        : glm::vec3(0.5f);
                    appendDebugBox(ownerWorld, halfSize,
                        warpDebugColor);
                }
                // A volume boundary alone is difficult to associate with its
                // mapping.  Give every warped space a yellow origin point and
                // short local-frame lines so its placement/orientation is
                // unambiguous in the Scene editor.
                appendDebugSphere(glm::translate(glm::mat4(1.f), center),
                    markerRadius, { 1.f, 0.94f, 0.18f, 1.f });
                const float axisLength = std::clamp(
                    manipulator->warpVolumeRadius * 0.25f, 0.35f, 1.25f);
                appendDebugSegment(center, center + glm::vec3(ownerWorld[0]) *
                    axisLength, markerRadius * 0.3f, warpDebugColor);
                appendDebugSegment(center, center + glm::vec3(ownerWorld[1]) *
                    axisLength, markerRadius * 0.3f, warpDebugColor);
                appendDebugSegment(center, center + glm::vec3(ownerWorld[2]) *
                    axisLength, markerRadius * 0.3f, warpDebugColor);
            }
            else if (mode == Engine::Components::SpatialManipulator::ConnectionMode::MatrixOverlay)
            {
                appendDebugBox(ownerWorld * manipulator->GetOverlayMatrix(),
                    glm::vec3(0.5f), { 1.f, 0.72f, 0.08f, 1.f });
            }
        }
    }
    if (portalVertexCursor > 0)
        m_portalApertureBuffer->FlushMappedWrites();

    struct PortalViewJob
    {
        const PortalStencilPass* portal = nullptr;
        const Engine::Core::Object* contentScopeRoot = nullptr;
        glm::mat4 apertureView { 1.f };
        glm::mat4 mappedView { 1.f };
        glm::vec3 mappedCameraPosition { 0.f };
        uint32_t depth = 0;
        uint32_t rootStencilBase = 1;
        size_t parentJobIndex = std::numeric_limits<size_t>::max();
        // Constant-buffer records retain globally unique frame slots because
        // deferred backends reference them after command recording.
        uint32_t dataSlotBase = 0;
        // Structured portal data lives in its own compact buffer and starts
        // at zero rather than after the ordinary-view capacity.
        uint32_t objectDataSlotBase = 0;
        uint64_t skyboxConstantBufferOffset = 0;
        uint64_t shadowKey = 0;
        float screenArea = 0.f;
        size_t shadowCacheIndex = std::numeric_limits<size_t>::max();
    };
    std::vector<PortalViewJob> portalViewJobs;
    const uint32_t portalDepthLimit = static_cast<uint32_t>(
        Engine::Rendering::Portal::ClampRecursionDepth(
            settings.portalRecursionDepth));
    const size_t portalViewBudget = static_cast<size_t>(
        Engine::Rendering::Portal::ClampViewBudget(
            settings.portalMaxViewsPerFrame));

    const auto apertureRegionInView = [&](const PortalStencilPass& pass,
        const glm::mat4& candidateView)
    {
        const std::vector<glm::vec3> points =
            activePortalPoints(*pass.source);
        if (points.size() < 3)
            return std::vector<glm::vec2>{};
        const glm::mat4 candidateCameraWorld = glm::inverse(candidateView);
        const glm::mat4 sourceFrame = activePortalFrame(*pass.source);
        const float cameraPlaneDistance = glm::dot(
            glm::vec3(candidateCameraWorld[3]) - glm::vec3(sourceFrame[3]),
            glm::vec3(sourceFrame[2]));
        // A camera exactly on an aperture plane has no well-defined source
        // side, and every aperture vertex projects with w=0. Do not feed that
        // singular polygon into recursive scheduling; physical traversal will
        // select the connected chart as soon as the camera body departs.
        if (std::abs(cameraPlaneDistance) <= 1e-5f)
            return std::vector<glm::vec2>{};
        std::vector<glm::vec4> clipPolygon;
        clipPolygon.reserve(points.size());
        for (const glm::vec3& point : points)
            clipPolygon.push_back(proj * candidateView * glm::vec4(point, 1.f));
        clipPolygon = Engine::Rendering::Portal::ClipApertureToViewFrustum(
            std::move(clipPolygon));
        std::vector<glm::vec2> screenPolygon;
        screenPolygon.reserve(clipPolygon.size());
        for (const glm::vec4& point : clipPolygon)
        {
            if (point.w <= 1e-6f || !std::isfinite(point.w))
                return std::vector<glm::vec2>{};
            screenPolygon.push_back(glm::vec2(point) / point.w);
        }
        return screenPolygon;
    };

    const auto apertureScissorInView = [&](const PortalStencilPass& pass,
        const glm::mat4& candidateView)
        -> std::optional<Engine::Graphics::IGraphicsContext::ScissorRect>
    {
        // Stencil is the correctness boundary. This rectangle only avoids
        // shading pixels which cannot belong to the aperture, so a partly
        // near-clipped polygon deliberately falls back to the full viewport.
        if (viewportWidth == 0u || viewportHeight == 0u)
            return std::nullopt;
        const std::vector<glm::vec3> points =
            activePortalPoints(*pass.source);
        if (points.size() < 3u)
            return std::nullopt;

        std::vector<glm::vec4> clipPolygon;
        clipPolygon.reserve(points.size());
        for (const glm::vec3& point : points)
            clipPolygon.push_back(proj * candidateView * glm::vec4(point, 1.f));
        clipPolygon = Engine::Rendering::Portal::ClipApertureToViewFrustum(
            std::move(clipPolygon));
        if (clipPolygon.size() < 3u)
            return Engine::Graphics::IGraphicsContext::ScissorRect { 0, 0, 0, 0 };

        glm::vec2 minimum(std::numeric_limits<float>::max());
        glm::vec2 maximum(std::numeric_limits<float>::lowest());
        for (const glm::vec4& clip : clipPolygon)
        {
            if (clip.w <= 1e-6f || !std::isfinite(clip.w))
                return std::nullopt;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            minimum = glm::min(minimum, ndc);
            maximum = glm::max(maximum, ndc);
        }

        const float width = static_cast<float>(viewportWidth);
        const float height = static_cast<float>(viewportHeight);
        const int32_t left = std::max(0, static_cast<int32_t>(std::floor(
            (minimum.x * 0.5f + 0.5f) * width)) - 1);
        const int32_t right = std::min(static_cast<int32_t>(viewportWidth),
            static_cast<int32_t>(std::ceil((maximum.x * 0.5f + 0.5f) * width)) + 1);
        // The graphics context normalizes Vulkan's inverted native viewport,
        // so this top-left conversion is shared by DX11, DX12, and Vulkan.
        const int32_t top = std::max(0, static_cast<int32_t>(std::floor(
            (0.5f - maximum.y * 0.5f) * height)) - 1);
        const int32_t bottom = std::min(static_cast<int32_t>(viewportHeight),
            static_cast<int32_t>(std::ceil((0.5f - minimum.y * 0.5f) * height)) + 1);
        if (left >= right || top >= bottom)
            return Engine::Graphics::IGraphicsContext::ScissorRect { 0, 0, 0, 0 };
        return Engine::Graphics::IGraphicsContext::ScissorRect {
            left, top, right, bottom };
    };

    using PortalEdge = std::pair<
        const Engine::Components::SpatialManipulator*,
        const Engine::Components::SpatialManipulator*>;
    struct PortalScheduleNode
    {
        glm::mat4 apertureView { 1.f };
        uint32_t depth = 0;
        uint32_t inheritedRootStencilBase = 0;
        const Engine::Components::SpatialManipulator* previousExit = nullptr;
        const Engine::Core::Object* contentScopeRoot = nullptr;
        size_t parentJobIndex = std::numeric_limits<size_t>::max();
        std::vector<glm::vec2> visibleScreenRegion;
        std::vector<PortalEdge> path;
    };
    uint32_t nextRootStencilBase = 1u;
    std::vector<PortalScheduleNode> scheduleQueue;
    if (!portalPasses.empty())
    {
        scheduleQueue.push_back({ view, 0u, 0u, nullptr, nullptr,
            std::numeric_limits<size_t>::max(),
            { {-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f} }, {} });
    }
    size_t scheduleCursor = 0u;
    // Keep every visible root aperture before spending the remaining budget
    // on recursion. A depth-first walk allowed one early portal chain to
    // starve later roots, making them vanish as traversal order changed.
    while (scheduleCursor < scheduleQueue.size() &&
        portalViewJobs.size() < portalViewBudget)
    {
        PortalScheduleNode node = std::move(scheduleQueue[scheduleCursor++]);
        const glm::mat4& apertureView = node.apertureView;
        const uint32_t depth = node.depth;
        if (depth >= portalDepthLimit || portalViewJobs.size() >= portalViewBudget)
            continue;
        const glm::mat4 cameraWorldForView = glm::inverse(apertureView);
        const glm::vec3 viewCameraPosition(cameraWorldForView[3]);
        const glm::vec3 viewForward = glm::normalize(
            glm::vec3(cameraWorldForView[2]));
        const glm::vec3 viewUp = glm::normalize(glm::vec3(cameraWorldForView[1]));
        for (const PortalStencilPass& pass : portalPasses)
        {
            if (portalViewJobs.size() >= portalViewBudget)
                break;
            // The virtual camera has just emerged behind previousExit. That
            // aperture is the boundary of the parent connected view, not a
            // new entrance visible in the remote room. Recursing through it
            // immediately makes equal, aligned portals overwrite their whole
            // parent view. Other apertures—including the original source now
            // visible across the room—remain valid recursive entrances.
            if (Engine::Rendering::Portal::IsImmediateExitAperture(
                pass.source, node.previousExit))
                continue;
            if (!IsObjectInSpatialRegion(pass.source->Owner,
                node.contentScopeRoot))
                continue;
            if (pass.apertureVertexCount < 3)
                continue;

            glm::mat4 effectiveApertureView = apertureView;
            glm::vec3 effectiveCameraPosition = viewCameraPosition;
            const glm::mat4 sourceFrame = activePortalFrame(*pass.source);
            const glm::vec3 sourcePlanePoint(sourceFrame[3]);
            const glm::vec3 sourcePlaneNormal = glm::normalize(
                glm::vec3(sourceFrame[2]));
            const float cameraPlaneDistance = glm::dot(
                viewCameraPosition - sourcePlanePoint, sourcePlaneNormal);
            const float cameraFacing = glm::dot(viewForward, sourcePlaneNormal);
            // At the aperture plane its vertices all project with w=0, so a
            // finite stencil polygon does not exist. If the eye is inside the
            // opening and looking through it, choose that connected chart and
            // offset only the render view onto the source side. Keeping the
            // aperture beyond the near plane yields a stable full-screen mask
            // without moving the physical camera or weakening target clipping.
            if (std::abs(cameraPlaneDistance) <= 1e-5f &&
                std::abs(cameraFacing) > 0.1f &&
                pass.source->IsWorldPointInsidePortalAperture(
                    viewCameraPosition, 0.f))
            {
                const float renderSeparation = std::max(
                    0.002f, cam->nearPlane * 1.05f /
                        std::abs(cameraFacing));
                effectiveCameraPosition -= sourcePlaneNormal *
                    (cameraFacing > 0.f ? renderSeparation : -renderSeparation);
                glm::mat4 effectiveCameraWorld = cameraWorldForView;
                effectiveCameraWorld[3] = glm::vec4(
                    effectiveCameraPosition, 1.f);
                effectiveApertureView = glm::inverse(effectiveCameraWorld);
            }
            std::vector<glm::vec2> visibleScreenRegion =
                Engine::Rendering::Portal::IntersectConvexScreenPolygons(
                    apertureRegionInView(pass, effectiveApertureView),
                    node.visibleScreenRegion);
            if (visibleScreenRegion.size() < 3u)
                continue;
            const PortalEdge edge { pass.source, pass.target };
            const size_t priorConnectionVisits = static_cast<size_t>(
                std::count_if(node.path.begin(), node.path.end(),
                    [&](const PortalEdge& previous)
                    {
                        return (previous.first == edge.first &&
                                previous.second == edge.second) ||
                               (previous.first == edge.second &&
                                previous.second == edge.first);
                    }));
            // A linked pair is allowed to repeat deliberately: with the
            // default limit of two, looking through A shows B and then A once
            // more. The limit prevents an unbounded A -> B -> A loop while
            // preserving recursion through unrelated connections.
            if (!Engine::Rendering::Portal::CanRepeatConnection(
                priorConnectionVisits, settings.portalConnectionRepeatLimit))
                continue;

            uint32_t rootStencilBase = node.inheritedRootStencilBase;
            if (depth == 0u)
            {
                if (nextRootStencilBase + portalDepthLimit - 1u > 0xFFu)
                    break;
                rootStencilBase = nextRootStencilBase;
                nextRootStencilBase += portalDepthLimit;
            }

            const glm::vec3 mappedCamera = mapActivePortalPoint(
                *pass.source, effectiveCameraPosition, *pass.target);
            const glm::vec3 mappedLookAt = mapActivePortalPoint(
                *pass.source, effectiveCameraPosition + viewForward, *pass.target);
            const glm::vec3 mappedUpPoint = mapActivePortalPoint(
                *pass.source, effectiveCameraPosition + viewUp, *pass.target);
            const glm::vec3 mappedForward = glm::normalize(
                mappedLookAt - mappedCamera);
            glm::vec3 mappedUp = mappedUpPoint - mappedCamera;
            mappedUp = glm::dot(mappedUp, mappedUp) <= 1e-6f
                ? glm::vec3(0.f, 1.f, 0.f)
                : glm::normalize(mappedUp);

            PortalViewJob job{};
            job.portal = &pass;
            job.contentScopeRoot =
                pass.target->ResolvePortalContentScopeRoot();
            job.apertureView = effectiveApertureView;
            job.mappedCameraPosition = mappedCamera;
            job.mappedView = glm::lookAtLH(mappedCamera,
                mappedCamera + mappedForward, mappedUp);
            job.depth = depth;
            job.rootStencilBase = rootStencilBase;
            job.parentJobIndex = node.parentJobIndex;
            job.shadowKey = 1469598103934665603ull;
            HashRevision(job.shadowKey, static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(cam)));
            for (const auto& pathEdge : node.path)
            {
                HashRevision(job.shadowKey, static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(pathEdge.first)));
                HashRevision(job.shadowKey, static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(pathEdge.second)));
            }
            HashRevision(job.shadowKey, static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(pass.source)));
            HashRevision(job.shadowKey, static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(pass.target)));
            for (size_t pointIndex = 0; pointIndex < visibleScreenRegion.size();
                ++pointIndex)
            {
                const glm::vec2& first = visibleScreenRegion[pointIndex];
                const glm::vec2& second = visibleScreenRegion[
                    (pointIndex + 1u) % visibleScreenRegion.size()];
                job.screenArea += first.x * second.y - first.y * second.x;
            }
            job.screenArea = std::abs(job.screenArea) * 0.5f;
            const size_t jobIndex = portalViewJobs.size();
            portalViewJobs.push_back(job);

            PortalScheduleNode child{};
            child.apertureView = job.mappedView;
            child.depth = depth + 1u;
            child.inheritedRootStencilBase = rootStencilBase;
            child.previousExit = pass.target;
            child.contentScopeRoot = job.contentScopeRoot;
            child.parentJobIndex = jobIndex;
            child.visibleScreenRegion = std::move(visibleScreenRegion);
            child.path = node.path;
            child.path.push_back(edge);
            scheduleQueue.push_back(std::move(child));
        }
    }

    // Breadth-first discovery protects visible root portals from exhausting
    // the frame budget, but stencil recursion itself must execute depth-first.
    // Sibling apertures use the same depth value; rendering a sibling before
    // an earlier branch's descendants leaves that value in unrelated pixels
    // and lets the descendant mask leak into (or be rejected by) the sibling.
    // Reorder only after the fair set of jobs has been selected.
    if (portalViewJobs.size() > 1u)
    {
        const size_t noParent = std::numeric_limits<size_t>::max();
        std::vector<std::vector<size_t>> childJobs(portalViewJobs.size());
        std::vector<size_t> rootJobs;
        rootJobs.reserve(portalViewJobs.size());
        for (size_t jobIndex = 0; jobIndex < portalViewJobs.size(); ++jobIndex)
        {
            const size_t parentIndex = portalViewJobs[jobIndex].parentJobIndex;
            if (parentIndex == noParent)
                rootJobs.push_back(jobIndex);
            else if (parentIndex < childJobs.size())
                childJobs[parentIndex].push_back(jobIndex);
        }

        std::vector<size_t> depthFirstOrder;
        depthFirstOrder.reserve(portalViewJobs.size());
        std::function<void(size_t)> appendSubtree = [&](size_t jobIndex)
        {
            depthFirstOrder.push_back(jobIndex);
            for (const size_t childIndex : childJobs[jobIndex])
                appendSubtree(childIndex);
        };
        for (const size_t rootIndex : rootJobs)
            appendSubtree(rootIndex);

        if (depthFirstOrder.size() == portalViewJobs.size())
        {
            std::vector<PortalViewJob> orderedJobs;
            orderedJobs.reserve(portalViewJobs.size());
            for (const size_t jobIndex : depthFirstOrder)
                orderedJobs.push_back(std::move(portalViewJobs[jobIndex]));
            portalViewJobs = std::move(orderedJobs);
        }
    }

    for (PortalShadowCacheEntry& entry : m_portalShadowCache)
        if (entry.ownerCamera == cam)
            entry.selected = false;
    ++m_portalShadowGeneration;
    for (auto iterator = m_portalShadowCache.begin();
        iterator != m_portalShadowCache.end();)
    {
        const bool stale = m_portalShadowGeneration >
            iterator->touchedGeneration + 120u;
        if (!stale)
        {
            ++iterator;
            continue;
        }
        if (iterator->constantBuffer && iterator->constantMapped)
            iterator->constantBuffer->Unmap();
        if (iterator->objectBuffer && iterator->objectMapped)
            iterator->objectBuffer->Unmap();
        if (iterator->constantBuffer)
            m_retiredRenderBuffers.push_back(
                std::move(iterator->constantBuffer));
        if (iterator->objectBuffer)
            m_retiredRenderBuffers.push_back(
                std::move(iterator->objectBuffer));
        if (iterator->atlas)
            m_retiredShadowTextures.push_back(std::move(iterator->atlas));
        iterator = m_portalShadowCache.erase(iterator);
    }
    if (m_realtimeShadowSettings.portalPolicy !=
        Engine::Model::PortalShadowPolicy::ReuseMain)
    {
        std::vector<size_t> selectedJobs(portalViewJobs.size());
        std::iota(selectedJobs.begin(), selectedJobs.end(), 0u);
        std::stable_sort(selectedJobs.begin(), selectedJobs.end(),
            [&](size_t leftIndex, size_t rightIndex)
            {
                const PortalViewJob& left = portalViewJobs[leftIndex];
                const PortalViewJob& right = portalViewJobs[rightIndex];
                if (left.depth != right.depth)
                    return left.depth < right.depth;
                if (std::abs(left.screenArea - right.screenArea) > 1e-6f)
                    return left.screenArea > right.screenArea;
                return left.shadowKey < right.shadowKey;
            });
        if (m_realtimeShadowSettings.portalPolicy ==
            Engine::Model::PortalShadowPolicy::Budgeted)
        {
            selectedJobs.resize(std::min(selectedJobs.size(),
                static_cast<size_t>(std::clamp(
                    m_realtimeShadowSettings.portalViewBudget, 1u, 64u))));
        }
        for (const size_t jobIndex : selectedJobs)
        {
            PortalViewJob& job = portalViewJobs[jobIndex];
            auto found = std::find_if(m_portalShadowCache.begin(),
                m_portalShadowCache.end(), [&](const PortalShadowCacheEntry& entry)
                {
                    return entry.ownerCamera == cam && entry.key == job.shadowKey;
                });
            if (found == m_portalShadowCache.end())
            {
                m_portalShadowCache.emplace_back();
                found = std::prev(m_portalShadowCache.end());
                found->key = job.shadowKey;
                found->ownerCamera = cam;
            }
            found->targetChart = job.portal->target;
            found->contentScopeRoot = job.contentScopeRoot;
            found->view = job.mappedView;
            found->cameraPosition = job.mappedCameraPosition;
            found->depth = job.depth;
            found->screenArea = job.screenArea;
            found->touchedGeneration = m_portalShadowGeneration;
            found->selected = true;
            job.shadowCacheIndex = static_cast<size_t>(
                std::distance(m_portalShadowCache.begin(), found));
        }
    }

    const auto connectedSpaceClipPlane = [&activePortalFrame](
        const PortalStencilPass& portalPass,
        const glm::vec3& mappedCameraPosition)
    {
        const glm::mat4 targetFrame = activePortalFrame(*portalPass.target);
        const glm::vec3 clipPoint(targetFrame[3]);
        glm::vec3 clipNormal = glm::normalize(glm::vec3(targetFrame[2]));
        // Keep the half-space beyond the exit aperture. The earlier version
        // pointed this normal toward the virtual camera, retaining the space
        // behind the portal and clipping the floor/scene the opening was
        // supposed to reveal.
        if (glm::dot(mappedCameraPosition - clipPoint, clipNormal) > 0.f)
            clipNormal = -clipNormal;
        return glm::vec4(clipNormal,
            -glm::dot(clipNormal, clipPoint) - 0.0005f);
    };

    // Populate every recorded portal draw before issuing any of them. DX11
    // copies these values as draws are submitted, while DX12/Vulkan retain GPU
    // addresses until command execution; unique ranges satisfy both models.
    for (size_t jobIndex = 0; jobIndex < portalViewJobs.size(); ++jobIndex)
    {
        PortalViewJob& portalJob = portalViewJobs[jobIndex];
        const PortalStencilPass& portalPass = *portalJob.portal;
        portalJob.dataSlotBase = m_objectCapacity +
            static_cast<uint32_t>(jobIndex) * portalRenderSlotsPerView;
        portalJob.objectDataSlotBase =
            static_cast<uint32_t>(jobIndex) * portalRenderSlotsPerView;
        portalJob.skyboxConstantBufferOffset =
            static_cast<uint64_t>(jobIndex + 1u) * kCBStride;

        ObjectGPUData apertureData = portalPass.apertureObjectData;
        apertureData.viewProjection = proj * portalJob.apertureView;
        const uint32_t maskSlot = portalJob.dataSlotBase;
        const uint32_t resetSlot = maskSlot + 1u;
        const uint32_t maskObjectSlot = portalJob.objectDataSlotBase;
        const uint32_t resetObjectSlot = maskObjectSlot + 1u;
        memcpy(static_cast<uint8_t*>(m_portalObjectDataMapped) +
            static_cast<size_t>(maskObjectSlot) * sizeof(ObjectGPUData),
            &apertureData, sizeof(apertureData));
        memcpy(static_cast<uint8_t*>(m_portalObjectDataMapped) +
            static_cast<size_t>(resetObjectSlot) * sizeof(ObjectGPUData),
            &apertureData, sizeof(apertureData));

        DrawCBData maskDraw = portalPass.apertureDrawData;
        maskDraw.objectIndex = maskObjectSlot;
        DrawCBData resetDraw = portalPass.apertureDrawData;
        resetDraw.objectIndex = resetObjectSlot;
        resetDraw.flags |= kPortalDepthResetFlag;
        memcpy(static_cast<uint8_t*>(m_objectCBMapped) +
            static_cast<size_t>(maskSlot) * kCBStride,
            &maskDraw, sizeof(maskDraw));
        memcpy(static_cast<uint8_t*>(m_objectCBMapped) +
            static_cast<size_t>(resetSlot) * kCBStride,
            &resetDraw, sizeof(resetDraw));

        const glm::vec4 portalClipPlane = connectedSpaceClipPlane(
            portalPass, portalJob.mappedCameraPosition);
        for (const PreparedDraw& draw : preparedDraws)
        {
            if (!IsObjectInSpatialRegion(draw.object,
                portalJob.contentScopeRoot))
                continue;
            if (!Engine::Rendering::Portal::
                IsTraversalInstanceVisibleInConnectedChart(
                    draw.traversalChartPortal, portalPass.target))
                continue;
            ObjectGPUData mappedData = draw.objectData;
            mappedData.viewProjection = proj * portalJob.mappedView;
            mappedData.sphereView = portalJob.mappedView;
            mappedData.portalClipPlane = portalClipPlane;
            mappedData.viewPositionAlphaCutoff.x =
                portalJob.mappedCameraPosition.x;
            mappedData.viewPositionAlphaCutoff.y =
                portalJob.mappedCameraPosition.y;
            mappedData.viewPositionAlphaCutoff.z =
                portalJob.mappedCameraPosition.z;
            if (portalJob.shadowCacheIndex < m_portalShadowCache.size())
            {
                const PortalShadowCacheEntry& portalShadow =
                    m_portalShadowCache[portalJob.shadowCacheIndex];
                if (portalShadow.rendered && portalShadow.atlas)
                {
                    const float shadowDistanceScale = mainShadowCascades.data.w > 0.f
                        ? std::clamp(mappedData.shadowCascadeData.w /
                            mainShadowCascades.data.w, 0.f, 1.f)
                        : 1.f;
                    std::copy(portalShadow.viewProjections.begin(),
                        portalShadow.viewProjections.end(),
                        mappedData.shadowViewProjections);
                    mappedData.shadowCascadeSplits = portalShadow.cascadeSplits;
                    mappedData.shadowCascadeData = portalShadow.cascadeData;
                    mappedData.shadowCascadeData.w *= shadowDistanceScale;
                    mappedData.shadowCameraData = portalShadow.cameraData;
                }
            }
            if (includeEditorVisuals && settings.portalDebugVisuals &&
                settings.portalDebugTintRemoteView)
            {
                mappedData.baseColor = glm::vec4(
                    mappedData.baseColor.r * 0.35f,
                    mappedData.baseColor.g * 0.70f,
                    mappedData.baseColor.b * 1.15f,
                    mappedData.baseColor.a);
                mappedData.emissiveOcclusion.x += 0.05f;
                mappedData.emissiveOcclusion.y += 0.15f;
                mappedData.emissiveOcclusion.z += 0.2f;
            }

            const uint32_t drawSlot = portalJob.dataSlotBase + 2u +
                draw.drawData.objectIndex;
            const uint32_t objectDrawSlot = portalJob.objectDataSlotBase + 2u +
                draw.drawData.objectIndex;
            DrawCBData mappedDraw = draw.drawData;
            mappedDraw.objectIndex = objectDrawSlot;
            mappedDraw.flags &= ~kClusteredLightingFlag;
            memcpy(static_cast<uint8_t*>(m_objectCBMapped) +
                static_cast<size_t>(drawSlot) * kCBStride,
                &mappedDraw, sizeof(mappedDraw));
            memcpy(static_cast<uint8_t*>(m_portalObjectDataMapped) +
                static_cast<size_t>(objectDrawSlot) * sizeof(ObjectGPUData),
                &mappedData, sizeof(mappedData));
        }

        SkyboxCBData skyboxData{};
        const glm::mat4 directionOnlyView =
            glm::mat4(glm::mat3(portalJob.mappedView));
        skyboxData.invVP = glm::inverse(proj * directionOnlyView);
        skyboxData.displayParams = {
            m_editorMode2D ? 1.f : 0.f,
            std::exp2(settings.hdriExposure),
            glm::radians(settings.hdriRotation), 0.f };
        memcpy(static_cast<uint8_t*>(m_skyboxCBMapped) +
            portalJob.skyboxConstantBufferOffset,
            &skyboxData, sizeof(skyboxData));
    }
    const uint32_t debugSlotBase = m_objectCapacity;
    const uint32_t debugConstantBufferSlotBase = m_objectCapacity +
        portalObjectSlotCount;
    const float debugAlpha = std::clamp(
        settings.portalDebugOverlayAlpha, 0.f, 1.f);
    for (size_t debugIndex = 0; debugIndex < spatialDebugPasses.size() &&
        debugIndex < kMaxSpatialDebugDraws; ++debugIndex)
    {
        SpatialDebugPass& pass = spatialDebugPasses[debugIndex];
        pass.dataSlot = debugSlotBase + static_cast<uint32_t>(debugIndex);
        pass.constantBufferSlot = debugConstantBufferSlotBase +
            static_cast<uint32_t>(debugIndex);
        const DrawCBData debugDraw { pass.dataSlot, 0u, 0u, 0u };
        ObjectGPUData debugData{};
        debugData.world = glm::mat4(1.f);
        debugData.viewProjection = proj * view;
        debugData.baseColor = glm::vec4(glm::vec3(pass.color), debugAlpha);
        debugData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
        debugData.emissiveOcclusion = glm::vec4(
            glm::vec3(pass.color) * 0.2f, 1.f);
        debugData.materialParams = { 0.f, 1.f, 1.f, 0.f };
        debugData.viewPositionAlphaCutoff =
            glm::vec4(cameraPosition, 0.001f);
        memcpy(static_cast<uint8_t*>(m_objectCBMapped) +
            static_cast<size_t>(pass.constantBufferSlot) * kCBStride,
            &debugDraw, sizeof(debugDraw));
        memcpy(static_cast<uint8_t*>(m_objectDataMapped) +
            static_cast<size_t>(pass.dataSlot) * sizeof(ObjectGPUData),
            &debugData, sizeof(debugData));
    }
    if (!portalViewJobs.empty())
    {
        const uint64_t portalRecords =
            static_cast<uint64_t>(portalViewJobs.size() - 1u) *
                portalRenderSlotsPerView + 2u + preparedDraws.size();
        const uint64_t portalBytes = portalRecords * sizeof(ObjectGPUData);
        m_portalObjectDataBuffer->FlushMappedWrites(0, portalBytes);
        m_lastObjectDataUploadBytes += portalBytes;
        // Aperture-mask draws are the first portal commands and also index
        // portal object data, so switch before issuing any portal work.
        context->SetStructuredBuffer(7, m_portalObjectDataBuffer.get());
    }
    if (!spatialDebugPasses.empty())
    {
        // D3D11's discard upload replaces the buffer contents, so preserve
        // the ordinary prefix when editor diagnostics occupy the tail.
        const uint64_t debugRecords = static_cast<uint64_t>(m_objectCapacity) +
            std::min(spatialDebugPasses.size(),
                static_cast<size_t>(kMaxSpatialDebugDraws));
        const uint64_t debugBytes = debugRecords * sizeof(ObjectGPUData);
        m_objectDataBuffer->FlushMappedWrites(0, debugBytes);
        m_lastObjectDataUploadBytes += debugBytes;
    }

    const auto drawConnectedSkybox = [&](const PortalViewJob& portalJob)
    {
        // A scoped spatial region owns only its explicit hierarchy. Falling
        // back to the scene-global skybox would leak content from outside the
        // connected chart and hide missing region content.
        if (portalJob.contentScopeRoot)
            return;
        if (!skybox || !skybox->GetGraphicsTexture() ||
            !m_portalSkyboxStencilReadPipeline)
            return;
        context->SetPipeline(m_portalSkyboxStencilReadPipeline.get());
        context->SetConstantBuffer(0, m_skyboxConstantBuffer.get(),
            portalJob.skyboxConstantBufferOffset);
        context->SetTexture(0, skybox->GetGraphicsTexture());
        context->DrawInstanced(3, 1, 0, 0);
    };
    const auto portalTexture = [&](const PortalViewJob& portalJob,
        const PreparedDraw& draw, uint32_t textureSlot)
        -> const Engine::Graphics::IGraphicsTexture*
    {
        if (textureSlot == 7u &&
            portalJob.shadowCacheIndex < m_portalShadowCache.size())
        {
            const PortalShadowCacheEntry& shadow =
                m_portalShadowCache[portalJob.shadowCacheIndex];
            if (shadow.rendered && shadow.atlas)
                return shadow.atlas.get();
        }
        return draw.textures[textureSlot];
    };

// Retained only as a diagnostic fallback for older drivers. Normal builds use
// the unified graphics-pipeline path below for DX11, DX12, and Vulkan.
#if defined(_WIN32) && defined(ENGINE_USE_LEGACY_DX11_PORTAL_PATH)
    // Render linked-space view through portal aperture with a stencil mask.
    if (dynamic_cast<Engine::Renderers::D3D11GraphicsProvider*>(m_graphicsProvider))
    {
        ID3D11DeviceContext* dx11Context =
            static_cast<ID3D11DeviceContext*>(context->GetNativeHandle());
        if (dx11Context)
        {
            if (!portalViewJobs.empty())
            {
                ID3D11Device* dx11Device = nullptr;
                dx11Context->GetDevice(&dx11Device);
                if (dx11Device)
                {
                    static Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
                        stencilWriteState;
                    static Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
                        stencilIncrementState;
                    static Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
                        stencilReadState;
                    static Microsoft::WRL::ComPtr<ID3D11BlendState>
                        colorMaskOffState;

                    if (!stencilWriteState)
                    {
                        D3D11_DEPTH_STENCIL_DESC descriptor{};
                        descriptor.DepthEnable = TRUE;
                        descriptor.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                        descriptor.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
                        descriptor.StencilEnable = TRUE;
                        descriptor.StencilReadMask = 0xFF;
                        descriptor.StencilWriteMask = 0xFF;
                        descriptor.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
                        descriptor.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
                        descriptor.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.BackFace = descriptor.FrontFace;
                        dx11Device->CreateDepthStencilState(
                            &descriptor, &stencilWriteState);
                    }
                    if (!stencilReadState)
                    {
                        D3D11_DEPTH_STENCIL_DESC descriptor{};
                        descriptor.DepthEnable = FALSE;
                        descriptor.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                        descriptor.DepthFunc = D3D11_COMPARISON_ALWAYS;
                        descriptor.StencilEnable = TRUE;
                        descriptor.StencilReadMask = 0xFF;
                        descriptor.StencilWriteMask = 0x00;
                        descriptor.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
                        descriptor.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.BackFace = descriptor.FrontFace;
                        dx11Device->CreateDepthStencilState(
                            &descriptor, &stencilReadState);
                    }
                    if (!stencilIncrementState)
                    {
                        D3D11_DEPTH_STENCIL_DESC descriptor{};
                        descriptor.DepthEnable = TRUE;
                        descriptor.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                        descriptor.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
                        descriptor.StencilEnable = TRUE;
                        descriptor.StencilReadMask = 0xFF;
                        descriptor.StencilWriteMask = 0xFF;
                        descriptor.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
                        descriptor.FrontFace.StencilPassOp = D3D11_STENCIL_OP_INCR_SAT;
                        descriptor.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.BackFace = descriptor.FrontFace;
                        dx11Device->CreateDepthStencilState(
                            &descriptor, &stencilIncrementState);
                    }
                    if (!colorMaskOffState)
                    {
                        D3D11_BLEND_DESC descriptor{};
                        descriptor.RenderTarget[0].BlendEnable = FALSE;
                        descriptor.RenderTarget[0].RenderTargetWriteMask = 0;
                        dx11Device->CreateBlendState(&descriptor,
                            &colorMaskOffState);
                    }
                    dx11Device->Release();

                    if (stencilWriteState && stencilIncrementState &&
                        stencilReadState && colorMaskOffState)
                    {
                        const float blendFactor[4]{};
                        for (const PortalViewJob& portalJob : portalViewJobs)
                        {
                        const PortalStencilPass& portalPass = *portalJob.portal;
                        if (portalPass.apertureVertexCount < 3)
                            continue;
                        const auto stencilStep = Engine::Rendering::Portal::
                            StencilForDepth(
                                Engine::Rendering::Portal::Backend::DirectX11,
                                portalJob.depth, portalJob.rootStencilBase);
                        const UINT portalStencilRef = stencilStep.sceneReadReference;

                        context->SetPipeline(m_objectDoubleSidedPipeline.get());
                        context->SetConstantBuffer(
                            0, m_objectConstantBuffer.get(),
                            static_cast<uint64_t>(portalJob.dataSlotBase) *
                                kCBStride);
                        context->SetVertexBuffer(
                            0, m_portalApertureBuffer.get(),
                            sizeof(Engine::Model::AnimationVertex),
                            portalPass.apertureVertexOffset);
                        dx11Context->OMSetBlendState(colorMaskOffState.Get(),
                            blendFactor, UINT_MAX);
                        dx11Context->OMSetDepthStencilState(
                            stencilStep.writeOperation == Engine::Rendering::
                                Portal::ApertureWriteOperation::Replace
                                ? stencilWriteState.Get()
                                : stencilIncrementState.Get(),
                            stencilStep.writeReference);
                        context->DrawInstanced(
                            portalPass.apertureVertexCount, 1, 0, 0);

                        context->SetStructuredBuffer(6, m_lightDataBuffer.get());
                        context->SetStructuredBuffer(7,
                            m_portalObjectDataBuffer.get());
                        context->SetStructuredBuffer(8, m_boneDataBuffer.get());

                        for (const PreparedDraw& draw : preparedDraws)
                        {
                            if (!draw.object || !draw.vertexBuffer)
                                continue;
                            if (isSpatialManipulatorCarrierDraw(draw))
                                continue;
                            if (!IsObjectInSpatialRegion(draw.object,
                                portalJob.contentScopeRoot))
                                continue;
                            if (!Engine::Rendering::Portal::
                                IsTraversalInstanceVisibleInConnectedChart(
                                    draw.traversalChartPortal,
                                    portalPass.target))
                                continue;

                            context->SetPipeline(geometryPipeline(
                                draw, draw.pipeline));
                            const uint32_t drawSlot = portalJob.dataSlotBase +
                                2u + draw.drawData.objectIndex;
                            context->SetConstantBuffer(
                                0, m_objectConstantBuffer.get(),
                                static_cast<uint64_t>(drawSlot) * kCBStride);
                            for (uint32_t textureSlot = 0;
                                textureSlot < draw.textureSlotCount; ++textureSlot)
                            {
                                context->SetTexture(textureSlot,
                                    portalTexture(portalJob, draw, textureSlot));
                            }
                            submitMaterialGeometry(draw, geometryPipeline(draw, draw.pipeline), [&]
                            {
                                dx11Context->OMSetDepthStencilState(
                                    stencilReadState.Get(), portalStencilRef);
                            });
                        }
                        }

                        dx11Context->OMSetDepthStencilState(nullptr, 0);
                        dx11Context->OMSetBlendState(nullptr, blendFactor,
                            UINT_MAX);
                    }
                }
            }
        }
    }
#endif

#if defined(_WIN32) || defined(ENGINE_VULKAN_ENABLED)
    if (!portalViewJobs.empty())
        context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::Portal);
#if defined(_WIN32)
    const bool isDx11Provider =
        dynamic_cast<Engine::Renderers::D3D11GraphicsProvider*>(m_graphicsProvider) != nullptr;
#else
    const bool isDx11Provider = false;
#endif
    const bool isDx12Provider =
        dynamic_cast<Engine::Renderers::D3D12GraphicsProvider*>(m_graphicsProvider) != nullptr;
#if defined(ENGINE_VULKAN_ENABLED)
    const bool isVulkanProvider =
        dynamic_cast<Engine::Renderers::VulkanGraphicsProvider*>(m_graphicsProvider) != nullptr;
#else
    const bool isVulkanProvider = false;
#endif

    if ((isDx11Provider || isDx12Provider
#if defined(ENGINE_VULKAN_ENABLED)
            || isVulkanProvider
#endif
        ) && m_objectPortalStencilWritePipeline &&
            m_objectPortalStencilIncrementPipeline &&
            m_objectPortalDepthResetPipeline)
    {
        auto resolveStencilReadPipeline =
            [&](Engine::Graphics::IPipelineState* base)
        {
            if (base == m_objectPipeline.get())
                return m_objectPortalStencilReadPipeline.get();
            if (base == m_objectDoubleSidedPipeline.get())
                return m_objectPortalStencilReadDoubleSidedPipeline.get();
            if (base == m_objectBlendPipeline.get())
                return m_objectPortalStencilReadBlendPipeline.get();
            if (base == m_objectBlendDoubleSidedPipeline.get())
                return m_objectPortalStencilReadBlendDoubleSidedPipeline.get();
            if (base == m_objectWirePipeline.get())
                return m_objectPortalStencilReadWirePipeline.get();
            if (base == m_objectWireDoubleSidedPipeline.get())
                return m_objectPortalStencilReadWireDoubleSidedPipeline.get();
            if (base == m_objectBlendWirePipeline.get())
                return m_objectPortalStencilReadBlendWirePipeline.get();
            if (base == m_objectBlendWireDoubleSidedPipeline.get())
                return m_objectPortalStencilReadBlendWireDoubleSidedPipeline.get();
            if (base == m_objectPreviewPipeline.get())
                return m_objectPortalStencilReadPreviewPipeline.get();
            if (base == m_objectPreviewDoubleSidedPipeline.get())
                return m_objectPortalStencilReadPreviewDoubleSidedPipeline.get();
            if (base == m_objectPreviewWirePipeline.get())
                return m_objectPortalStencilReadPreviewWirePipeline.get();
            if (base == m_objectPreviewWireDoubleSidedPipeline.get())
                return m_objectPortalStencilReadPreviewWireDoubleSidedPipeline.get();
            return base;
        };

        if (!portalViewJobs.empty())
        {
            // Establish every top-level aperture while the depth buffer still
            // contains only the local scene.  Interleaving root-mask writes
            // with connected-scene draws lets geometry rendered through an
            // earlier portal replace local depth.  In folded layouts (such as
            // the three-room matrix ring), a portal that was originally
            // occluded can then pass the depth test as a second root and
            // overwrite the legitimate recursive view.
            //
            // portalPasses are ordered far-to-near, so overlapping roots keep
            // the nearest local aperture's disjoint stencil range.
            bool usedRootScissor = false;
            for (const PortalViewJob& portalJob : portalViewJobs)
            {
                if (portalJob.depth != 0u)
                    continue;
                const PortalStencilPass& portalPass = *portalJob.portal;
                if (portalPass.apertureVertexCount < 3)
                    continue;

                const auto rootScissor = apertureScissorInView(portalPass,
                    portalJob.apertureView);
                if (rootScissor)
                {
                    context->SetScissorRect(*rootScissor);
                    usedRootScissor = true;
                }
                const auto portalBackend = isDx11Provider
                    ? Engine::Rendering::Portal::Backend::DirectX11
                    : (isDx12Provider
                        ? Engine::Rendering::Portal::Backend::DirectX12
                        : Engine::Rendering::Portal::Backend::Vulkan);
                const auto stencilStep = Engine::Rendering::Portal::
                    StencilForDepth(portalBackend, portalJob.depth,
                        portalJob.rootStencilBase);
                context->SetPipeline(m_objectPortalStencilWritePipeline.get());
                context->SetStencilReference(stencilStep.writeReference);
                context->SetConstantBuffer(
                    0, m_objectConstantBuffer.get(),
                    static_cast<uint64_t>(portalJob.dataSlotBase) * kCBStride);
                context->SetVertexBuffer(
                    0, m_portalApertureBuffer.get(),
                    sizeof(Engine::Model::AnimationVertex),
                    portalPass.apertureVertexOffset);
                context->DrawInstanced(
                    portalPass.apertureVertexCount, 1, 0, 0);
            }
            if (usedRootScissor)
            {
                context->SetScissorRect({ 0, 0,
                    static_cast<int32_t>(viewportWidth),
                    static_cast<int32_t>(viewportHeight) });
            }

            for (const PortalViewJob& portalJob : portalViewJobs)
            {
            const PortalStencilPass& portalPass = *portalJob.portal;
            if (portalPass.apertureVertexCount < 3)
                continue;
            const auto portalScissor = apertureScissorInView(portalPass,
                portalJob.apertureView);
            if (portalScissor)
                context->SetScissorRect(*portalScissor);
            const auto portalBackend = isDx11Provider
                ? Engine::Rendering::Portal::Backend::DirectX11
                : (isDx12Provider
                    ? Engine::Rendering::Portal::Backend::DirectX12
                    : Engine::Rendering::Portal::Backend::Vulkan);
            const auto stencilStep = Engine::Rendering::Portal::
                StencilForDepth(portalBackend, portalJob.depth,
                    portalJob.rootStencilBase);
            // Root apertures were written together against untouched local
            // depth above. Descendants alone modify stencil here, constrained
            // by their inherited root range and the parent connected view.
            if (portalJob.depth > 0u)
            {
                context->SetPipeline(m_objectPortalStencilIncrementPipeline.get());
                context->SetStencilReference(stencilStep.writeReference);
                context->SetConstantBuffer(
                    0, m_objectConstantBuffer.get(),
                    static_cast<uint64_t>(portalJob.dataSlotBase) * kCBStride);
                context->SetVertexBuffer(
                    0, m_portalApertureBuffer.get(),
                    sizeof(Engine::Model::AnimationVertex),
                    portalPass.apertureVertexOffset);
                context->DrawInstanced(portalPass.apertureVertexCount, 1, 0, 0);
            }
            context->SetStencilReference(stencilStep.sceneReadReference);

            context->SetPipeline(m_objectPortalDepthResetPipeline.get());
            context->SetConstantBuffer(
                0, m_objectConstantBuffer.get(),
                static_cast<uint64_t>(portalJob.dataSlotBase + 1u) *
                    kCBStride);
            // Root masks are established in a separate pre-pass, so the
            // currently bound aperture can belong to another root. Always
            // rebind the matching geometry before resetting depth; otherwise
            // a different portal's polygon punches skybox-coloured wedges
            // through local and connected geometry near the aperture edge.
            context->SetVertexBuffer(
                0, m_portalApertureBuffer.get(),
                sizeof(Engine::Model::AnimationVertex),
                portalPass.apertureVertexOffset);
            context->DrawInstanced(portalPass.apertureVertexCount, 1, 0, 0);

            drawConnectedSkybox(portalJob);

            context->SetStructuredBuffer(6, m_lightDataBuffer.get());
            context->SetStructuredBuffer(7, m_portalObjectDataBuffer.get());
            context->SetStructuredBuffer(8, m_boneDataBuffer.get());

            for (const PreparedDraw& draw : preparedDraws)
            {
                if (!draw.object || !draw.vertexBuffer)
                    continue;
                if (isSpatialManipulatorCarrierDraw(draw))
                    continue;
                if (!IsObjectInSpatialRegion(draw.object,
                    portalJob.contentScopeRoot))
                    continue;
                if (!Engine::Rendering::Portal::
                    IsTraversalInstanceVisibleInConnectedChart(
                        draw.traversalChartPortal, portalPass.target))
                    continue;

                context->SetPipeline(geometryPipeline(draw,
                    resolveStencilReadPipeline(draw.pipeline)));
                const uint32_t drawSlot = portalJob.dataSlotBase + 2u +
                    draw.drawData.objectIndex;
                context->SetConstantBuffer(
                    0, m_objectConstantBuffer.get(),
                    static_cast<uint64_t>(drawSlot) * kCBStride);
                for (uint32_t textureSlot = 0;
                    textureSlot < draw.textureSlotCount; ++textureSlot)
                {
                    context->SetTexture(textureSlot,
                        portalTexture(portalJob, draw, textureSlot));
                }
                submitMaterialGeometry(draw, geometryPipeline(draw,
                    resolveStencilReadPipeline(draw.pipeline)));
            }

            // The connected chart uses the same particle records with its
            // own camera, stencil aperture, content scope, and clip plane.
            context->SetStencilReference(stencilStep.sceneReadReference);
            RenderParticles(context, proj * portalJob.mappedView,
                portalJob.mappedView, portalJob.contentScopeRoot,
                connectedSpaceClipPlane(portalPass,
                    portalJob.mappedCameraPosition), true);

            if (portalScissor)
            {
                context->SetScissorRect({ 0, 0,
                    static_cast<int32_t>(viewportWidth),
                    static_cast<int32_t>(viewportHeight) });
            }
        }
        }
    }
#endif

    if (!portalViewJobs.empty())
        context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Portal);

    if (!portalPasses.empty())
    {
        for (const PreparedDraw& draw : preparedDraws)
        {
            memcpy(static_cast<uint8_t*>(m_objectCBMapped) +
                draw.constantBufferOffset,
                &draw.drawData, sizeof(draw.drawData));
            memcpy(static_cast<uint8_t*>(m_objectDataMapped) +
                static_cast<size_t>(draw.drawData.objectIndex) *
                sizeof(ObjectGPUData),
                &draw.objectData, sizeof(draw.objectData));
        }
        const uint64_t restoredRecords = !spatialDebugPasses.empty()
            ? static_cast<uint64_t>(m_objectCapacity) + std::min(
                spatialDebugPasses.size(),
                static_cast<size_t>(kMaxSpatialDebugDraws))
            : static_cast<uint64_t>(m_renderObjectSlotCount);
        const uint64_t restoredBytes = restoredRecords * sizeof(ObjectGPUData);
        m_objectDataBuffer->FlushMappedWrites(0, restoredBytes);
        m_lastObjectDataUploadBytes += restoredBytes;
        context->SetStructuredBuffer(6, m_lightDataBuffer.get());
        context->SetStructuredBuffer(7, m_objectDataBuffer.get());
        context->SetStructuredBuffer(8, m_boneDataBuffer.get());
    }

    if (includeEditorVisuals && settings.portalDebugVisuals)
    {
        Engine::Graphics::IPipelineState* portalDebugPipeline =
            settings.portalDebugWireframe
                ? m_objectSpatialDebugWirePipeline.get()
                : m_objectSpatialDebugPipeline.get();

        if (portalDebugPipeline)
        {
            // Apertures are deliberately not redrawn here. They already own
            // the stencil-masked portal image; an editor debug fill would
            // compete with that image in depth/blend order. SpatialDebugPass
            // contains only the requested point and line diagnostics.
            for (const SpatialDebugPass& pass : spatialDebugPasses)
            {
                if (pass.vertexCount < 3 || pass.dataSlot ==
                    std::numeric_limits<uint32_t>::max())
                    continue;
                context->SetStructuredBuffer(6, m_lightDataBuffer.get());
                context->SetStructuredBuffer(7, m_objectDataBuffer.get());
                context->SetStructuredBuffer(8, m_boneDataBuffer.get());
                context->SetPipeline(portalDebugPipeline);
                context->SetConstantBuffer(0, m_objectConstantBuffer.get(),
                    static_cast<uint64_t>(pass.constantBufferSlot) * kCBStride);
                context->SetVertexBuffer(0, m_portalApertureBuffer.get(),
                    sizeof(Engine::Model::AnimationVertex), pass.vertexOffset);
                context->DrawInstanced(pass.vertexCount, 1, 0, 0);
            }
        }
    }

    RenderParticles(context, proj * view, view);

    // Draw scene helpers (grid) after opaque objects so blending works correctly
    if (includeEditorVisuals && settings.showGrid && m_gridPipeline)
    {
        // Prepare grid constant buffer data
        GridCBData gridData{};
        // Reconstruct grid rays relative to the camera. Inverting a matrix
        // with a large camera translation loses precision near the horizon.
        gridData.invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
        gridData.cameraPos = cameraPosition;
        gridData.cellSize = settings.gridCellSize;
        gridData.gridColor = { settings.gridColor.x, settings.gridColor.y, settings.gridColor.z, settings.gridOpacity };
        // One zoom level for the whole plane keeps the grid's hierarchy
        // stable as the camera moves. The shader uses axisColor.a for it.
        const float gridBaseSpacing = std::max(std::abs(settings.gridCellSize), 0.000001f);
        const float gridZoomLevel = m_editorMode2D ? 0.f : std::clamp(
            std::log10(std::max(std::abs(cameraPosition.y) / gridBaseSpacing,
                0.000001f)), -6.f, 8.f);
        gridData.axisColor = { settings.gridOriginColor.x, settings.gridOriginColor.y,
            settings.gridOriginColor.z, gridZoomLevel };
        gridData.fadeDistance = settings.gridFadeDistance;
        gridData.nearPlane = cam->nearPlane;
        gridData.farPlane = cam->farPlane;
        gridData.mode2D = m_editorMode2D ? 1.f : 0.f;

        // Write to constant buffer
        memcpy(m_gridCBMapped, &gridData, sizeof(GridCBData));

        // Set pipeline and constant buffer
        context->SetPipeline(m_gridPipeline.get());
        context->SetConstantBuffer(0, m_gridConstantBuffer.get(), 0);

        // Draw fullscreen triangle (3 vertices, no vertex buffer)
        context->DrawInstanced(3, 1, 0, 0);
    }

    m_lastRenderCpuTimings.portalMs=std::chrono::duration<double,std::milli>(RenderClock::now()-portalWorkStart).count();
    const auto retainedUiStart=RenderClock::now();
    // Screen-space retained UI is a game-view pass. The scene editor camera
    // should inspect UI objects in the scene without applying runtime
    // fullscreen composition that anchors to the active viewport.
    if (m_uiRenderer && (!includeEditorVisuals || settings.sceneViewUiOverlay))
        m_uiRenderer->Render(*this, context, aspect);
    updatePortalShadowDebug();
    m_lastRenderCpuTimings.retainedUiMs=std::chrono::duration<double,std::milli>(RenderClock::now()-retainedUiStart).count();
}

}
