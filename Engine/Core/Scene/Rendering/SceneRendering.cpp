#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/SpatialManipulator.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
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

// Large, indexed records live in shader-readable buffers on every backend.
struct ObjectGPUData
{
    glm::mat4 mvp;
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
static_assert(sizeof(ObjectGPUData) == 640, "Object buffer layout must match Object.hlsl");
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
        256);
    if (!m_skyboxConstantBuffer)
        throw std::runtime_error("Failed to create skybox constant buffer");
    m_skyboxCBMapped = m_skyboxConstantBuffer->Map();
    if (!m_skyboxCBMapped)
        throw std::runtime_error("Failed to map skybox constant buffer");

    // Engine::Core::Object constant buffer (256 * kMaxObjects bytes for per-object data)
    const uint64_t objectCBSize = static_cast<uint64_t>(kMaxObjects) * kCBStride;
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
        static_cast<uint64_t>(kMaxObjects) * sizeof(ObjectGPUData),
        nullptr, sizeof(ObjectGPUData));
    m_objectDataMapped = m_objectDataBuffer ? m_objectDataBuffer->Map() : nullptr;
    if (!m_objectDataMapped)
        throw std::runtime_error("Failed to create object structured buffer");

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
        static_cast<uint64_t>(kMaxObjects) * kMaxBonesPerObject * sizeof(glm::mat4),
        nullptr, sizeof(glm::mat4));
    m_boneDataMapped = m_boneDataBuffer ? m_boneDataBuffer->Map() : nullptr;
    if (!m_boneDataMapped)
        throw std::runtime_error("Failed to create bone palette structured buffer");

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

    auto builder = pipelineFactory->CreateBuilder();
    if (!builder)
        throw std::runtime_error("Failed to create skybox pipeline builder");
    m_skyboxPipeline = builder->SetVertexShader(vertexShader.get())
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
        .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 40)
        .Build();
    if (!m_skyboxPipeline)
        throw std::runtime_error("Failed to build skybox pipeline: " + builder->GetLastError());

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
        .SetRenderTargetFormat(28, 40)         // DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT
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

    enum class PortalStencilMode
    {
        None,
        Write,
        Read
    };

    auto buildMaterialPipeline = [&](bool doubleSided, bool blend,
                                     bool wireframe,
                                     PortalStencilMode stencilMode,
                                     bool colorWriteEnabled,
                                     const char* description)
    {
        auto materialBuilder = pipelineFactory->CreateBuilder();
        if (!materialBuilder)
            throw std::runtime_error(
                std::string("Failed to create ") + description +
                " pipeline state builder");
        auto& state = materialBuilder->SetVertexShader(vsShader.get())
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
        case PortalStencilMode::Read:
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
            .SetDepthWriteEnable(stencilMode == PortalStencilMode::Write
                ? false : !blend)
            .SetDepthFunc(stencilMode == PortalStencilMode::Write
                ? 3 : (blend ? 3 : 1))
            .SetInputLayout(layout, 10)
            .SetPrimitiveTopology(
                Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(28, 40)
            .Build();
        if (!pipeline)
            throw std::runtime_error(
                std::string("Failed to build ") + description +
                " pipeline: " + materialBuilder->GetLastError());
        return pipeline;
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
        .SetRenderTargetFormat(28, 40)
        .Build();
    if (!m_objectOutlinePipeline)
        throw std::runtime_error("Failed to build object outline pipeline: " + outlineBuilder->GetLastError());
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

    m_frameRenderItems.reserve(m_objects.size());
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
        item.world = candidate->transform.GetWorldMatrixWithLayer();

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

        if (skinPaletteSlot < kMaxObjects)
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
    }

    if (boneDataChanged)
        m_boneDataBuffer->FlushMappedWrites();
    m_renderFramePrepared = true;
}

// ---------------------------------------------------------------------------
// Scene::Render
// ---------------------------------------------------------------------------

void Scene::Render(Engine::Graphics::IGraphicsContext* context, float aspect,
    Engine::Components::Camera* cameraOverride, bool includeEditorVisuals)
{
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

    glm::mat4 view = cam->GetViewMatrix();
    if (m_editorMode2D && cam->Owner)
    {
        const glm::vec3 cameraWorld = cam->Owner->transform.GetWorldPosition();
        const glm::vec3 eye(cameraWorld.x, cameraWorld.y, -10.f);
        view = glm::lookAtLH(eye, eye + glm::vec3(0.f, 0.f, 1.f),
            glm::vec3(0.f, 1.f, 0.f));
    }
    const glm::mat4 proj = cam->GetProjectionMatrix(aspect, m_editorMode2D);
    const glm::vec3 cameraPosition = glm::vec3(glm::inverse(view)[3]);
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
        float worldDepth = 0.f;
        int sortingLayer = 0;
        bool blended = false;
    };
    std::vector<ViewRenderItem> renderObjects;
    renderObjects.reserve(m_frameRenderItems.size());
    for (const FrameRenderItem& item : m_frameRenderItems)
    {
        if (!item.belongsToPreview || includeEditorVisuals)
        {
            const glm::vec3 delta = glm::vec3(item.world[3]) - cameraPosition;
            renderObjects.push_back({ &item, glm::dot(delta, delta),
                item.world[3].z, item.sortingLayer, item.blended });
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
                : false;
        });

    struct PreparedDraw
    {
        Engine::Core::Object* object = nullptr;
        Engine::Graphics::IGraphicsBuffer* vertexBuffer = nullptr;
        Engine::Graphics::IPipelineState* pipeline = nullptr;
        std::array<const Engine::Graphics::IGraphicsTexture*, 7> textures{};
        UINT64 constantBufferOffset = 0;
        DrawCBData drawData{};
        ObjectGPUData objectData{};
        uint32_t vertexStride = 0;
        uint32_t vertexCount = 0;
        bool preview = false;
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
        PreparedDraw preparedDraw{};
        preparedDraw.object = obj;
        preparedDraw.preview = isPreview;
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
        glm::mat4 world = renderItem->world;
        if (sprite)
        {
            if (m_editorMode2D)
                world[3].z = 0.f;
            world = world * glm::scale(glm::mat4(1.f),
                glm::vec3(renderItem->spriteWorldSize, 1.f));
        }
        UINT64 offset = static_cast<UINT64>(slot) * kCBStride;

        ObjectGPUData objectData{};
        if (settings.hdriLightingEnabled && skybox)
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
        objectData.mvp = proj * view * world;
        objectData.world = world;
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
            prepareTexture(2, mat->normalTexture, 4u);
            prepareTexture(3, mat->occlusionTexture, 8u);
            prepareTexture(4, mat->emissiveTexture, 16u);
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
            objectData.environmentParams.z = mat->environmentDiffuseStrength;
            objectData.environmentParams.w = mat->reflectionStrength;
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

        const DrawCBData drawData{ slot,
            forceUnlitMode ? 0u : m_frameLightCount, 0u, 0u };
        preparedDraw.drawData = drawData;
        preparedDraw.objectData = objectData;
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
            : (mesh ? mesh->GetGraphicsBuffer() : nullptr);
        preparedDraw.vertexStride = sprite
            ? sprite->GetVertexStride() : mesh->GetVertexStride();
        preparedDraw.vertexCount = sprite
            ? sprite->GetVertexCount() : mesh->GetVertexCount();
        preparedDraws.push_back(preparedDraw);

        ++slot;
    }

    // DX11 buffers use CPU-side shadow storage. Upload the complete object
    // array once, then keep structured-buffer binding free of hidden copies.
    if (!preparedDraws.empty())
    {
        m_objectDataBuffer->FlushMappedWrites();
        context->SetStructuredBuffer(6, m_lightDataBuffer.get());
        context->SetStructuredBuffer(7, m_objectDataBuffer.get());
        context->SetStructuredBuffer(8, m_boneDataBuffer.get());
    }

    for (const PreparedDraw& draw : preparedDraws)
    {
        if (!draw.vertexBuffer)
            continue;
        context->SetPipeline(draw.pipeline);
        context->SetConstantBuffer(
            0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
        for (uint32_t textureSlot = 0; textureSlot < draw.textures.size(); ++textureSlot)
            context->SetTexture(textureSlot, draw.textures[textureSlot]);
        context->SetVertexBuffer(0, draw.vertexBuffer, draw.vertexStride, 0);
        context->DrawInstanced(draw.vertexCount, 1, 0, 0);

        // Draw selected object outline overlay. Structured buffers remain
        // bound across the pipeline change and do not need rebinding.
        if (includeEditorVisuals && !draw.preview &&
            draw.object == m_selectedObject && m_objectOutlinePipeline)
        {
            context->SetPipeline(m_objectOutlinePipeline.get());
            context->SetConstantBuffer(
                0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
            context->DrawInstanced(draw.vertexCount, 1, 0, 0);
        }
    }

#if defined(_WIN32)
    // Render linked-space view through portal aperture with a stencil mask.
    if (dynamic_cast<Engine::Renderers::D3D11GraphicsProvider*>(m_graphicsProvider))
    {
        ID3D11DeviceContext* dx11Context =
            static_cast<ID3D11DeviceContext*>(context->GetNativeHandle());
        if (dx11Context)
        {
            struct PortalStencilPass
            {
                const PreparedDraw* aperture = nullptr;
                Engine::Components::SpatialManipulator* source = nullptr;
                Engine::Components::SpatialManipulator* target = nullptr;
                glm::mat4 mappedView = glm::mat4(1.f);
                glm::vec3 mappedCameraPosition = glm::vec3(0.f);
            };

            PortalStencilPass portalPass{};
            for (const PreparedDraw& draw : preparedDraws)
            {
                if (!draw.object || !draw.vertexBuffer || draw.vertexCount < 3)
                    continue;

                auto* manipulator =
                    draw.object->GetComponent<Engine::Components::SpatialManipulator>();
                if (!manipulator || !manipulator->enabled)
                    continue;

                const auto mode = static_cast<Engine::Components::SpatialManipulator::ConnectionMode>(
                    manipulator->connectionMode);
                if (mode != Engine::Components::SpatialManipulator::ConnectionMode::Portal &&
                    mode != Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
                    continue;

                Engine::Components::SpatialManipulator* target =
                    manipulator->ResolveTarget();
                if (!target || !target->Owner)
                    continue;

                const glm::mat4 cameraWorld = cam->Owner
                    ? cam->Owner->transform.GetWorldMatrixWithLayer()
                    : glm::mat4(1.f);
                const glm::vec3 cameraForward = glm::normalize(glm::vec3(cameraWorld[2]));
                const glm::vec3 cameraUp = glm::normalize(glm::vec3(cameraWorld[1]));

                const glm::vec3 mappedCamera =
                    manipulator->MapWorldPointThroughPortalShape(cameraPosition, *target);
                const glm::vec3 mappedLookAt = manipulator->MapWorldPointThroughPortalShape(
                    cameraPosition + cameraForward, *target);
                const glm::vec3 mappedUpPoint = manipulator->MapWorldPointThroughPortalShape(
                    cameraPosition + cameraUp, *target);

                const glm::vec3 mappedForward = glm::normalize(mappedLookAt - mappedCamera);
                glm::vec3 mappedUp = mappedUpPoint - mappedCamera;
                if (glm::dot(mappedUp, mappedUp) <= 1e-6f)
                    mappedUp = glm::vec3(0.f, 1.f, 0.f);
                else
                    mappedUp = glm::normalize(mappedUp);

                portalPass.aperture = &draw;
                portalPass.source = manipulator;
                portalPass.target = target;
                portalPass.mappedCameraPosition = mappedCamera;
                portalPass.mappedView = glm::lookAtLH(
                    mappedCamera, mappedCamera + mappedForward, mappedUp);
                break;
            }

            if (portalPass.aperture)
            {
                ID3D11Device* dx11Device = nullptr;
                dx11Context->GetDevice(&dx11Device);
                if (dx11Device)
                {
                    static Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
                        stencilWriteState;
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
                    if (!colorMaskOffState)
                    {
                        D3D11_BLEND_DESC descriptor{};
                        descriptor.RenderTarget[0].BlendEnable = FALSE;
                        descriptor.RenderTarget[0].RenderTargetWriteMask = 0;
                        dx11Device->CreateBlendState(&descriptor,
                            &colorMaskOffState);
                    }
                    dx11Device->Release();

                    if (stencilWriteState && stencilReadState && colorMaskOffState)
                    {
                        constexpr UINT portalStencilRef = 1u;
                        const float blendFactor[4]{};

                        context->SetPipeline(portalPass.aperture->pipeline);
                        context->SetConstantBuffer(
                            0, m_objectConstantBuffer.get(),
                            portalPass.aperture->constantBufferOffset);
                        context->SetVertexBuffer(
                            0, portalPass.aperture->vertexBuffer,
                            portalPass.aperture->vertexStride, 0);
                        dx11Context->OMSetBlendState(colorMaskOffState.Get(),
                            blendFactor, UINT_MAX);
                        dx11Context->OMSetDepthStencilState(
                            stencilWriteState.Get(), portalStencilRef);
                        context->DrawInstanced(
                            portalPass.aperture->vertexCount, 1, 0, 0);

                        for (PreparedDraw& draw : preparedDraws)
                        {
                            if (!draw.object || !draw.vertexBuffer)
                                continue;
                            if (portalPass.source && portalPass.source->Owner == draw.object)
                                continue;

                            ObjectGPUData mappedData = draw.objectData;
                            mappedData.mvp = proj * portalPass.mappedView *
                                mappedData.world;
                            mappedData.viewPositionAlphaCutoff.x =
                                portalPass.mappedCameraPosition.x;
                            mappedData.viewPositionAlphaCutoff.y =
                                portalPass.mappedCameraPosition.y;
                            mappedData.viewPositionAlphaCutoff.z =
                                portalPass.mappedCameraPosition.z;
                            if (settings.portalDebugVisuals &&
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

                            memcpy(static_cast<uint8_t*>(m_objectCBMapped) +
                                draw.constantBufferOffset,
                                &draw.drawData, sizeof(draw.drawData));
                            memcpy(static_cast<uint8_t*>(m_objectDataMapped) +
                                static_cast<size_t>(draw.drawData.objectIndex) *
                                sizeof(ObjectGPUData),
                                &mappedData, sizeof(mappedData));
                        }

                        m_objectDataBuffer->FlushMappedWrites();
                        context->SetStructuredBuffer(6, m_lightDataBuffer.get());
                        context->SetStructuredBuffer(7, m_objectDataBuffer.get());
                        context->SetStructuredBuffer(8, m_boneDataBuffer.get());

                        for (const PreparedDraw& draw : preparedDraws)
                        {
                            if (!draw.object || !draw.vertexBuffer)
                                continue;
                            if (portalPass.source && portalPass.source->Owner == draw.object)
                                continue;

                            context->SetPipeline(draw.pipeline);
                            context->SetConstantBuffer(
                                0, m_objectConstantBuffer.get(),
                                draw.constantBufferOffset);
                            for (uint32_t textureSlot = 0;
                                textureSlot < draw.textures.size(); ++textureSlot)
                            {
                                context->SetTexture(textureSlot,
                                    draw.textures[textureSlot]);
                            }
                            context->SetVertexBuffer(0, draw.vertexBuffer,
                                draw.vertexStride, 0);
                            dx11Context->OMSetDepthStencilState(
                                stencilReadState.Get(), portalStencilRef);
                            context->DrawInstanced(draw.vertexCount, 1, 0, 0);
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
    const bool isDx12Provider =
        dynamic_cast<Engine::Renderers::D3D12GraphicsProvider*>(m_graphicsProvider) != nullptr;
#if defined(ENGINE_VULKAN_ENABLED)
    const bool isVulkanProvider =
        dynamic_cast<Engine::Renderers::VulkanGraphicsProvider*>(m_graphicsProvider) != nullptr;
#else
    const bool isVulkanProvider = false;
#endif

    if ((isDx12Provider
#if defined(ENGINE_VULKAN_ENABLED)
            || isVulkanProvider
#endif
        ) && m_objectPortalStencilWritePipeline)
    {
        struct PortalStencilPass
        {
            const PreparedDraw* aperture = nullptr;
            Engine::Components::SpatialManipulator* source = nullptr;
            Engine::Components::SpatialManipulator* target = nullptr;
            glm::mat4 mappedView = glm::mat4(1.f);
            glm::vec3 mappedCameraPosition = glm::vec3(0.f);
        };

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

        PortalStencilPass portalPass{};
        for (const PreparedDraw& draw : preparedDraws)
        {
            if (!draw.object || !draw.vertexBuffer || draw.vertexCount < 3)
                continue;

            auto* manipulator =
                draw.object->GetComponent<Engine::Components::SpatialManipulator>();
            if (!manipulator || !manipulator->enabled)
                continue;

            const auto mode = static_cast<Engine::Components::SpatialManipulator::ConnectionMode>(
                manipulator->connectionMode);
            if (mode != Engine::Components::SpatialManipulator::ConnectionMode::Portal &&
                mode != Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
                continue;

            Engine::Components::SpatialManipulator* target =
                manipulator->ResolveTarget();
            if (!target || !target->Owner)
                continue;

            const glm::mat4 cameraWorld = cam->Owner
                ? cam->Owner->transform.GetWorldMatrixWithLayer()
                : glm::mat4(1.f);
            const glm::vec3 cameraForward = glm::normalize(glm::vec3(cameraWorld[2]));
            const glm::vec3 cameraUp = glm::normalize(glm::vec3(cameraWorld[1]));

            const glm::vec3 mappedCamera =
                manipulator->MapWorldPointThroughPortalShape(cameraPosition, *target);
            const glm::vec3 mappedLookAt = manipulator->MapWorldPointThroughPortalShape(
                cameraPosition + cameraForward, *target);
            const glm::vec3 mappedUpPoint = manipulator->MapWorldPointThroughPortalShape(
                cameraPosition + cameraUp, *target);

            const glm::vec3 mappedForward = glm::normalize(mappedLookAt - mappedCamera);
            glm::vec3 mappedUp = mappedUpPoint - mappedCamera;
            if (glm::dot(mappedUp, mappedUp) <= 1e-6f)
                mappedUp = glm::vec3(0.f, 1.f, 0.f);
            else
                mappedUp = glm::normalize(mappedUp);

            portalPass.aperture = &draw;
            portalPass.source = manipulator;
            portalPass.target = target;
            portalPass.mappedCameraPosition = mappedCamera;
            portalPass.mappedView = glm::lookAtLH(
                mappedCamera, mappedCamera + mappedForward, mappedUp);
            break;
        }

        if (portalPass.aperture)
        {
#if defined(_WIN32)
            if (isDx12Provider)
            {
                if (ID3D12GraphicsCommandList* dx12CommandList =
                    static_cast<ID3D12GraphicsCommandList*>(context->GetNativeHandle()))
                {
                    dx12CommandList->OMSetStencilRef(1u);
                }
            }
#endif

            context->SetPipeline(m_objectPortalStencilWritePipeline.get());
            context->SetConstantBuffer(
                0, m_objectConstantBuffer.get(),
                portalPass.aperture->constantBufferOffset);
            context->SetVertexBuffer(
                0, portalPass.aperture->vertexBuffer,
                portalPass.aperture->vertexStride, 0);
            context->DrawInstanced(portalPass.aperture->vertexCount, 1, 0, 0);

            for (PreparedDraw& draw : preparedDraws)
            {
                if (!draw.object || !draw.vertexBuffer)
                    continue;
                if (portalPass.source && portalPass.source->Owner == draw.object)
                    continue;

                ObjectGPUData mappedData = draw.objectData;
                mappedData.mvp = proj * portalPass.mappedView * mappedData.world;
                mappedData.viewPositionAlphaCutoff.x =
                    portalPass.mappedCameraPosition.x;
                mappedData.viewPositionAlphaCutoff.y =
                    portalPass.mappedCameraPosition.y;
                mappedData.viewPositionAlphaCutoff.z =
                    portalPass.mappedCameraPosition.z;
                if (settings.portalDebugVisuals &&
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

                memcpy(static_cast<uint8_t*>(m_objectCBMapped) +
                    draw.constantBufferOffset,
                    &draw.drawData, sizeof(draw.drawData));
                memcpy(static_cast<uint8_t*>(m_objectDataMapped) +
                    static_cast<size_t>(draw.drawData.objectIndex) *
                    sizeof(ObjectGPUData),
                    &mappedData, sizeof(mappedData));
            }

            m_objectDataBuffer->FlushMappedWrites();
            context->SetStructuredBuffer(6, m_lightDataBuffer.get());
            context->SetStructuredBuffer(7, m_objectDataBuffer.get());
            context->SetStructuredBuffer(8, m_boneDataBuffer.get());

            for (const PreparedDraw& draw : preparedDraws)
            {
                if (!draw.object || !draw.vertexBuffer)
                    continue;
                if (portalPass.source && portalPass.source->Owner == draw.object)
                    continue;

                context->SetPipeline(resolveStencilReadPipeline(draw.pipeline));
                context->SetConstantBuffer(
                    0, m_objectConstantBuffer.get(),
                    draw.constantBufferOffset);
                for (uint32_t textureSlot = 0;
                    textureSlot < draw.textures.size(); ++textureSlot)
                {
                    context->SetTexture(textureSlot,
                        draw.textures[textureSlot]);
                }
                context->SetVertexBuffer(0, draw.vertexBuffer,
                    draw.vertexStride, 0);
                context->DrawInstanced(draw.vertexCount, 1, 0, 0);
            }
        }
    }
#endif

    if (settings.portalDebugVisuals)
    {
        const float debugAlpha = std::clamp(
            settings.portalDebugOverlayAlpha, 0.f, 1.f);
        Engine::Graphics::IPipelineState* portalDebugPipeline =
            settings.portalDebugWireframe
                ? m_objectPreviewWireDoubleSidedPipeline.get()
                : m_objectPreviewDoubleSidedPipeline.get();

        if (portalDebugPipeline)
        {
            for (const PreparedDraw& draw : preparedDraws)
            {
                if (!draw.object || !draw.vertexBuffer)
                    continue;

                auto* manipulator =
                    draw.object->GetComponent<Engine::Components::SpatialManipulator>();
                if (!manipulator || !manipulator->enabled)
                    continue;

                const auto mode = static_cast<Engine::Components::SpatialManipulator::ConnectionMode>(
                    manipulator->connectionMode);
                if (mode != Engine::Components::SpatialManipulator::ConnectionMode::Portal &&
                    mode != Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
                    continue;

                DrawCBData debugDraw = draw.drawData;
                debugDraw.lightCount = 0u;

                ObjectGPUData debugData = draw.objectData;
                debugData.baseColor = { 0.08f, 0.95f, 1.0f, debugAlpha };
                debugData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
                debugData.emissiveOcclusion = { 0.25f, 0.9f, 1.0f, 1.f };
                debugData.materialParams.w = 0.f;
                debugData.viewPositionAlphaCutoff.w = 0.001f;

                memcpy(static_cast<uint8_t*>(m_objectCBMapped) +
                    draw.constantBufferOffset,
                    &debugDraw, sizeof(debugDraw));
                memcpy(static_cast<uint8_t*>(m_objectDataMapped) +
                    static_cast<size_t>(debugDraw.objectIndex) *
                    sizeof(ObjectGPUData),
                    &debugData, sizeof(debugData));
            }

            m_objectDataBuffer->FlushMappedWrites();
            context->SetStructuredBuffer(6, m_lightDataBuffer.get());
            context->SetStructuredBuffer(7, m_objectDataBuffer.get());
            context->SetStructuredBuffer(8, m_boneDataBuffer.get());

            for (const PreparedDraw& draw : preparedDraws)
            {
                if (!draw.object || !draw.vertexBuffer)
                    continue;

                auto* manipulator =
                    draw.object->GetComponent<Engine::Components::SpatialManipulator>();
                if (!manipulator || !manipulator->enabled)
                    continue;

                const auto mode = static_cast<Engine::Components::SpatialManipulator::ConnectionMode>(
                    manipulator->connectionMode);
                if (mode != Engine::Components::SpatialManipulator::ConnectionMode::Portal &&
                    mode != Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
                    continue;

                context->SetPipeline(portalDebugPipeline);
                context->SetConstantBuffer(
                    0, m_objectConstantBuffer.get(),
                    draw.constantBufferOffset);
                context->SetVertexBuffer(0, draw.vertexBuffer,
                    draw.vertexStride, 0);
                context->DrawInstanced(draw.vertexCount, 1, 0, 0);
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
