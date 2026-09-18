#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include "Core/Model/LightingData.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IShader.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Memory/CacheStore.h"
#include "Core/Renderers/UIRenderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <glm/glm.hpp>
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

    struct MeshPositionKey
    {
        float x;
        float y;
        float z;

        bool operator==(const MeshPositionKey& other) const
        {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct MeshPositionKeyHash
    {
        size_t operator()(const MeshPositionKey& value) const
        {
            size_t result = std::hash<float>{}(value.x);
            result ^= std::hash<float>{}(value.y) + 0x9e3779b9u +
                (result << 6u) + (result >> 2u);
            result ^= std::hash<float>{}(value.z) + 0x9e3779b9u +
                (result << 6u) + (result >> 2u);
            return result;
        }
    };

    float SrgbToLinear(uint8_t value)
    {
        const float color = static_cast<float>(value) / 255.f;
        return color <= 0.04045f
            ? color / 12.92f
            : std::pow((color + 0.055f) / 1.055f, 2.4f);
    }

    glm::vec3 ReadEnvironmentPixel(const Engine::Components::Texture& texture,
        uint32_t x, uint32_t y)
    {
        const size_t index = static_cast<size_t>(y) * texture.GetWidth() + x;
        const std::vector<uint8_t>& pixels = texture.GetPixels();
        if (texture.GetFormat() == Engine::Graphics::GraphicsTextureFormat::Rgba32Float)
        {
            glm::vec3 value{};
            std::memcpy(&value.x, pixels.data() + index * 16, sizeof(float));
            std::memcpy(&value.y, pixels.data() + index * 16 + 4, sizeof(float));
            std::memcpy(&value.z, pixels.data() + index * 16 + 8, sizeof(float));
            return glm::max(value, glm::vec3(0.f));
        }
        const uint8_t* source = pixels.data() + index * 4;
        if (texture.IsSrgb())
            return { SrgbToLinear(source[0]), SrgbToLinear(source[1]),
                SrgbToLinear(source[2]) };
        return glm::vec3(source[0], source[1], source[2]) / 255.f;
    }

    std::array<float, 9> SphericalHarmonicBasis(const glm::vec3& direction)
    {
        return {
            0.282095f,
            0.488603f * direction.y,
            0.488603f * direction.z,
            0.488603f * direction.x,
            1.092548f * direction.x * direction.y,
            1.092548f * direction.y * direction.z,
            0.315392f * (3.f * direction.z * direction.z - 1.f),
            1.092548f * direction.x * direction.z,
            0.546274f * (direction.x * direction.x - direction.y * direction.y)
        };
    }

    std::array<glm::vec4, 9> ProjectEnvironment(
        const Engine::Components::Texture& texture)
    {
        std::array<glm::vec4, 9> coefficients{};
        if (!texture.HasPixels() || !texture.GetWidth() || !texture.GetHeight())
            return coefficients;

        const uint32_t stepX = std::max(1u, texture.GetWidth() / 256u);
        const uint32_t stepY = std::max(1u, texture.GetHeight() / 128u);
        constexpr float pi = 3.14159265358979323846f;
        float accumulatedWeight = 0.f;
        for (uint32_t y = 0; y < texture.GetHeight(); y += stepY)
        {
            const float v = (static_cast<float>(y) + 0.5f) /
                static_cast<float>(texture.GetHeight());
            const float polar = v * pi;
            const float sinPolar = std::sin(polar);
            for (uint32_t x = 0; x < texture.GetWidth(); x += stepX)
            {
                const float u = (static_cast<float>(x) + 0.5f) /
                    static_cast<float>(texture.GetWidth());
                const float azimuth = (u - 0.5f) * 2.f * pi;
                const glm::vec3 direction(
                    sinPolar * std::cos(azimuth),
                    std::cos(polar),
                    sinPolar * std::sin(azimuth));
                const glm::vec3 radiance = ReadEnvironmentPixel(texture, x, y);
                const auto basis = SphericalHarmonicBasis(direction);
                for (size_t coefficient = 0; coefficient < basis.size(); ++coefficient)
                    coefficients[coefficient] +=
                        glm::vec4(radiance * basis[coefficient] * sinPolar, 0.f);
                accumulatedWeight += sinPolar;
            }
        }
        if (accumulatedWeight > 0.f)
        {
            const float solidAngleScale = 4.f * pi / accumulatedWeight;
            for (glm::vec4& coefficient : coefficients)
                coefficient *= solidAngleScale;
        }
        return coefficients;
    }

    std::shared_ptr<std::array<glm::vec4, 9>> CachedEnvironmentProjection(
        const Engine::Components::Texture& texture)
    {
        const std::string key = Engine::Memory::CacheStore::PathKey(
            texture.GetFilePath());
        return Engine::Memory::CacheStore::Get().GetOrCreate<
            std::array<glm::vec4, 9>>(
                Engine::Memory::CacheLifetime::LongTerm,
                "Lighting.EnvironmentSH", key,
                [&texture]()
                {
                    return std::make_shared<std::array<glm::vec4, 9>>(
                        ProjectEnvironment(texture));
                });
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

// Large, indexed records live in shader-readable buffers on every backend.
struct ObjectGPUData
{
    // Kept separate from world so adjoining meshes transform a shared world
    // position through exactly the same view-projection arithmetic. Baking
    // world into this matrix per object can expose sub-pixel cracks between
    // otherwise identical terrain borders.
    glm::mat4 viewProjection;
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
    glm::vec4 environmentParams; // intensity, rotation radians, diffuse, reflections
    glm::vec4 environmentSH[9]; // RGB radiance coefficients
    glm::vec4 reflectionEnvironmentParams; // exposure scale, rotation, custom enabled, reserved
    glm::vec4 reflectionEnvironmentSH[9];
    // Enabled per portal draw through DrawCBData::flags; dot(world, plane)
    // selects the connected half-space at the target aperture.
    glm::vec4 portalClipPlane;
    // Independent from portal-view clipping: active traversal bodies render
    // their local and remote chart instances from one untouched mesh buffer.
    glm::vec4 traversalClipPlane;
};

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

static_assert(sizeof(DrawCBData) == 16, "Draw constants must remain small");
static_assert(sizeof(ObjectGPUData) == 672, "Object buffer layout must match Object.hlsl");
static_assert(sizeof(Engine::Model::LightData) == 48,
    "Light buffer layout must match Object.hlsl");
static_assert(sizeof(GridCBData) == 128, "Grid constant-buffer layout must match Grid.hlsl");
static_assert(sizeof(SkyboxCBData) == 80, "Skybox constant-buffer layout must match Skybox.hlsl");

// ---------------------------------------------------------------------------
// Scene::Init
// ---------------------------------------------------------------------------

void Scene::Init(Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider)
        throw std::runtime_error("Scene::Init requires a non-null graphics provider");

    m_graphicsProvider = graphicsProvider;

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

    // Ordinary draws occupy the first kMaxObjects entries. Portal views use
    // stable per-view ranges so deferred command buffers never observe data
    // overwritten by a later recursive pass.
    const uint64_t objectCBSize =
        static_cast<uint64_t>(kObjectRenderSlotCount) * kCBStride;
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
        static_cast<uint64_t>(kOrdinaryObjectSlotCount) * sizeof(ObjectGPUData),
        nullptr, sizeof(ObjectGPUData));
    m_objectDataMapped = m_objectDataBuffer ? m_objectDataBuffer->Map() : nullptr;
    if (!m_objectDataMapped)
        throw std::runtime_error("Failed to create object structured buffer");

    // Portal recursion has a much larger worst-case record count than an
    // ordinary view. Keep it separate so terrain-only frames never upload or
    // invalidate the reserved portal address space.
    m_portalObjectDataBuffer = bufferFactory->CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
        Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(kPortalObjectSlotCount) * sizeof(ObjectGPUData),
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
        static_cast<uint64_t>(kMaxSkinnedObjects) * kMaxBonesPerObject *
            sizeof(glm::mat4),
        nullptr, sizeof(glm::mat4));
    m_boneDataMapped = m_boneDataBuffer ? m_boneDataBuffer->Map() : nullptr;
    if (!m_boneDataMapped)
        throw std::runtime_error("Failed to create bone palette structured buffer");

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
    SetEditorMode2D(m_editorMode2D);

    // Build pipeline states
    BuildGridPipeline();
    BuildSkyboxPipeline();
    BuildObjectPipeline();
    m_uiRenderer = std::make_unique<Engine::Renderers::UIRenderer>();
    m_uiRenderer->Initialize(m_graphicsProvider);
}

void Scene::SetEditorMode2D(bool enabled)
{
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
        : glm::vec3(0.f, 1.5f, -3.f);
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
            .SetRenderTargetFormat(28, 20);
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
    if (!m_defaultSkyboxTexture->Prepare(m_graphicsProvider))
        m_defaultSkyboxTexture.reset();
}

const Engine::Components::Texture* Scene::ResolveSkyboxTexture()
{
    if (settings.skyboxTexture.empty())
        return m_defaultSkyboxTexture.get();

    if (m_loadedSkyboxPath != settings.skyboxTexture)
    {
        m_loadedSkyboxPath = settings.skyboxTexture;
        m_sceneSkyboxTexture = Engine::Components::Texture::Acquire(settings.skyboxTexture);
        if (!m_sceneSkyboxTexture->Prepare(m_graphicsProvider))
            m_sceneSkyboxTexture.reset();
    }
    return m_sceneSkyboxTexture ? m_sceneSkyboxTexture.get() : m_defaultSkyboxTexture.get();
}

const Engine::Components::Texture* Scene::GetSkyboxPreviewTexture()
{
    return ResolveSkyboxTexture();
}

void Scene::UpdateEnvironmentLighting(const Engine::Components::Texture* texture)
{
    const std::string path = texture ? texture->GetFilePath() : std::string{};
    if (path == m_environmentLightingPath)
        return;

    m_environmentLightingPath = path;
    const auto projection = texture
        ? CachedEnvironmentProjection(*texture) : nullptr;
    m_environmentSH = projection
        ? *projection : std::array<glm::vec4, 9>{};
}

std::shared_ptr<const std::array<glm::vec4, 9>>
Scene::ResolveReflectionEnvironment(
    const Engine::Components::Material& material)
{
    if (!material.useCustomReflectionEnvironment ||
        !material.reflectionEnvironmentMap ||
        !material.reflectionEnvironmentMap->HasPixels())
        return nullptr;
    return CachedEnvironmentProjection(*material.reflectionEnvironmentMap);
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
    // - Depth test at far plane to let background show through
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
        .SetDestBlendAlpha(0)                  // D3D12_BLEND_ZERO (0-indexed: 0)
        .SetBlendOpAlpha(0)                    // D3D12_BLEND_OP_ADD (0-indexed: 0)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)                       // D3D12_COMPARISON_FUNC_LESS_EQUAL (0-indexed: 3)
        .SetInputLayout(nullptr, 0)            // No vertex buffer
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 20)         // DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT
        .Build();
    
    if (!m_gridPipeline)
        throw std::runtime_error("Failed to build grid pipeline: " + builder->GetLastError());
}

// ---------------------------------------------------------------------------
// Scene::BuildObjectPipeline
// ---------------------------------------------------------------------------

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
    };
    Engine::Graphics::IPipelineStateBuilder::VertexElement terrainLayout[] =
    {
        { "POSITION", 0, 6, 0,  0, false },
        { "NORMAL",   0, 6, 0, 12, false },
        { "TEXCOORD", 0, 16, 0, 24, false },
        { "COLOR",    0, 2, 0, 32, false },
    };
    m_terrainPipelineByBase.clear();

    enum class PortalStencilMode
    {
        None,
        Write,
        Increment,
        ResetDepth,
        Read
    };

    auto buildMaterialPipelineVariant = [&](bool doubleSided, bool blend,
                                     bool wireframe,
                                     PortalStencilMode stencilMode,
                                     bool colorWriteEnabled,
                                     const char* description,
                                     bool terrainFormat)
    {
        auto materialBuilder = pipelineFactory->CreateBuilder();
        if (!materialBuilder)
            throw std::runtime_error(
                std::string("Failed to create ") + description +
                " pipeline state builder");
        auto& state = materialBuilder->SetVertexShader(
                terrainFormat ? terrainVsShader.get() : vsShader.get())
            .SetPixelShader(psShader.get())
            .SetFillMode(wireframe)
            .SetCullMode(!doubleSided)
            .SetFrontCounterClockwise(false)
            .SetDepthClipEnable(true)
            .SetBlendEnable(blend)
            .SetColorWriteMask(colorWriteEnabled ? 0x0F : 0x00);
        if (blend)
        {
            state.SetSrcBlend(4)
                .SetDestBlend(5)
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
                    ? false : !blend)
            .SetDepthFunc(
                stencilMode == PortalStencilMode::ResetDepth ? 7 :
                stencilMode == PortalStencilMode::Write ||
                stencilMode == PortalStencilMode::Increment
                ? 3 : (blend ? 3 : 1))
            .SetInputLayout(terrainFormat ? terrainLayout : layout,
                terrainFormat ? 4u : 10u)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(28, 20)
            .Build();
        if (!pipeline)
            throw std::runtime_error(
                std::string("Failed to build ") + description +
                " pipeline: " + materialBuilder->GetLastError());
        return pipeline;
    };

    auto buildMaterialPipeline = [&](bool doubleSided, bool blend,
                                     bool wireframe,
                                     PortalStencilMode stencilMode,
                                     bool colorWriteEnabled,
                                     const char* description)
    {
        auto base = buildMaterialPipelineVariant(doubleSided, blend,
            wireframe, stencilMode, colorWriteEnabled, description, false);
        auto terrain = buildMaterialPipelineVariant(doubleSided, blend,
            wireframe, stencilMode, colorWriteEnabled, description, true);
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
            .SetInputLayout(layout, 10)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(28, 20)
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
        .SetInputLayout(layout, 10)
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 20)
        .Build();
    if (!m_objectOutlinePipeline)
        throw std::runtime_error("Failed to build object outline pipeline: " + outlineBuilder->GetLastError());

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
        .SetRenderTargetFormat(28, 20)
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

void Scene::PrepareRenderFrame()
{
    m_renderFramePrepared = false;
    m_frameRenderItems.clear();
    m_frameLightCount = 0;

    if (!m_graphicsProvider || !m_lightDataMapped || !m_boneDataMapped)
        return;

    m_frameLightCount = m_realtimeLightingPipeline.CollectLights(
        *this,
        static_cast<Engine::Model::LightData*>(m_lightDataMapped),
        kMaxLights);
    m_lightDataBuffer->FlushMappedWrites();

    // Nonlinear mesh results depend on authored geometry and active warp
    // volumes, not on camera motion. A compact signature lets both editor and
    // runtime frames reuse static CPU deformation and its upload buffer.
    uint64_t warpRevision = 1469598103934665603ull;
    bool hasActiveWarpVolume = false;
    const auto hashWarpObject = [&](const Engine::Core::Object* object)
    {
        if (!object)
            return;
        if (const auto* manipulator = object->GetComponent<
                Engine::Components::SpatialManipulator>())
        {
            hasActiveWarpVolume = hasActiveWarpVolume ||
                (manipulator->enabled && manipulator->definesWarpVolume &&
                    object->IsEnabledInHierarchy());
            HashRevision(warpRevision,
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(manipulator)));
            HashRevision(warpRevision, manipulator->GetConfigurationRevision());
            HashRevision(warpRevision, object->transform.GetWorldRevision());
            HashRevision(warpRevision, manipulator->enabled ? 1u : 0u);
            HashRevision(warpRevision, manipulator->definesWarpVolume ? 1u : 0u);
            HashRevision(warpRevision, object->IsEnabledInHierarchy() ? 1u : 0u);
        }
    };
    for (const auto& object : m_objects)
        hashWarpObject(object.get());

    m_frameRenderItems.reserve(m_objects.size());
    if (!hasActiveWarpVolume)
        m_warpedRenderMeshes.clear();
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
            cache = m_warpedRenderMeshes.erase(cache);
        else
            ++cache;
    }
    std::unordered_map<Engine::Core::Object*,
        std::vector<Engine::Components::SpatialManipulator::TraversalRenderInstance>>
        traversalRenderInstances;
    for (const auto& object : m_objects)
    {
        if (!object)
            continue;
        if (auto* manipulator = object->GetComponent<
                Engine::Components::SpatialManipulator>())
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
    bool boneDataChanged = false;
    for (const auto& object : m_objects)
    {
        Engine::Core::Object* candidate = object.get();
        Engine::Components::Mesh* mesh =
            candidate->GetComponent<Engine::Components::Mesh>();
        Engine::Components::Sprite* sprite =
            candidate->GetComponent<Engine::Components::Sprite>();
        Engine::Components::Sprite::RenderData spriteData;
        const bool spriteReady = sprite &&
            sprite->PrepareRenderData(m_graphicsProvider, spriteData);
        const bool renderable = spriteReady ||
            (mesh && mesh->IsReady());
        if (!candidate->IsEnabledInHierarchy() || !renderable)
            continue;

        FrameRenderItem item{};
        item.object = candidate;
        item.mesh = mesh;
        item.sprite = sprite;
        item.material = candidate->GetComponent<Engine::Components::Material>();
        item.bakedLighting =
            candidate->GetComponent<Engine::Rendering::BakedLightingData>();
        item.belongsToPreview = m_previewObject &&
            IsObjectOrDescendant(candidate, m_previewObject);
        glm::mat4 authoredWorld = candidate->transform.GetWorldMatrix();
        if (candidate->transform.matrixLayer.enabled)
        {
            authoredWorld = candidate->transform.matrixLayer.localToLayer *
                authoredWorld;
        }
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
        const bool hasSkinnedMesh = candidate->GetComponent<
            Engine::Components::SkinnedMesh>() != nullptr;
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
            const glm::mat3 authoredLinear(authoredWorld);
            const float authoredDeterminant = glm::determinant(authoredLinear);
            const glm::mat3 authoredNormal =
                std::isfinite(authoredDeterminant) &&
                std::abs(authoredDeterminant) > 1e-7f
                    ? glm::transpose(glm::inverse(authoredLinear))
                    : glm::mat3(1.f);
            const SpatialQuery renderQuery {
                SpatialQueryDomain::Rendering, candidate };
            const SpatialQuerySample originSample = SampleSpatialPoint(
                glm::vec3(authoredWorld[3]), renderQuery);
            std::vector<Engine::Model::AnimationVertex> warpedVertices;
            warpedVertices.reserve(mesh->GetVertices().size());
            std::unordered_map<MeshPositionKey, SpatialQuerySample,
                MeshPositionKeyHash> spatialSamples;
            spatialSamples.reserve(mesh->GetVertices().size());
            bool affectedByWarp = originSample.affectedByWarpVolume;
            for (const Engine::Model::AnimationVertex& sourceVertex : mesh->GetVertices())
            {
                Engine::Model::AnimationVertex warpedVertex = sourceVertex;
                const glm::vec3 localPosition(sourceVertex.pos[0],
                    sourceVertex.pos[1], sourceVertex.pos[2]);
                const glm::vec3 worldPosition = glm::vec3(authoredWorld *
                    glm::vec4(localPosition, 1.f));
                const MeshPositionKey positionKey {
                    localPosition.x, localPosition.y, localPosition.z };
                auto [sampleIt, inserted] = spatialSamples.try_emplace(
                    positionKey);
                if (inserted)
                    sampleIt->second = SampleSpatialPoint(worldPosition,
                        renderQuery);
                const SpatialQuerySample& sample = sampleIt->second;
                affectedByWarp = affectedByWarp || sample.affectedByWarpVolume;

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
                warpedVertices.push_back(warpedVertex);
            }
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
                    if (topologyChanged || !cached.vertexBuffer)
                    {
                        cached.vertexBuffer = m_graphicsProvider->GetBufferFactory()
                            ->CreateBuffer(Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer,
                                Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                                byteSize, warpedVertices.data());
                    }
                    else if (void* mapped = cached.vertexBuffer->Map())
                    {
                        std::memcpy(mapped, warpedVertices.data(), byteSize);
                        cached.vertexBuffer->Unmap();
                        cached.vertexBuffer->FlushMappedWrites();
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
            item.material->Validate();
            item.material->PrepareTextures(m_graphicsProvider);
            item.blended = item.material->GetAlphaMode() ==
                Engine::Components::MaterialAlphaMode::Blend;
        }
        item.blended = item.belongsToPreview || item.blended;

        if (skinPaletteSlot < kMaxSkinnedObjects)
        {
            if (Engine::Components::SkinnedMesh* skinned =
                candidate->GetComponent<Engine::Components::SkinnedMesh>())
            {
                std::vector<glm::mat4> palette;
                if (skinned->BuildPalette(palette))
                {
                    const size_t count =
                        std::min<size_t>(palette.size(), kMaxBonesPerObject);
                    item.skinPaletteOffset = skinPaletteSlot * kMaxBonesPerObject;
                    item.skinJointCount = static_cast<uint32_t>(count);
                    std::memcpy(
                        static_cast<glm::mat4*>(m_boneDataMapped) +
                            item.skinPaletteOffset,
                        palette.data(), count * sizeof(glm::mat4));
                    ++skinPaletteSlot;
                    boneDataChanged = true;
                }
            }
        }

        m_frameRenderItems.push_back(item);
        if (hasSplitRenderInstances)
        {
            for (size_t index = 1u; index < splitInstances->second.size(); ++index)
            {
                FrameRenderItem remoteItem = item;
                remoteItem.world = splitInstances->second[index].world;
                remoteItem.traversalClipPlane = splitInstances->second[index].clipPlane;
                remoteItem.traversalChartPortal =
                    splitInstances->second[index].chartPortal;
                m_frameRenderItems.push_back(remoteItem);
            }
        }
    }

    if (boneDataChanged)
        m_boneDataBuffer->FlushMappedWrites();
    m_renderFramePrepared = true;
}

// ---------------------------------------------------------------------------
// Scene::Render
// ---------------------------------------------------------------------------

void Scene::Render(Engine::Graphics::IGraphicsContext* context, float aspect,
    Engine::Components::Camera* cameraOverride, bool includeEditorVisuals,
    uint32_t viewportWidth, uint32_t viewportHeight)
{
    m_lastObjectDataUploadBytes = 0;
    m_lastOcclusionCulledCount = 0;
    if (!context)
    {
        return;
    }

    if (!m_graphicsProvider || !m_objectPipeline)
    {
        return;
    }

    if (!m_renderFramePrepared)
        PrepareRenderFrame();

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
        std::function<void(const Engine::Core::Object*)> findContainingVolume;
        findContainingVolume = [&](const Engine::Core::Object* object)
        {
            if (!object || cameraInsideInteriorOnlyWarp ||
                !object->IsEnabledInHierarchy())
                return;
            const auto* volume = object->GetComponent<
                Engine::Components::SpatialManipulator>();
            if (volume && volume->enabled && volume->definesWarpVolume &&
                volume->renderWarpInteriorOnly &&
                volume->ContainsWorldPoint(authoredCameraPosition))
            {
                cameraInsideInteriorOnlyWarp = true;
                return;
            }
            for (const Engine::Core::Object* child : object->Children)
                findContainingVolume(child);
        };
        for (const auto& object : m_objects)
            findContainingVolume(object.get());
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
            const glm::vec3 rayOrigin = glm::vec3(authoredCameraWorld[3]);
            const glm::vec3 rayDirection = glm::normalize(
                glm::vec3(glm::inverse(view)[2]));
            const auto rayIntersectsVolume = [&](const Engine::Components::SpatialManipulator& volume)
            {
                if (!volume.Owner || !volume.definesWarpVolume || !volume.enabled)
                    return false;
                const glm::mat4 inverseVolume = glm::inverse(
                    volume.Owner->transform.GetWorldMatrix());
                const glm::vec3 origin = glm::vec3(inverseVolume *
                    glm::vec4(rayOrigin, 1.f));
                const glm::vec3 direction = glm::vec3(inverseVolume *
                    glm::vec4(rayDirection, 0.f));
                if (glm::dot(direction, direction) <= 1e-10f)
                    return false;
                const auto shape = static_cast<Engine::Components::SpatialManipulator::
                    WarpVolumeShape>(volume.warpVolumeShape);
                if (shape == Engine::Components::SpatialManipulator::WarpVolumeShape::Infinite)
                    return true;
                if (shape == Engine::Components::SpatialManipulator::WarpVolumeShape::Sphere)
                {
                    const float radius = std::max(0.001f,
                        std::abs(volume.warpVolumeRadius));
                    const float a = glm::dot(direction, direction);
                    const float b = 2.f * glm::dot(origin, direction);
                    const float c = glm::dot(origin, origin) - radius * radius;
                    const float discriminant = b * b - 4.f * a * c;
                    return discriminant >= 0.f &&
                        (-b + std::sqrt(discriminant)) / (2.f * a) >= 0.f;
                }

                const glm::vec3 halfSize = glm::max(glm::abs(volume.warpVolumeSize) *
                    0.5f, glm::vec3(0.0001f));
                float entry = 0.f;
                float exit = std::numeric_limits<float>::infinity();
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (std::abs(direction[axis]) <= 1e-7f)
                    {
                        if (origin[axis] < -halfSize[axis] ||
                            origin[axis] > halfSize[axis])
                            return false;
                        continue;
                    }
                    float nearT = (-halfSize[axis] - origin[axis]) / direction[axis];
                    float farT = (halfSize[axis] - origin[axis]) / direction[axis];
                    if (nearT > farT)
                        std::swap(nearT, farT);
                    entry = std::max(entry, nearT);
                    exit = std::min(exit, farT);
                    if (entry > exit)
                        return false;
                }
                return exit >= 0.f;
            };
            std::function<void(const Engine::Core::Object*)> findIntersectedVolume;
            findIntersectedVolume = [&](const Engine::Core::Object* object)
            {
                if (!object || cameraUsesSourceWarpChart ||
                    !object->IsEnabledInHierarchy())
                    return;
                const auto* volume = object->GetComponent<
                    Engine::Components::SpatialManipulator>();
                if (volume && (sourceChartAllowed ||
                    volume->renderWarpInteriorOnly) &&
                    rayIntersectsVolume(*volume))
                {
                    cameraUsesSourceWarpChart = true;
                    return;
                }
                for (const Engine::Core::Object* child : object->Children)
                    findIntersectedVolume(child);
            };
            for (const auto& object : m_objects)
                findIntersectedVolume(object.get());
        }
    }
    // A source-chart view continues through a finite nonlinear volume instead
    // of projecting its already embedded bend with a straight raster camera.
    // Editor views stay embedded unless Spatial Debug explicitly enables the
    // Scene Camera Warp Look-Through option.
    const auto sourceChartWorld = [](const FrameRenderItem& item)
    {
        glm::mat4 world = item.object->transform.GetWorldMatrix();
        if (item.object->transform.matrixLayer.enabled)
            world = item.object->transform.matrixLayer.localToLayer * world;
        return world;
    };
    const auto useSourceChartMesh = [&](const FrameRenderItem& item)
    {
        return cameraUsesSourceWarpChart && item.mesh && !item.sprite &&
            glm::all(glm::equal(item.traversalClipPlane, glm::vec4(0.f)));
    };
    // Light data is view-chart-specific. Rebuild it for every view so an
    // inside-volume game camera cannot leave source-chart lights bound when
    // the editor view subsequently renders the embedded chart (or vice versa).
    m_frameLightCount = m_realtimeLightingPipeline.CollectLights(*this,
        static_cast<Engine::Model::LightData*>(m_lightDataMapped), kMaxLights,
        !cameraUsesSourceWarpChart);
    m_lightDataBuffer->FlushMappedWrites();
        const bool wireframeMode =
            settings.renderMode == Engine::Model::SceneRenderMode::Wireframe;
        const bool forceUnlitMode =
            settings.renderMode == Engine::Model::SceneRenderMode::Unlit;

    const Engine::Components::Texture* skybox = ResolveSkyboxTexture();
    UpdateEnvironmentLighting(skybox);
    if (skybox && skybox->GetGraphicsTexture() && m_skyboxPipeline)
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
    struct ViewRenderItem
    {
        const FrameRenderItem* source = nullptr;
        float cameraDistanceSquared = 0.f;
        float lightingDistanceSquared = 0.f;
        float worldDepth = 0.f;
        int sortingLayer = 0;
        bool blended = false;
    };
    const auto nearestLightingDistanceSquared = [&](const FrameRenderItem& item,
        const glm::mat4& itemWorld)
    {
        const glm::vec3 centerDelta = glm::vec3(itemWorld[3]) - cameraPosition;
        if (!item.mesh || item.sprite || !item.mesh->HasBounds())
            return glm::dot(centerDelta, centerDelta);

        const glm::vec3 minimum = item.mesh->GetBoundsMin();
        const glm::vec3 maximum = item.mesh->GetBoundsMax();
        glm::vec3 worldMinimum(std::numeric_limits<float>::max());
        glm::vec3 worldMaximum(std::numeric_limits<float>::lowest());
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x)
                {
                    const glm::vec3 corner = glm::vec3(itemWorld * glm::vec4(
                        x ? maximum.x : minimum.x,
                        y ? maximum.y : minimum.y,
                        z ? maximum.z : minimum.z, 1.f));
                    worldMinimum = glm::min(worldMinimum, corner);
                    worldMaximum = glm::max(worldMaximum, corner);
                }
        const glm::vec3 nearest = glm::clamp(cameraPosition,
            worldMinimum, worldMaximum);
        const glm::vec3 delta = nearest - cameraPosition;
        return glm::dot(delta, delta);
    };
    const glm::mat4 viewProjection = proj * view;
    const auto outsideViewFrustum = [&](const FrameRenderItem& item,
        const glm::mat4& itemWorld)
    {
        if (!item.mesh || item.sprite || item.warpedVertexBuffer ||
            !item.mesh->HasBounds() || item.traversalChartPortal)
            return false;
        const glm::vec3 minimum = item.mesh->GetBoundsMin();
        const glm::vec3 maximum = item.mesh->GetBoundsMax();
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
    std::vector<ViewRenderItem> renderObjects;
    renderObjects.reserve(m_frameRenderItems.size());
    for (const FrameRenderItem& item : m_frameRenderItems)
    {
        if (!item.belongsToPreview || includeEditorVisuals)
        {
            const glm::mat4 itemWorld = useSourceChartMesh(item)
                ? sourceChartWorld(item) : item.world;
            if (outsideViewFrustum(item, itemWorld))
                continue;
            const glm::vec3 delta = glm::vec3(itemWorld[3]) - cameraPosition;
            renderObjects.push_back({ &item, glm::dot(delta, delta),
                nearestLightingDistanceSquared(item, itemWorld),
                itemWorld[3].z, item.sortingLayer, item.blended });
        }
    }
    std::stable_sort(renderObjects.begin(), renderObjects.end(),
        [&](const ViewRenderItem& first, const ViewRenderItem& second)
        {
            if (m_editorMode2D)
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
        }
    }
    const uint64_t occlusionViewId =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)) ^
        (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cam)) << 1u);
    const bool occlusionQueriesEnabled = context->BeginOcclusionFrame(
        occlusionViewId, occlusionSignature);

    struct PreparedDraw
    {
        Engine::Core::Object* object = nullptr;
        Engine::Graphics::IGraphicsBuffer* vertexBuffer = nullptr;
        Engine::Graphics::IGraphicsBuffer* indexBuffer = nullptr;
        Engine::Graphics::IPipelineState* pipeline = nullptr;
        std::array<const Engine::Graphics::IGraphicsTexture*, 7> textures{};
        UINT64 constantBufferOffset = 0;
        DrawCBData drawData{};
        ObjectGPUData objectData{};
        uint32_t vertexStride = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        const Engine::Components::SpatialManipulator* traversalChartPortal = nullptr;
        bool preview = false;
        bool occlusionCandidate = false;
        uint64_t occlusionId = 0;
    };
    std::vector<PreparedDraw> preparedDraws;
    preparedDraws.reserve(std::min<size_t>(renderObjects.size(), kMaxObjects));

    UINT slot = 0;
    for (const ViewRenderItem& sortedItem : renderObjects)
    {
        if (slot >= kMaxObjects)
            break;

        const FrameRenderItem* renderItem = sortedItem.source;
        Engine::Core::Object* obj = renderItem->object;
        Engine::Components::Mesh* mesh = renderItem->mesh;
        Engine::Components::Sprite* sprite = renderItem->sprite;
        Engine::Components::Material* mat = renderItem->material;
        const bool belongsToPreview = renderItem->belongsToPreview;
        const bool isPreview = belongsToPreview;
        const Engine::Model::DistanceLightingBand* lightingBand = nullptr;
        if (m_distanceLightingSettings.enabled &&
            !m_distanceLightingSettings.bands.empty())
        {
            const float distance = std::sqrt(
                sortedItem.lightingDistanceSquared);
            for (const auto& candidate : m_distanceLightingSettings.bands)
            {
                lightingBand = &candidate;
                if (distance <= candidate.endDistance)
                    break;
            }
        }
        const bool allowNormalMapping = !lightingBand ||
            lightingBand->normalMapping;
        const bool allowParallaxMapping = !lightingBand ||
            lightingBand->parallaxMapping;
        const bool allowEnvironmentDiffuse = !lightingBand ||
            lightingBand->environmentDiffuse;
        const bool allowReflections = !lightingBand ||
            lightingBand->reflections;
        PreparedDraw preparedDraw{};
        preparedDraw.object = obj;
        preparedDraw.preview = isPreview;
        preparedDraw.occlusionCandidate = occlusionQueriesEnabled &&
            !wireframeMode &&
            !sortedItem.blended && !sprite &&
            !renderItem->warpedVertexBuffer &&
            !renderItem->traversalChartPortal;
        preparedDraw.occlusionId = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(obj));
        const Engine::Rendering::BakedLightingData* bakedLighting =
            renderItem->bakedLighting;
        // Version 3 and later bake lighting into generated material assets.
        // Keep the component values for inspection, but do not add them again
        // at runtime or the baked result would be double-lit.
        const bool usesLegacyProbeBake = bakedLighting && bakedLighting->valid &&
            bakedLighting->version < 3;
        const glm::vec3 bakedIrradiance =
            usesLegacyProbeBake
                ? bakedLighting->irradiance
                : glm::vec3(0.f);
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
        if (settings.hdriLightingEnabled && skybox &&
            (allowEnvironmentDiffuse || allowReflections))
        {
            objectData.environmentParams = {
                std::max(0.f, settings.hdriIntensity) *
                    std::exp2(std::clamp(settings.hdriExposure, -16.f, 16.f)),
                glm::radians(settings.hdriRotation), 1.f, 1.f };
            std::copy(m_environmentSH.begin(), m_environmentSH.end(),
                objectData.environmentSH);
            preparedDraw.textures[6] = skybox->GetGraphicsTexture();
            if (preparedDraw.textures[6])
                objectData.reflectionEnvironmentParams.w = 1.f;
        }
        objectData.viewProjection = proj * view;
        objectData.world = world;
        objectData.traversalClipPlane = renderItem->traversalClipPlane;
        objectData.spriteUvRect = { 0.f, 0.f, 1.f, 1.f };
        if (renderItem->skinJointCount > 0)
        {
            objectData.skinParams = {
                static_cast<float>(renderItem->skinPaletteOffset),
                static_cast<float>(renderItem->skinJointCount), 0.f, 0.f };
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
            alphaMode = mat->GetAlphaMode();
            doubleSided = mat->doubleSided;
            uint32_t textureFlags = 0;
            auto prepareTexture = [&](uint32_t textureSlot,
                                      const std::shared_ptr<Engine::Components::Texture>& texture,
                                      uint32_t flag)
            {
                const Engine::Graphics::IGraphicsTexture* graphicsTexture =
                    texture ? texture->GetGraphicsTexture() : nullptr;
                preparedDraw.textures[textureSlot] = graphicsTexture;
                if (graphicsTexture)
                    textureFlags |= flag;
            };
            prepareTexture(0, mat->baseColorTexture, 1u);
            prepareTexture(1, mat->metallicRoughnessTexture, 2u);
            if (allowNormalMapping)
                prepareTexture(2, mat->normalTexture, 4u);
            prepareTexture(3, mat->occlusionTexture, 8u);
            prepareTexture(4, mat->emissiveTexture, 16u);
            if (allowParallaxMapping)
                prepareTexture(5, mat->heightTexture, 64u);
            if (alphaMode == Engine::Components::MaterialAlphaMode::Mask)
                textureFlags |= 32u;

            objectData.baseColor = glm::vec4(mat->diffuseColor, mat->baseColorAlpha);
            objectData.ambientUnlit = glm::vec4(
                settings.ambientColor + mat->ambientColor + bakedIrradiance,
                mat->unlit ? 1.f : 0.f);
            objectData.emissiveOcclusion = glm::vec4(
                mat->emissiveColor, mat->occlusionStrength);
            objectData.materialParams = {
                mat->metallicFactor, mat->roughnessFactor, mat->normalScale,
                static_cast<float>(textureFlags)
            };
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
            if (const auto reflectionSH = ResolveReflectionEnvironment(*mat))
            {
                objectData.reflectionEnvironmentParams = {
                    std::exp2(std::clamp(mat->reflectionEnvironmentExposure,
                        -16.f, 16.f)),
                    glm::radians(mat->reflectionEnvironmentRotation), 1.f, 0.f };
                std::copy(reflectionSH->begin(), reflectionSH->end(),
                    objectData.reflectionEnvironmentSH);
                if (mat->reflectionEnvironmentMap &&
                    mat->reflectionEnvironmentMap->GetGraphicsTexture())
                {
                    preparedDraw.textures[6] =
                        mat->reflectionEnvironmentMap->GetGraphicsTexture();
                    objectData.reflectionEnvironmentParams.w = 1.f;
                }
            }
            objectData.textureUvSets0 = { static_cast<float>(mat->baseColorUvSet),
                static_cast<float>(mat->metallicRoughnessUvSet), static_cast<float>(mat->normalUvSet),
                static_cast<float>(mat->occlusionUvSet) };
            objectData.textureUvSets1 = { static_cast<float>(mat->emissiveUvSet),
                static_cast<float>(mat->heightUvSet), 0.f, 0.f };
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
        if (isPreview)
            objectData.baseColor.a *= 0.45f;
            if (forceUnlitMode)
                objectData.ambientUnlit.w = 1.f;
        if (usesLegacyProbeBake)
        {
            objectData.bakedDirectional = glm::vec4(
                bakedLighting->directionalIrradiance, 0.f);
            objectData.bakedLightDirection = glm::vec4(
                bakedLighting->lightDirection, 1.f);
        }

        const uint32_t qualityLightCount = lightingBand
            ? std::min(m_frameLightCount, lightingBand->maxRealtimeLights)
            : m_frameLightCount;
        const DrawCBData drawData{ slot,
            forceUnlitMode ? 0u : qualityLightCount, 0u, 0u };
        preparedDraw.drawData = drawData;
        preparedDraw.objectData = objectData;
        preparedDraw.traversalChartPortal = renderItem->traversalChartPortal;
        memcpy(static_cast<uint8_t*>(m_objectCBMapped) + offset,
            &preparedDraw.drawData, sizeof(preparedDraw.drawData));
        memcpy(static_cast<uint8_t*>(m_objectDataMapped) +
            static_cast<size_t>(slot) * sizeof(ObjectGPUData),
            &preparedDraw.objectData, sizeof(preparedDraw.objectData));

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
        preparedDraw.pipeline = materialPipeline;
        preparedDraw.constantBufferOffset = offset;
        preparedDraw.vertexBuffer = sprite
            ? renderItem->spriteVertexBuffer
            : (!useSourceChart && renderItem->warpedVertexBuffer
                ? renderItem->warpedVertexBuffer
                : (mesh ? mesh->GetGraphicsBuffer() : nullptr));
        preparedDraw.vertexStride = sprite
            ? sprite->GetVertexStride() : mesh->GetVertexStride();
        preparedDraw.vertexCount = sprite
            ? sprite->GetVertexCount() : mesh->GetVertexCount();
        const bool usesDirectMeshBuffer = !sprite && !useSourceChart &&
            !renderItem->warpedVertexBuffer;
        if (usesDirectMeshBuffer && mesh && mesh->GetIndexBuffer())
        {
            preparedDraw.indexBuffer = mesh->GetIndexBuffer();
            preparedDraw.indexCount = mesh->GetIndexCount();
        }
        preparedDraws.push_back(preparedDraw);

        ++slot;
    }

    // DX11 buffers use CPU-side shadow storage. Upload the complete object
    // array once, then keep structured-buffer binding free of hidden copies.
    if (!preparedDraws.empty())
    {
        const uint64_t ordinaryBytes = static_cast<uint64_t>(
            preparedDraws.size()) * sizeof(ObjectGPUData);
        m_objectDataBuffer->FlushMappedWrites(0, ordinaryBytes);
        m_lastObjectDataUploadBytes += ordinaryBytes;
        context->SetStructuredBuffer(6, m_lightDataBuffer.get());
        context->SetStructuredBuffer(7, m_objectDataBuffer.get());
        context->SetStructuredBuffer(8, m_boneDataBuffer.get());
    }

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
        for (const auto& candidateObject : m_objects)
        {
            if (!candidateObject || candidateObject.get() == source->Owner)
                continue;
            auto* candidate = candidateObject->GetComponent<
                Engine::Components::SpatialManipulator>();
            if (candidate && candidate->ResolveTarget() == source)
                return candidate;
        }
        return nullptr;
    };

    const auto isSpatialManipulatorCarrierDraw = [&](const PreparedDraw& draw)
    {
        if (!draw.object)
            return false;
        auto* manipulator =
            draw.object->GetComponent<Engine::Components::SpatialManipulator>();
        return manipulator && manipulator->enabled;
    };

    const auto geometryPipeline = [&](const PreparedDraw& draw,
        Engine::Graphics::IPipelineState* base)
    {
        if (!draw.indexBuffer)
            return base;
        const auto terrain = m_terrainPipelineByBase.find(base);
        return terrain != m_terrainPipelineByBase.end()
            ? terrain->second.get() : base;
    };

    const auto submitGeometry = [&](const PreparedDraw& draw)
    {
        context->SetVertexBuffer(0, draw.vertexBuffer, draw.vertexStride, 0);
        if (draw.indexBuffer && draw.indexCount > 0u)
        {
            context->SetIndexBuffer(draw.indexBuffer, draw.indexCount, 0);
            context->DrawIndexedInstanced(draw.indexCount, 1, 0, 0, 0);
        }
        else
            context->DrawInstanced(draw.vertexCount, 1, 0, 0);
    };

    for (const PreparedDraw& draw : preparedDraws)
    {
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
        if (draw.occlusionCandidate)
            context->BeginOcclusionQuery(draw.occlusionId);
        context->SetPipeline(geometryPipeline(draw, draw.pipeline));
        context->SetConstantBuffer(
            0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
        for (uint32_t textureSlot = 0; textureSlot < draw.textures.size(); ++textureSlot)
            context->SetTexture(textureSlot, draw.textures[textureSlot]);
        submitGeometry(draw);
        if (draw.occlusionCandidate)
            context->EndOcclusionQuery();

        // Draw selected object outline overlay. Structured buffers remain
        // bound across the pipeline change and do not need rebinding.
        if (includeEditorVisuals && !draw.preview &&
            draw.object == m_selectedObject && m_objectOutlinePipeline)
        {
            context->SetPipeline(geometryPipeline(
                draw, m_objectOutlinePipeline.get()));
            context->SetConstantBuffer(
                0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
            submitGeometry(draw);
        }
    }

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
    const glm::mat4 cameraWorld = cam->Owner
        ? cam->Owner->transform.GetWorldMatrixWithLayer()
        : glm::mat4(1.f);
    const glm::vec3 cameraForward = glm::normalize(glm::vec3(cameraWorld[2]));
    size_t portalObjectOrder = 0;
    for (const auto& sceneObject : m_objects)
    {
        Engine::Core::Object* object = sceneObject.get();
        const size_t objectOrder = portalObjectOrder++;
        if (!object || !object->IsEnabledInHierarchy())
            continue;

        auto* manipulator =
            object->GetComponent<Engine::Components::SpatialManipulator>();
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
            manipulator->GetRenderWorldPortalShapePoints();
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
            pass.source->GetRenderWorldPortalShapePoints();
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
                portalPass.source->GetRenderWorldPortalShapePoints();
            const std::vector<glm::vec3> targetPoints =
                portalPass.target->GetRenderWorldPortalShapePoints();
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
                const glm::vec3 mappedSourcePoint = portalPass.source
                    ->MapRenderWorldPointThroughPortalShape(
                        sourcePoints[pointIndex], *portalPass.target);
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

        for (const auto& sceneObject : m_objects)
        {
            if (!sceneObject || !sceneObject->IsEnabledInHierarchy())
                continue;
            auto* manipulator = sceneObject->GetComponent<
                Engine::Components::SpatialManipulator>();
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
        glm::mat4 apertureView { 1.f };
        glm::mat4 mappedView { 1.f };
        glm::vec3 mappedCameraPosition { 0.f };
        uint32_t depth = 0;
        uint32_t rootStencilBase = 1;
        // Constant-buffer records retain globally unique frame slots because
        // deferred backends reference them after command recording.
        uint32_t dataSlotBase = 0;
        // Structured portal data lives in its own compact buffer and starts
        // at zero rather than after the ordinary-view capacity.
        uint32_t objectDataSlotBase = 0;
        uint64_t skyboxConstantBufferOffset = 0;
    };
    std::vector<PortalViewJob> portalViewJobs;
    const uint32_t portalDepthLimit = static_cast<uint32_t>(
        Engine::Rendering::Portal::ClampRecursionDepth(
            settings.portalRecursionDepth));
    const size_t portalViewBudget = static_cast<size_t>(
        Engine::Rendering::Portal::ClampViewBudget(
            settings.portalMaxViewsPerFrame));

    const auto apertureVisibleInView = [&](const PortalStencilPass& pass,
        const glm::mat4& candidateView)
    {
        const std::vector<glm::vec3> points =
            pass.source->GetRenderWorldPortalShapePoints();
        if (points.size() < 3)
            return false;
        std::vector<glm::vec4> clipPolygon;
        clipPolygon.reserve(points.size());
        for (const glm::vec3& point : points)
            clipPolygon.push_back(proj * candidateView * glm::vec4(point, 1.f));
        return Engine::Rendering::Portal::ClipApertureToViewFrustum(
            std::move(clipPolygon)).size() >= 3u;
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
            pass.source->GetRenderWorldPortalShapePoints();
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
    std::vector<PortalEdge> portalPath;
    uint32_t nextRootStencilBase = 1u;
    std::function<void(const glm::mat4&, uint32_t, uint32_t,
        const Engine::Components::SpatialManipulator*)>
        schedulePortalViews;
    schedulePortalViews = [&](const glm::mat4& apertureView, uint32_t depth,
                              uint32_t inheritedRootStencilBase,
                              const Engine::Components::SpatialManipulator*
                                  previousExit)
    {
        if (depth >= portalDepthLimit || portalViewJobs.size() >= portalViewBudget)
            return;
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
                pass.source, previousExit))
                continue;
            if (pass.apertureVertexCount < 3 ||
                !apertureVisibleInView(pass, apertureView))
                continue;
            const PortalEdge edge { pass.source, pass.target };
            const size_t priorConnectionVisits = static_cast<size_t>(
                std::count_if(portalPath.begin(), portalPath.end(),
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

            uint32_t rootStencilBase = inheritedRootStencilBase;
            if (depth == 0u)
            {
                if (nextRootStencilBase + portalDepthLimit - 1u > 0xFFu)
                    break;
                rootStencilBase = nextRootStencilBase;
                nextRootStencilBase += portalDepthLimit;
            }

            const glm::vec3 mappedCamera =
                pass.source->MapRenderWorldPointThroughPortalShape(
                    viewCameraPosition, *pass.target);
            const glm::vec3 mappedLookAt =
                pass.source->MapRenderWorldPointThroughPortalShape(
                    viewCameraPosition + viewForward, *pass.target);
            const glm::vec3 mappedUpPoint =
                pass.source->MapRenderWorldPointThroughPortalShape(
                    viewCameraPosition + viewUp, *pass.target);
            const glm::vec3 mappedForward = glm::normalize(
                mappedLookAt - mappedCamera);
            glm::vec3 mappedUp = mappedUpPoint - mappedCamera;
            mappedUp = glm::dot(mappedUp, mappedUp) <= 1e-6f
                ? glm::vec3(0.f, 1.f, 0.f)
                : glm::normalize(mappedUp);

            PortalViewJob job{};
            job.portal = &pass;
            job.apertureView = apertureView;
            job.mappedCameraPosition = mappedCamera;
            job.mappedView = glm::lookAtLH(mappedCamera,
                mappedCamera + mappedForward, mappedUp);
            job.depth = depth;
            job.rootStencilBase = rootStencilBase;
            portalViewJobs.push_back(job);

            portalPath.push_back(edge);
            schedulePortalViews(
                job.mappedView, depth + 1u, rootStencilBase, pass.target);
            portalPath.pop_back();
        }
    };
    schedulePortalViews(view, 0u, 0u, nullptr);

    const auto connectedSpaceClipPlane = [](
        const PortalStencilPass& portalPass,
        const glm::vec3& mappedCameraPosition)
    {
        const glm::mat4 targetFrame =
            portalPass.target->GetRenderPortalWorldFrame();
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
        portalJob.dataSlotBase = kMaxObjects +
            static_cast<uint32_t>(jobIndex) * kPortalRenderSlotsPerView;
        portalJob.objectDataSlotBase =
            static_cast<uint32_t>(jobIndex) * kPortalRenderSlotsPerView;
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
            if (!Engine::Rendering::Portal::
                IsTraversalInstanceVisibleInConnectedChart(
                    draw.traversalChartPortal, portalPass.target))
                continue;
            ObjectGPUData mappedData = draw.objectData;
            mappedData.viewProjection = proj * portalJob.mappedView;
            mappedData.portalClipPlane = portalClipPlane;
            mappedData.viewPositionAlphaCutoff.x =
                portalJob.mappedCameraPosition.x;
            mappedData.viewPositionAlphaCutoff.y =
                portalJob.mappedCameraPosition.y;
            mappedData.viewPositionAlphaCutoff.z =
                portalJob.mappedCameraPosition.z;
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
    const uint32_t debugSlotBase = kMaxObjects;
    const uint32_t debugConstantBufferSlotBase = kMaxObjects +
        kPortalObjectSlotCount;
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
                kPortalRenderSlotsPerView + 2u + preparedDraws.size();
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
        const uint64_t debugRecords = static_cast<uint64_t>(kMaxObjects) +
            std::min(spatialDebugPasses.size(),
                static_cast<size_t>(kMaxSpatialDebugDraws));
        const uint64_t debugBytes = debugRecords * sizeof(ObjectGPUData);
        m_objectDataBuffer->FlushMappedWrites(0, debugBytes);
        m_lastObjectDataUploadBytes += debugBytes;
    }

    const auto drawConnectedSkybox = [&](const PortalViewJob& portalJob)
    {
        if (!skybox || !skybox->GetGraphicsTexture() ||
            !m_portalSkyboxStencilReadPipeline)
            return;
        context->SetPipeline(m_portalSkyboxStencilReadPipeline.get());
        context->SetConstantBuffer(0, m_skyboxConstantBuffer.get(),
            portalJob.skyboxConstantBufferOffset);
        context->SetTexture(0, skybox->GetGraphicsTexture());
        context->DrawInstanced(3, 1, 0, 0);
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
                                textureSlot < draw.textures.size(); ++textureSlot)
                            {
                                context->SetTexture(textureSlot,
                                    draw.textures[textureSlot]);
                            }
                            dx11Context->OMSetDepthStencilState(
                                stencilReadState.Get(), portalStencilRef);
                            submitGeometry(draw);
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
                    textureSlot < draw.textures.size(); ++textureSlot)
                {
                    context->SetTexture(textureSlot,
                        draw.textures[textureSlot]);
                }
                submitGeometry(draw);
            }

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
            ? static_cast<uint64_t>(kMaxObjects) + std::min(
                spatialDebugPasses.size(),
                static_cast<size_t>(kMaxSpatialDebugDraws))
            : static_cast<uint64_t>(preparedDraws.size());
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

    // Draw scene helpers (grid) after opaque objects so blending works correctly
    if (includeEditorVisuals && settings.showGrid && m_gridPipeline)
    {
        const glm::mat4 vp = proj * view;
        const glm::vec3& cp = cam->Owner->transform.position;

        // Prepare grid constant buffer data
        GridCBData gridData{};
        gridData.invVP = glm::inverse(vp);
        gridData.cameraPos = { cp.x, cp.y, cp.z };
        gridData.cellSize = settings.gridCellSize;
        gridData.gridColor = { settings.gridColor.x, settings.gridColor.y, settings.gridColor.z, settings.gridOpacity };
        gridData.axisColor = { settings.gridOriginColor.x, settings.gridOriginColor.y, settings.gridOriginColor.z, 1.f };
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

    // Screen-space retained UI is a game-view pass. The scene editor camera
    // should inspect UI objects in the scene without applying runtime
    // fullscreen composition that anchors to the active viewport.
    if (m_uiRenderer && (!includeEditorVisuals || settings.sceneViewUiOverlay))
        m_uiRenderer->Render(*this, context, aspect);
}

}
