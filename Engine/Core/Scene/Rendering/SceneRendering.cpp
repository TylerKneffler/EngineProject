#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/Lighting/LightingTypes.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IShader.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

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

    bool IsObjectOrDescendant(const Object* object, const Object* root)
    {
        for (const Object* current = object; current; current = current->Parent)
            if (current == root)
                return true;
        return false;
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
static_assert(sizeof(ObjectGPUData) == 320, "Object buffer layout must match Object.hlsl");
static_assert(sizeof(Engine::Rendering::Lighting::LightData) == 48,
    "Light buffer layout must match Object.hlsl");
static_assert(sizeof(GridCBData) == 128, "Grid constant-buffer layout must match Grid.hlsl");
static_assert(sizeof(SkyboxCBData) == 80, "Skybox constant-buffer layout must match Skybox.hlsl");

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

    m_boneDataBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ShaderResource,
        IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(kMaxObjects) * kMaxBonesPerObject * sizeof(glm::mat4),
        nullptr, sizeof(glm::mat4));
    m_boneDataMapped = m_boneDataBuffer ? m_boneDataBuffer->Map() : nullptr;
    if (!m_boneDataMapped)
        throw std::runtime_error("Failed to create bone palette structured buffer");

    // Set up the default editor camera
    Camera* editorCameraComponent = editorCamera.AddComponent<Camera>();
    editorCameraComponent->useTransformRotation = false;
    SetEditorMode2D(m_editorMode2D);

    // Build pipeline states
    BuildGridPipeline();
    BuildSkyboxPipeline();
    BuildObjectPipeline();
}

void Scene::SetEditorMode2D(bool enabled)
{
    if (m_editorCameraModeInitialized && m_editorMode2D == enabled)
        return;
    m_editorMode2D = enabled;
    Camera* camera = editorCamera.GetComponent<Camera>();
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

const Texture* Scene::GetSkyboxPreviewTexture()
{
    return ResolveSkyboxTexture();
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

    // Engine-native model stream, including the second UV set and vertex colour.
    IPipelineStateBuilder::VertexElement layout[] =
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

    auto buildMaterialPipeline = [&](bool doubleSided, bool blend,
                                     const char* description)
    {
        auto materialBuilder = pipelineFactory->CreateBuilder();
        if (!materialBuilder)
            throw std::runtime_error(
                std::string("Failed to create ") + description +
                " pipeline state builder");
        auto& state = materialBuilder->SetVertexShader(vsShader.get())
            .SetPixelShader(psShader.get())
            .SetFillMode(false)
            .SetCullMode(!doubleSided)
            .SetFrontCounterClockwise(true)
            .SetDepthClipEnable(true)
            .SetBlendEnable(blend);
        if (blend)
        {
            state.SetSrcBlend(4)
                .SetDestBlend(5)
                .SetBlendOp(0)
                .SetSrcBlendAlpha(1)
                .SetDestBlendAlpha(0)
                .SetBlendOpAlpha(0);
        }
        auto pipeline = state.SetDepthEnable(true)
            .SetDepthWriteEnable(!blend)
            .SetDepthFunc(blend ? 3 : 1)
            .SetInputLayout(layout, 10)
            .SetPrimitiveTopology(
                IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(28, 40)
            .Build();
        if (!pipeline)
            throw std::runtime_error(
                std::string("Failed to build ") + description +
                " pipeline: " + materialBuilder->GetLastError());
        return pipeline;
    };

    m_objectPipeline = buildMaterialPipeline(false, false, "opaque material");
    m_objectDoubleSidedPipeline =
        buildMaterialPipeline(true, false, "double-sided material");
    m_objectBlendPipeline =
        buildMaterialPipeline(false, true, "blended material");
    m_objectBlendDoubleSidedPipeline =
        buildMaterialPipeline(true, true, "blended double-sided material");

    // Placement previews always blend, regardless of the source alpha mode.
    m_objectPreviewPipeline =
        buildMaterialPipeline(false, true, "object preview");
    m_objectPreviewDoubleSidedPipeline =
        buildMaterialPipeline(true, true, "double-sided object preview");

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
        .SetInputLayout(layout, 10)
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 40)
        .Build();
    if (!m_objectOutlinePipeline)
        throw std::runtime_error("Failed to build object outline pipeline: " + outlineBuilder->GetLastError());
}

// ---------------------------------------------------------------------------
// Scene::Render
// ---------------------------------------------------------------------------

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

    if (const Texture* skybox = ResolveSkyboxTexture();
        skybox && skybox->GetGraphicsTexture() && m_skyboxPipeline)
    {
        SkyboxCBData skyboxData{};
        skyboxData.invVP = glm::inverse(proj * view);
        skyboxData.displayParams.x = m_editorMode2D ? 1.f : 0.f;
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


    // Opaque and masked materials render first. Blended materials render
    // back-to-front with depth writes disabled.
    std::vector<Object*> renderObjects;
    renderObjects.reserve(m_objects.size());
    for (const auto& object : m_objects)
    {
        Object* candidate = object.get();
        const bool preview = m_previewObject &&
            IsObjectOrDescendant(candidate, m_previewObject);
        Mesh* mesh = candidate->GetComponent<Mesh>();
        Sprite* sprite = candidate->GetComponent<Sprite>();
        if (sprite)
            sprite->Prepare(m_graphicsProvider);
        const bool renderable = (sprite && sprite->IsReady()) ||
            (mesh && mesh->IsReady());
        if (candidate->IsEnabledInHierarchy() && renderable &&
            (!preview || includeEditorVisuals))
            renderObjects.push_back(candidate);
    }
    auto isBlended = [&](Object* object)
    {
        if (m_previewObject && IsObjectOrDescendant(object, m_previewObject))
            return true;
        if (object->GetComponent<Sprite>())
            return true;
        const Material* material = object->GetComponent<Material>();
        return material &&
            material->GetAlphaMode() == MaterialAlphaMode::Blend;
    };
    auto distanceSquared = [&](Object* object)
    {
        const glm::vec3 delta =
            glm::vec3(object->transform.GetWorldMatrix()[3]) - cameraPosition;
        return glm::dot(delta, delta);
    };
    std::stable_sort(renderObjects.begin(), renderObjects.end(),
        [&](Object* first, Object* second)
        {
            if (m_editorMode2D)
            {
                const Sprite* firstSprite = first->GetComponent<Sprite>();
                const Sprite* secondSprite = second->GetComponent<Sprite>();
                const int firstLayer = firstSprite ? firstSprite->sortingLayer : 0;
                const int secondLayer = secondSprite ? secondSprite->sortingLayer : 0;
                if (firstLayer != secondLayer)
                    return firstLayer < secondLayer;
                return first->transform.GetWorldPosition().z <
                    second->transform.GetWorldPosition().z;
            }
            const bool firstBlend = isBlended(first);
            const bool secondBlend = isBlended(second);
            if (firstBlend != secondBlend)
                return !firstBlend;
            return firstBlend
                ? distanceSquared(first) > distanceSquared(second)
                : false;
        });

    // Populate every palette before the first draw. This lets D3D11 upload the
    // large shared palette once while D3D12/Vulkan read the same mapped data.
    std::vector<uint32_t> skinJointCounts(
        std::min<size_t>(renderObjects.size(), kMaxObjects), 0u);
    for (size_t skinSlot = 0; skinSlot < skinJointCounts.size(); ++skinSlot)
    {
        if (SkinnedMesh* skinned = renderObjects[skinSlot]->GetComponent<SkinnedMesh>())
        {
            std::vector<glm::mat4> palette;
            if (skinned->BuildPalette(palette))
            {
                const size_t count = std::min<size_t>(palette.size(), kMaxBonesPerObject);
                const size_t paletteOffset = skinSlot * kMaxBonesPerObject;
                std::memcpy(static_cast<glm::mat4*>(m_boneDataMapped) + paletteOffset,
                    palette.data(), count * sizeof(glm::mat4));
                skinJointCounts[skinSlot] = static_cast<uint32_t>(count);
            }
        }
    }
    m_boneDataBuffer->FlushMappedWrites();

    UINT slot = 0;
    for (Object* obj : renderObjects)
    {
        if (slot >= kMaxObjects)
            break;

        const bool belongsToPreview = m_previewObject &&
            IsObjectOrDescendant(obj, m_previewObject);
        Mesh* mesh = obj->GetComponent<Mesh>();
        Sprite* sprite = obj->GetComponent<Sprite>();

        Material* mat = obj->GetComponent<Material>();
        const bool isPreview = belongsToPreview;
        const BakedLightingData* bakedLighting =
            obj->GetComponent<BakedLightingData>();
        // Version 3 and later bake lighting into generated material assets.
        // Keep the component values for inspection, but do not add them again
        // at runtime or the baked result would be double-lit.
        const bool usesLegacyProbeBake = bakedLighting && bakedLighting->valid &&
            bakedLighting->version < 3;
        const glm::vec3 bakedIrradiance =
            usesLegacyProbeBake
                ? bakedLighting->irradiance
                : glm::vec3(0.f);
        glm::mat4 world = obj->transform.GetWorldMatrix();
        if (sprite)
        {
            if (m_editorMode2D)
                world[3].z = 0.f;
            const glm::vec2 size = sprite->GetWorldSize();
            world = world * glm::scale(glm::mat4(1.f), glm::vec3(size, 1.f));
        }
        UINT64 offset = static_cast<UINT64>(slot) * kCBStride;

        ObjectGPUData objectData{};
        objectData.mvp = proj * view * world;
        objectData.world = world;
        objectData.spriteUvRect = { 0.f, 0.f, 1.f, 1.f };
        if (skinJointCounts[slot] > 0)
        {
            const size_t paletteOffset = static_cast<size_t>(slot) * kMaxBonesPerObject;
            objectData.skinParams = { static_cast<float>(paletteOffset),
                static_cast<float>(skinJointCounts[slot]), 0.f, 0.f };
        }
        MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
        bool doubleSided = false;

        if (sprite)
        {
            const Texture* texture = sprite->GetTexture();
            const IGraphicsTexture* graphicsTexture = texture
                ? texture->GetGraphicsTexture() : nullptr;
            context->SetTexture(0, graphicsTexture);
            for (uint32_t textureSlot = 1; textureSlot < 6; ++textureSlot)
                context->SetTexture(textureSlot, nullptr);
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
            objectData.spriteUvRect = sprite->GetUvRect();
            alphaMode = MaterialAlphaMode::Blend;
            doubleSided = true;
        }
        else if (mat)
        {
            mat->Validate();
            alphaMode = mat->GetAlphaMode();
            doubleSided = mat->doubleSided;
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
            bindTexture(5, mat->heightTexture, 64u);
            if (alphaMode == MaterialAlphaMode::Mask)
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
        if (usesLegacyProbeBake)
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

        IPipelineState* materialPipeline = nullptr;
        if (isPreview)
            materialPipeline = doubleSided
                ? m_objectPreviewDoubleSidedPipeline.get()
                : m_objectPreviewPipeline.get();
        else if (alphaMode == MaterialAlphaMode::Blend)
            materialPipeline = doubleSided
                ? m_objectBlendDoubleSidedPipeline.get()
                : m_objectBlendPipeline.get();
        else
            materialPipeline = doubleSided
                ? m_objectDoubleSidedPipeline.get()
                : m_objectPipeline.get();
        context->SetPipeline(materialPipeline);
        context->SetConstantBuffer(0, m_objectConstantBuffer.get(), offset);
        context->SetStructuredBuffer(6, m_lightDataBuffer.get());
        context->SetStructuredBuffer(7, m_objectDataBuffer.get());
        context->SetStructuredBuffer(8, m_boneDataBuffer.get());

        // Set vertex buffer and draw
        IGraphicsBuffer* vertexBuffer = sprite
            ? sprite->GetGraphicsBuffer()
            : (mesh ? mesh->GetGraphicsBuffer() : nullptr);
        if (vertexBuffer)
        {
            const uint32_t vertexStride = sprite
                ? sprite->GetVertexStride() : mesh->GetVertexStride();
            const uint32_t vertexCount = sprite
                ? sprite->GetVertexCount() : mesh->GetVertexCount();
            context->SetVertexBuffer(0, vertexBuffer, vertexStride, 0);
            context->DrawInstanced(vertexCount, 1, 0, 0);

            // Draw selected object outline overlay.
            if (includeEditorVisuals && !isPreview &&
                obj == m_selectedObject && m_objectOutlinePipeline)
            {
                context->SetPipeline(m_objectOutlinePipeline.get());
                context->SetConstantBuffer(0, m_objectConstantBuffer.get(), offset);
                context->SetStructuredBuffer(6, m_lightDataBuffer.get());
                context->SetStructuredBuffer(7, m_objectDataBuffer.get());
                context->SetStructuredBuffer(8, m_boneDataBuffer.get());
                context->DrawInstanced(vertexCount, 1, 0, 0);
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
}
