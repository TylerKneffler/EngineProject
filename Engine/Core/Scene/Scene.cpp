#include "Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Light.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/Lighting/LightingTypes.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IShader.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <glm/glm.hpp>

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace
{
    JsonValue Vec3ToJson(const glm::vec3& value)
    {
        return JsonValue::MakeArray()
            .Push(JsonValue(value.x))
            .Push(JsonValue(value.y))
            .Push(JsonValue(value.z));
    }

    glm::vec3 Vec3FromJson(const JsonValue& value, const glm::vec3& fallback)
    {
        if (!value.IsArray() || value.ArraySize() < 3)
            return fallback;
        return {
            value.ArrayAt(0).AsFloat(),
            value.ArrayAt(1).AsFloat(),
            value.ArrayAt(2).AsFloat()
        };
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
}

JsonValue SceneSettings::Serialize() const
{
    JsonValue value = JsonValue::MakeObject();
    value.Set("showGrid", JsonValue(showGrid));
    value.Set("gridHalfSize", JsonValue(gridHalfSize));
    value.Set("gridCellSize", JsonValue(gridCellSize));
    value.Set("gridOpacity", JsonValue(gridOpacity));
    value.Set("gridFadeDistance", JsonValue(gridFadeDistance));
    value.Set("gridColor", Vec3ToJson(gridColor));
    value.Set("gridOriginColor", Vec3ToJson(gridOriginColor));
    value.Set("ambientColor", Vec3ToJson(ambientColor));
    value.Set("skyboxTexture", JsonValue(skyboxTexture));
    return value;
}

void SceneSettings::Deserialize(const JsonValue& value)
{
    if (!value.IsObject())
        return;
    if (value.Has("showGrid")) showGrid = value["showGrid"].AsBool();
    if (value.Has("gridHalfSize")) gridHalfSize = value["gridHalfSize"].AsInt();
    if (value.Has("gridCellSize")) gridCellSize = value["gridCellSize"].AsFloat();
    if (value.Has("gridOpacity")) gridOpacity = value["gridOpacity"].AsFloat();
    if (value.Has("gridFadeDistance")) gridFadeDistance = value["gridFadeDistance"].AsFloat();
    if (value.Has("gridColor"))
        gridColor = Vec3FromJson(value["gridColor"], gridColor);
    if (value.Has("gridOriginColor"))
        gridOriginColor = Vec3FromJson(value["gridOriginColor"], gridOriginColor);
    if (value.Has("ambientColor"))
        ambientColor = Vec3FromJson(value["ambientColor"], ambientColor);
    skyboxTexture = value.Has("skyboxTexture")
        ? value["skyboxTexture"].AsString()
        : std::string{};
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
    glm::vec4 specularShininess;
    glm::vec4 bakedDirectional;
    glm::vec4 bakedLightDirection;
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
    glm::vec3 padding;  // Constant-buffer allocation uses a 256-byte stride.
};

struct SkyboxCBData
{
    glm::mat4 invVP;
};

static_assert(sizeof(DrawCBData) == 16, "Draw constants must remain small");
static_assert(sizeof(ObjectGPUData) == 240, "Object buffer layout must match Object.hlsl");
static_assert(sizeof(Engine::Rendering::Lighting::LightData) == 48,
    "Light buffer layout must match Object.hlsl");
static_assert(sizeof(GridCBData) == 128, "Grid constant-buffer layout must match Grid.hlsl");
static_assert(sizeof(SkyboxCBData) == 64, "Skybox constant-buffer layout must match Skybox.hlsl");

// ---------------------------------------------------------------------------
// Scene::Init
// ---------------------------------------------------------------------------

void Scene::Init(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider)
        throw std::runtime_error("Scene::Init requires a non-null IGraphicsProvider");

    m_graphicsProvider = graphicsProvider;

    // Create constant buffers via the graphics buffer factory
    auto* bufferFactory = m_graphicsProvider->GetBufferFactory();
    if (!bufferFactory)
        throw std::runtime_error("Failed to get buffer factory from graphics provider");

    // Grid constant buffer (256 bytes, uploadable)
    m_gridConstantBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ConstantBuffer,
        IGraphicsBuffer::AccessMode::Upload,
        256);
    if (!m_gridConstantBuffer)
        throw std::runtime_error("Failed to create grid constant buffer");
    m_gridCBMapped = m_gridConstantBuffer->Map();
    if (!m_gridCBMapped)
        throw std::runtime_error("Failed to map grid constant buffer");

    m_skyboxConstantBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ConstantBuffer,
        IGraphicsBuffer::AccessMode::Upload,
        256);
    if (!m_skyboxConstantBuffer)
        throw std::runtime_error("Failed to create skybox constant buffer");
    m_skyboxCBMapped = m_skyboxConstantBuffer->Map();
    if (!m_skyboxCBMapped)
        throw std::runtime_error("Failed to map skybox constant buffer");

    // Object constant buffer (256 * kMaxObjects bytes for per-object data)
    const uint64_t objectCBSize = static_cast<uint64_t>(kMaxObjects) * kCBStride;
    m_objectConstantBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ConstantBuffer,
        IGraphicsBuffer::AccessMode::Upload,
        objectCBSize, nullptr, sizeof(DrawCBData));
    if (!m_objectConstantBuffer)
        throw std::runtime_error("Failed to create object constant buffer");
    m_objectCBMapped = m_objectConstantBuffer->Map();
    if (!m_objectCBMapped)
        throw std::runtime_error("Failed to map object constant buffer");

    m_objectDataBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ShaderResource,
        IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(kMaxObjects) * sizeof(ObjectGPUData),
        nullptr, sizeof(ObjectGPUData));
    m_objectDataMapped = m_objectDataBuffer ? m_objectDataBuffer->Map() : nullptr;
    if (!m_objectDataMapped)
        throw std::runtime_error("Failed to create object structured buffer");

    m_lightDataBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ShaderResource,
        IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(kMaxLights) *
            sizeof(Engine::Rendering::Lighting::LightData),
        nullptr, sizeof(Engine::Rendering::Lighting::LightData));
    m_lightDataMapped = m_lightDataBuffer ? m_lightDataBuffer->Map() : nullptr;
    if (!m_lightDataMapped)
        throw std::runtime_error("Failed to create light structured buffer");

    // Set up the default editor camera
    Camera* editorCameraComponent = editorCamera.AddComponent<Camera>();
    editorCameraComponent->useTransformRotation = false;
    editorCamera.transform.position = { 0.f, 1.5f, -3.f };

    // Build pipeline states
    BuildGridPipeline();
    BuildSkyboxPipeline();
    BuildObjectPipeline();
}

void Scene::BuildSkyboxPipeline()
{
    auto* shaderCompiler = m_graphicsProvider->GetShaderCompiler();
    auto* pipelineFactory = m_graphicsProvider->GetPipelineStateFactory();
    if (!shaderCompiler || !pipelineFactory)
        throw std::runtime_error("Failed to get skybox shader or pipeline factory");

    const std::string shaderPath = EngineShaderPath("Skybox.hlsl");
    auto vertexShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "VSMain", IShaderCompiler::CompileProfile::VS_5_0);
    if (!vertexShader)
        throw std::runtime_error("Failed to compile skybox vertex shader: " + shaderCompiler->GetLastError());
    auto pixelShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(), "PSMain", IShaderCompiler::CompileProfile::PS_5_0);
    if (!pixelShader)
        throw std::runtime_error("Failed to compile skybox pixel shader: " + shaderCompiler->GetLastError());

    auto builder = pipelineFactory->CreateBuilder();
    if (!builder)
        throw std::runtime_error("Failed to create skybox pipeline builder");
    m_skyboxPipeline = builder->SetVertexShader(vertexShader.get())
        .SetPixelShader(pixelShader.get())
        .SetFillMode(false)
        .SetCullMode(false)
        .SetFrontCounterClockwise(true)
        .SetDepthClipEnable(false)
        .SetBlendEnable(false)
        .SetDepthEnable(false)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(7)
        .SetInputLayout(nullptr, 0)
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 40)
        .Build();
    if (!m_skyboxPipeline)
        throw std::runtime_error("Failed to build skybox pipeline: " + builder->GetLastError());

    const std::string defaultPath =
        (std::filesystem::path(ENGINE_ASSETS_PATH) / "Textures" / "Skyboxes" /
            "editor-default-sky.png").string();
    m_defaultSkyboxTexture = Texture::Acquire(defaultPath);
    if (!m_defaultSkyboxTexture->Prepare(m_graphicsProvider))
        m_defaultSkyboxTexture.reset();
}

const Texture* Scene::ResolveSkyboxTexture()
{
    if (settings.skyboxTexture.empty())
        return m_defaultSkyboxTexture.get();

    if (m_loadedSkyboxPath != settings.skyboxTexture)
    {
        m_loadedSkyboxPath = settings.skyboxTexture;
        m_sceneSkyboxTexture = Texture::Acquire(settings.skyboxTexture);
        if (!m_sceneSkyboxTexture->Prepare(m_graphicsProvider))
            m_sceneSkyboxTexture.reset();
    }
    return m_sceneSkyboxTexture ? m_sceneSkyboxTexture.get() : m_defaultSkyboxTexture.get();
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
        IShaderCompiler::CompileProfile::VS_5_0);
    if (!vsShader)
        throw std::runtime_error("Failed to compile grid vertex shader: " + shaderCompiler->GetLastError());

    auto psShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(),
        "PSMain",
        IShaderCompiler::CompileProfile::PS_5_0);
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
        .SetFrontCounterClockwise(true)
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
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
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
        IShaderCompiler::CompileProfile::VS_5_0);
    if (!vsShader)
        throw std::runtime_error("Failed to compile object vertex shader: " + shaderCompiler->GetLastError());

    auto psShader = shaderCompiler->CompileFromFile(
        shaderPath.c_str(),
        "PSMain",
        IShaderCompiler::CompileProfile::PS_5_0);
    if (!psShader)
        throw std::runtime_error("Failed to compile object pixel shader: " + shaderCompiler->GetLastError());

    // Define input layout: POSITION (float3) + NORMAL (float3)
    IPipelineStateBuilder::VertexElement layout[] =
    {
        { "POSITION", 0, 6, 0,  0, false },   // DXGI_FORMAT_R32G32B32_FLOAT = 6
        { "NORMAL",   0, 6, 0, 12, false },   // DXGI_FORMAT_R32G32B32_FLOAT = 6, offset 12
        { "TEXCOORD", 0, 16, 0, 24, false },  // DXGI_FORMAT_R32G32_FLOAT = 16
        { "TANGENT",  0, 2, 0, 32, false },   // DXGI_FORMAT_R32G32B32A32_FLOAT = 2
    };

    // Build pipeline state using fluent API
    auto builder = pipelineFactory->CreateBuilder();
    if (!builder)
        throw std::runtime_error("Failed to create pipeline state builder");

    auto& bp = *builder;
    m_objectPipeline = bp.SetVertexShader(vsShader.get())
        .SetPixelShader(psShader.get())
        .SetFillMode(false)                    // Solid fill
        .SetCullMode(true)                     // Back-face culling
        .SetFrontCounterClockwise(true)
        .SetDepthClipEnable(true)
        .SetBlendEnable(false)                 // No blending for opaque objects
        .SetDepthEnable(true)
        .SetDepthWriteEnable(true)
        .SetDepthFunc(1)                       // D3D12_COMPARISON_FUNC_LESS (0-indexed: 1)
        .SetInputLayout(layout, 4)
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 40)         // DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT
        .Build();
    if (!m_objectPipeline)
        throw std::runtime_error("Failed to build object pipeline: " + builder->GetLastError());

    // Build a wireframe outline pipeline for selected object highlighting.
    auto outlineBuilder = pipelineFactory->CreateBuilder();
    if (!outlineBuilder)
        throw std::runtime_error("Failed to create outline pipeline state builder");

    // Compile dedicated outline shaders from file instead of reusing object shader.
    const std::string outlineShaderPath = EngineShaderPath("ObjectOutline.hlsl");
    auto outlineVsShader = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(),
        "VSMain",
        IShaderCompiler::CompileProfile::VS_5_0);
    if (!outlineVsShader)
        throw std::runtime_error("Failed to compile object outline vertex shader: " + shaderCompiler->GetLastError());

    auto outlinePsShader = shaderCompiler->CompileFromFile(
        outlineShaderPath.c_str(),
        "PSMain",
        IShaderCompiler::CompileProfile::PS_5_0);
    if (!outlinePsShader)
        throw std::runtime_error("Failed to compile object outline pixel shader: " + shaderCompiler->GetLastError());

    auto& op = *outlineBuilder;
    m_objectOutlinePipeline = op.SetVertexShader(outlineVsShader.get())
        .SetPixelShader(outlinePsShader.get())
        .SetFillMode(true)                     // Wireframe outline
        .SetCullMode(false)
        .SetFrontCounterClockwise(true)
        .SetDepthClipEnable(true)
        .SetBlendEnable(false)
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(3)                       // D3D12_COMPARISON_FUNC_LESS_EQUAL
        .SetInputLayout(layout, 4)
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 40)
        .Build();
    if (!m_objectOutlinePipeline)
        throw std::runtime_error("Failed to build object outline pipeline: " + outlineBuilder->GetLastError());
}

// ---------------------------------------------------------------------------
// Scene::Render
// ---------------------------------------------------------------------------

Camera* Scene::FindGameCamera()
{
    for (const auto& obj : m_objects)
        if (Camera* cam = obj->GetComponent<Camera>())
            if (obj->IsEnabledInHierarchy() && cam->active)
                return cam;
    return nullptr;
}

void Scene::Render(IGraphicsContext* context, float aspect,
    Camera* cameraOverride, bool includeEditorVisuals)
{
    if (!context)
    {
        return;
    }

    if (!m_graphicsProvider || !m_objectPipeline)
    {
        return;
    }

    // Scene View always uses its navigation camera. Game View supplies its
    // active scene camera explicitly, so hierarchy selection cannot hijack
    // either viewport.
    Camera* cam = cameraOverride
        ? cameraOverride
        : editorCamera.GetComponent<Camera>();
    if (!cam)
    {
        return;
    }

    const glm::mat4 view = cam->GetViewMatrix();
    const glm::mat4 proj = cam->GetProjectionMatrix(aspect);

    if (const Texture* skybox = ResolveSkyboxTexture();
        skybox && skybox->GetGraphicsTexture() && m_skyboxPipeline)
    {
        SkyboxCBData skyboxData{};
        skyboxData.invVP = glm::inverse(proj * view);
        memcpy(m_skyboxCBMapped, &skyboxData, sizeof(skyboxData));
        context->SetPipeline(m_skyboxPipeline.get());
        context->SetConstantBuffer(0, m_skyboxConstantBuffer.get(), 0);
        context->SetTexture(0, skybox->GetGraphicsTexture());
        context->DrawInstanced(3, 1, 0, 0);
    }

    const uint32_t lightCount = m_realtimeLightingPipeline.CollectLights(
        *this,
        static_cast<Engine::Rendering::Lighting::LightData*>(m_lightDataMapped),
        kMaxLights);


    // Draw all scene objects that have a Mesh component
    UINT slot = 0;
    for (const auto& objPtr : m_objects)
    {
        if (slot >= kMaxObjects)
            break;

        Object* obj = objPtr.get();
        if (!obj->IsEnabledInHierarchy())
            continue;
        Mesh* mesh = obj->GetComponent<Mesh>();
        if (!mesh)
        {
            continue;
        }
        if (!mesh->IsReady())
        {
            continue;
        }
        

        Material* mat = obj->GetComponent<Material>();
        const BakedLightingData* bakedLighting =
            obj->GetComponent<BakedLightingData>();
        const glm::vec3 bakedIrradiance =
            bakedLighting && bakedLighting->valid
                ? bakedLighting->irradiance
                : glm::vec3(0.f);
        const glm::mat4 world = obj->transform.GetWorldMatrix();
        UINT64 offset = static_cast<UINT64>(slot) * kCBStride;

        ObjectGPUData objectData{};
        objectData.mvp = proj * view * world;
        objectData.world = world;

        if (mat)
        {
            mat->PrepareTextures(m_graphicsProvider);
            uint32_t textureFlags = 0;
            auto bindTexture = [&](uint32_t textureSlot,
                                   const std::shared_ptr<Texture>& texture,
                                   uint32_t flag)
            {
                const IGraphicsTexture* graphicsTexture =
                    texture ? texture->GetGraphicsTexture() : nullptr;
                context->SetTexture(textureSlot, graphicsTexture);
                if (graphicsTexture)
                    textureFlags |= flag;
            };
            bindTexture(0, mat->baseColorTexture, 1u);
            bindTexture(1, mat->metallicRoughnessTexture, 2u);
            bindTexture(2, mat->normalTexture, 4u);
            bindTexture(3, mat->occlusionTexture, 8u);
            bindTexture(4, mat->emissiveTexture, 16u);

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
            objectData.specularShininess = glm::vec4(
                mat->specularColor, mat->shininess);
        }
        else
        {
            objectData.baseColor = { 0.8f, 0.8f, 0.8f, 1.f };
            objectData.ambientUnlit = glm::vec4(
                settings.ambientColor + bakedIrradiance, 0.f);
            objectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
            objectData.materialParams = { 0.f, 1.f, 1.f, 0.f };
            objectData.specularShininess = { 1.f, 1.f, 1.f, 32.f };
        }
        if (bakedLighting && bakedLighting->valid)
        {
            objectData.bakedDirectional = glm::vec4(
                bakedLighting->directionalIrradiance, 0.f);
            objectData.bakedLightDirection = glm::vec4(
                bakedLighting->lightDirection, 1.f);
        }

        const DrawCBData drawData{ slot, lightCount, 0u, 0u };
        memcpy(static_cast<uint8_t*>(m_objectCBMapped) + offset,
            &drawData, sizeof(drawData));
        memcpy(static_cast<uint8_t*>(m_objectDataMapped) +
            static_cast<size_t>(slot) * sizeof(ObjectGPUData),
            &objectData, sizeof(objectData));

        context->SetPipeline(m_objectPipeline.get());
        context->SetConstantBuffer(0, m_objectConstantBuffer.get(), offset);
        context->SetStructuredBuffer(5, m_lightDataBuffer.get());
        context->SetStructuredBuffer(6, m_objectDataBuffer.get());

        // Set vertex buffer and draw
        IGraphicsBuffer* vertexBuffer = mesh->GetGraphicsBuffer();
        if (vertexBuffer)
        {
            context->SetVertexBuffer(0, vertexBuffer, mesh->GetVertexStride(), 0);
            context->DrawInstanced(mesh->GetVertexCount(), 1, 0, 0);

            // Draw selected object outline overlay.
            if (includeEditorVisuals && obj == m_selectedObject && m_objectOutlinePipeline)
            {
                context->SetPipeline(m_objectOutlinePipeline.get());
                context->SetConstantBuffer(0, m_objectConstantBuffer.get(), offset);
                context->SetStructuredBuffer(5, m_lightDataBuffer.get());
                context->SetStructuredBuffer(6, m_objectDataBuffer.get());
                context->DrawInstanced(mesh->GetVertexCount(), 1, 0, 0);
            }
        }

        ++slot;
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

        // Write to constant buffer
        memcpy(m_gridCBMapped, &gridData, sizeof(GridCBData));

        // Set pipeline and constant buffer
        context->SetPipeline(m_gridPipeline.get());
        context->SetConstantBuffer(0, m_gridConstantBuffer.get(), 0);

        // Draw fullscreen triangle (3 vertices, no vertex buffer)
        context->DrawInstanced(3, 1, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Scene::FocusEditorCamera
// ---------------------------------------------------------------------------

void Scene::FocusEditorCamera(Object* obj)
{
    Camera* cam = editorCamera.GetComponent<Camera>();
    if (!cam) return;

    // Determine the world-space target point (object origin, or world origin).
    glm::vec3 targetPos = obj ? obj->transform.position : glm::vec3(0.f, 0.f, 0.f);

    // Keep a fixed orbital distance from the object (3 units gives a nice view
    // of a 1-unit cube; scale this later if bounding-box info is available).
    constexpr float kDistance = 3.f;

    // Preserve the current orbital angle but re-aim at the new target.
    // Compute the current direction from camera to its old target, then apply
    // the same direction offset to the new target.
    const glm::vec3& eye = editorCamera.transform.position;
    const glm::vec3 oldTarget = cam->target;
    glm::vec3 oldDir = eye - oldTarget;
    float oldLen = std::sqrt(oldDir.x*oldDir.x + oldDir.y*oldDir.y + oldDir.z*oldDir.z);
    if (oldLen < 0.001f)
    {
        // Camera was sitting on its target — use a default offset.
        oldDir = glm::vec3(0.f, 1.5f, -1.f);
        oldLen = std::sqrt(oldDir.x*oldDir.x + oldDir.y*oldDir.y + oldDir.z*oldDir.z);
    }
    const float s = kDistance / oldLen;
    editorCamera.transform.position = glm::vec3(
        targetPos.x + oldDir.x * s,
        targetPos.y + oldDir.y * s,
        targetPos.z + oldDir.z * s);
    cam->target = { targetPos.x, targetPos.y, targetPos.z };
}

// ---------------------------------------------------------------------------
// Scene::Object management
// ---------------------------------------------------------------------------

Object* Scene::AddObject()
{
    auto obj = std::make_unique<Object>();
    Object* raw = obj.get();
    raw->OwnerScene = this;
    m_objects.push_back(std::move(obj));
    return raw;
}

Object* Scene::AddObject(const std::string& name)
{
    Object* obj = AddObject();
    obj->name = name;
    obj->OwnerScene = this;
    return obj;
}

bool Scene::TryGetObjectPath(const Object* object, ObjectPath& path) const
{
    path.clear();
    if (!object)
        return false;

    for (const Object* current = object; current; current = current->Parent)
    {
        std::size_t index = 0;
        bool found = false;
        if (current->Parent)
        {
            const auto& siblings = current->Parent->Children;
            const auto position = std::find(siblings.begin(), siblings.end(), current);
            if (position != siblings.end())
            {
                index = static_cast<std::size_t>(position - siblings.begin());
                found = true;
            }
        }
        else
        {
            for (const auto& candidate : m_objects)
            {
                if (candidate->Parent)
                    continue;
                if (candidate.get() == current)
                {
                    found = true;
                    break;
                }
                ++index;
            }
        }
        if (!found)
        {
            path.clear();
            return false;
        }
        path.push_back(index);
    }

    std::reverse(path.begin(), path.end());
    return true;
}

Object* Scene::FindObjectByPath(const ObjectPath& path) const
{
    if (path.empty())
        return nullptr;

    Object* current = nullptr;
    std::size_t rootIndex = 0;
    for (const auto& candidate : m_objects)
    {
        if (candidate->Parent)
            continue;
        if (rootIndex++ == path.front())
        {
            current = candidate.get();
            break;
        }
    }
    if (!current)
        return nullptr;

    for (std::size_t depth = 1; depth < path.size(); ++depth)
    {
        if (path[depth] >= current->Children.size())
            return nullptr;
        current = current->Children[path[depth]];
    }
    return current;
}

bool Scene::MoveObject(Object* object, Object* target, ObjectPlacement placement)
{
    if (!object || object == target)
        return false;

    Object* newParent = placement == ObjectPlacement::AsChild
        ? target : (target ? target->Parent : nullptr);
    for (Object* ancestor = newParent; ancestor; ancestor = ancestor->Parent)
        if (ancestor == object)
            return false;

    const glm::mat4 oldWorld = object->transform.GetWorldMatrix();
    if (object->Parent)
    {
        auto& oldSiblings = object->Parent->Children;
        oldSiblings.erase(std::remove(oldSiblings.begin(), oldSiblings.end(), object),
            oldSiblings.end());
    }
    object->Parent = newParent;

    if (newParent)
    {
        auto& siblings = newParent->Children;
        if (placement == ObjectPlacement::AsChild || !target)
            siblings.push_back(object);
        else
        {
            auto position = std::find(siblings.begin(), siblings.end(), target);
            if (position == siblings.end())
                siblings.push_back(object);
            else
                siblings.insert(placement == ObjectPlacement::After
                    ? position + 1 : position, object);
        }
    }
    else
    {
        auto moving = std::find_if(m_objects.begin(), m_objects.end(),
            [object](const auto& value) { return value.get() == object; });
        if (moving == m_objects.end())
            return false;
        std::unique_ptr<Object> owned = std::move(*moving);
        m_objects.erase(moving);
        auto position = target
            ? std::find_if(m_objects.begin(), m_objects.end(),
                [target](const auto& value) { return value.get() == target; })
            : m_objects.end();
        if (position == m_objects.end())
            m_objects.push_back(std::move(owned));
        else
            m_objects.insert(placement == ObjectPlacement::After
                ? position + 1 : position, std::move(owned));
    }

    const glm::mat4 parentWorld = newParent
        ? newParent->transform.GetWorldMatrix() : glm::mat4(1.f);
    const glm::mat4 local = glm::inverse(parentWorld) * oldWorld;
    object->transform.position = glm::vec3(local[3]);
    object->transform.scale = {
        glm::length(glm::vec3(local[0])),
        glm::length(glm::vec3(local[1])),
        glm::length(glm::vec3(local[2]))
    };
    glm::mat3 rotation(1.f);
    for (int column = 0; column < 3; ++column)
    {
        const float scale = object->transform.scale[column];
        if (scale > 0.000001f)
            rotation[column] = glm::vec3(local[column]) / scale;
    }
    const float y = std::asin(std::clamp(-rotation[0][2], -1.f, 1.f));
    const float cosineY = std::cos(y);
    const float x = std::abs(cosineY) > 0.00001f
        ? std::atan2(rotation[1][2], rotation[2][2])
        : std::atan2(-rotation[2][1], rotation[1][1]);
    const float z = std::abs(cosineY) > 0.00001f
        ? std::atan2(rotation[0][1], rotation[0][0])
        : 0.f;
    object->transform.rotation = { x, y, z };
    return true;
}

void Scene::RemoveObject(Object* obj)
{
    if (!obj)
        return;

    std::vector<Object*> objectsToRemove;
    std::function<void(Object*)> collect = [&](Object* current)
    {
        if (!current)
            return;
        objectsToRemove.push_back(current);
        for (Object* child : current->Children)
            collect(child);
    };
    collect(obj);

    if (obj->Parent)
    {
        auto& siblings = obj->Parent->Children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), obj), siblings.end());
        obj->Parent = nullptr;
    }
    if (m_selectedObject &&
        std::find(objectsToRemove.begin(), objectsToRemove.end(), m_selectedObject) != objectsToRemove.end())
        m_selectedObject = nullptr;

    m_objects.erase(
        std::remove_if(m_objects.begin(), m_objects.end(),
            [&](const std::unique_ptr<Object>& p)
            {
                return std::find(objectsToRemove.begin(), objectsToRemove.end(), p.get()) !=
                    objectsToRemove.end();
            }),
        m_objects.end());
}

void Scene::ClearObjects()
{
    m_objects.clear();
    m_selectedObject = nullptr;
}

bool Scene::Save(const std::string& path) const
{
    return SceneSerializer::Save(*this, path);
}

bool Scene::Load(const std::string& path)
{
    if (!m_graphicsProvider)
        return false;
    return SceneSerializer::Load(*this, path, m_graphicsProvider);
}

std::string Scene::SaveToString() const
{
    return SceneSerializer::SaveToString(*this);
}

bool Scene::LoadFromString(const std::string& source)
{
    if (!m_graphicsProvider)
        return false;
    return SceneSerializer::LoadFromString(*this, source, m_graphicsProvider);
}

Engine::Rendering::Lighting::BakeResult Scene::BakeLighting()
{
    auto result = m_bakedLightingPipeline.Bake(*this);
    m_lightingBakeStatus = result.message;
    return result;
}

void Scene::ClearBakedLighting()
{
    const uint32_t count = m_bakedLightingPipeline.Clear(*this);
    m_lightingBakeStatus = "Cleared " + std::to_string(count) +
        " baked lighting record(s).";
}
