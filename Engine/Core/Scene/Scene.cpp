#include "Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IShader.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include <stdexcept>
#include <sstream>
#include <glm/glm.hpp>

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

namespace
{
    std::string EngineShaderPath(const char* fileName)
    {
        const std::filesystem::path bundled =
            std::filesystem::path("Engine") / "Shaders" / fileName;
        if (std::filesystem::is_regular_file(bundled))
            return bundled.string();
        return (std::filesystem::path(ENGINE_SHADERS_PATH) / fileName).string();
    }
}

// ---------------------------------------------------------------------------
// Constant buffer data structures
// ---------------------------------------------------------------------------

// Constant buffer for object rendering (MVP matrix, material colors)
struct CBData
{
    glm::mat4 mvp;
    glm::vec4 diffuseColor;
    glm::vec4 ambientColor;
    glm::vec4 specularColor;
    glm::vec4 padding;  // Constant-buffer allocation uses a 256-byte stride.
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

static_assert(sizeof(CBData) == 128, "Object constant-buffer layout must match Object.hlsl");
static_assert(sizeof(GridCBData) == 128, "Grid constant-buffer layout must match Grid.hlsl");

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

    // Object constant buffer (256 * kMaxObjects bytes for per-object data)
    const uint64_t objectCBSize = static_cast<uint64_t>(kMaxObjects) * kCBStride;
    m_objectConstantBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ConstantBuffer,
        IGraphicsBuffer::AccessMode::Upload,
        objectCBSize);
    if (!m_objectConstantBuffer)
        throw std::runtime_error("Failed to create object constant buffer");
    m_objectCBMapped = m_objectConstantBuffer->Map();
    if (!m_objectCBMapped)
        throw std::runtime_error("Failed to map object constant buffer");

    // Set up the default editor camera
    editorCamera.AddComponent<Camera>();
    editorCamera.transform.position = { 0.f, 1.5f, -3.f };

    // Build pipeline states
    BuildGridPipeline();
    BuildObjectPipeline();
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
        .SetInputLayout(layout, 2)             // 2 elements: POSITION, NORMAL
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 40)         // DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT
        .Build();
    if (!m_objectPipeline)
        throw std::runtime_error("Failed to build object pipeline: " + builder->GetLastError());
}

// ---------------------------------------------------------------------------
// Scene::Render
// ---------------------------------------------------------------------------

Camera* Scene::FindGameCamera()
{
    for (const auto& obj : m_objects)
        if (Camera* cam = obj->GetComponent<Camera>())
            return cam;
    return editorCamera.GetComponent<Camera>();
}

void Scene::Render(IGraphicsContext* context, float aspect, Camera* cameraOverride)
{
    if (!context)
    {
        return;
    }

    if (!m_graphicsProvider || !m_gridPipeline || !m_objectPipeline)
    {
        return;
    }

    // Priority: explicit override > selected object's camera > editor camera
    Camera* cam = cameraOverride;
    if (!cam && m_selectedObject)
        cam = m_selectedObject->GetComponent<Camera>();
    if (!cam)
        cam = editorCamera.GetComponent<Camera>();
    if (!cam)
    {
        return;
    }

    const glm::mat4 view = cam->GetViewMatrix();
    const glm::mat4 proj = cam->GetProjectionMatrix(aspect);


    // Draw all scene objects that have a Mesh component
    UINT slot = 0;
    for (const auto& objPtr : m_objects)
    {
        if (slot >= kMaxObjects)
            break;

        Object* obj = objPtr.get();
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
        const glm::mat4 world = obj->transform.GetWorldMatrix();
        UINT64 offset = static_cast<UINT64>(slot) * kCBStride;

        // Prepare constant buffer data
        CBData cbData{};
        cbData.mvp = proj * view * world;

        if (mat)
        {
            cbData.diffuseColor = { mat->diffuseColor.r, mat->diffuseColor.g, mat->diffuseColor.b, 1.f };
            cbData.ambientColor = { mat->ambientColor.r, mat->ambientColor.g, mat->ambientColor.b, 1.f };
            cbData.specularColor = { mat->specularColor.r, mat->specularColor.g, mat->specularColor.b, mat->shininess };
        }
        else
        {
            cbData.diffuseColor = { 0.8f, 0.8f, 0.8f, 1.f };
            cbData.ambientColor = { 0.1f, 0.1f, 0.1f, 1.f };
            cbData.specularColor = { 1.f, 1.f, 1.f, 32.f };
        }

        // Write to constant buffer
        memcpy(static_cast<uint8_t*>(m_objectCBMapped) + offset, &cbData, sizeof(cbData));

        // Set pipeline and constant buffer
        context->SetPipeline(m_objectPipeline.get());
        context->SetConstantBuffer(0, m_objectConstantBuffer.get(), offset);

        // Set vertex buffer and draw
        IGraphicsBuffer* vertexBuffer = mesh->GetGraphicsBuffer();
        if (vertexBuffer)
        {
            context->SetVertexBuffer(0, vertexBuffer, mesh->GetVertexStride(), 0);
            context->DrawInstanced(mesh->GetVertexCount(), 1, 0, 0);
        }

        ++slot;
    }

    // Draw scene helpers (grid) after opaque objects so blending works correctly
    if (settings.showGrid && m_gridPipeline)
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

void Scene::RemoveObject(Object* obj)
{
    m_objects.erase(
        std::remove_if(m_objects.begin(), m_objects.end(),
            [obj](const std::unique_ptr<Object>& p) { return p.get() == obj; }),
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
