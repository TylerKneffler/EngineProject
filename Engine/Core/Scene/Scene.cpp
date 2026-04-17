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
#include <DirectXMath.h>
#include <stdexcept>
#include <sstream>
#include <glm/glm.hpp>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Constant buffer data structures
// ---------------------------------------------------------------------------

// Constant buffer for object rendering (MVP matrix, material colors)
struct CBData
{
    DirectX::XMFLOAT4X4 mvp;
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4 diffuseColor;
    DirectX::XMFLOAT4 ambientColor;
    DirectX::XMFLOAT4 specularColor;
    DirectX::XMFLOAT4 padding;  // Align to 256 bytes
};

// Constant buffer for grid rendering
struct GridCBData
{
    DirectX::XMFLOAT4X4 invVP;
    DirectX::XMFLOAT3 cameraPos;
    float cellSize;
    DirectX::XMFLOAT4 gridColor;
    DirectX::XMFLOAT4 axisColor;
    float fadeDistance;
    DirectX::XMFLOAT3 padding;  // Align to 256 bytes
};

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

    // Compile shaders from disk
    // Assuming shaders are at standard paths (adjust as needed)
    auto vsShader = shaderCompiler->CompileFromFile(
        "Engine/Shaders/Grid.hlsl",
        "VSMain",
        IShaderCompiler::CompileProfile::VS_5_0);
    if (!vsShader)
        throw std::runtime_error("Failed to compile grid vertex shader: " + shaderCompiler->GetLastError());

    auto psShader = shaderCompiler->CompileFromFile(
        "Engine/Shaders/Grid.hlsl",
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
        .SetSrcBlend(5)                        // D3D12_BLEND_SRC_ALPHA
        .SetDestBlend(6)                       // D3D12_BLEND_INV_SRC_ALPHA
        .SetBlendOp(1)                         // D3D12_BLEND_OP_ADD
        .SetSrcBlendAlpha(1)                   // D3D12_BLEND_ONE
        .SetDestBlendAlpha(0)                  // D3D12_BLEND_ZERO
        .SetBlendOpAlpha(1)                    // D3D12_BLEND_OP_ADD
        .SetDepthEnable(true)
        .SetDepthWriteEnable(false)
        .SetDepthFunc(13)                      // D3D12_COMPARISON_FUNC_LESS_EQUAL
        .SetInputLayout(nullptr, 0)            // No vertex buffer
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 30)         // DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT
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
    auto vsShader = shaderCompiler->CompileFromFile(
        "Engine/Shaders/Object.hlsl",
        "VSMain",
        IShaderCompiler::CompileProfile::VS_5_0);
    if (!vsShader)
        throw std::runtime_error("Failed to compile object vertex shader: " + shaderCompiler->GetLastError());

    auto psShader = shaderCompiler->CompileFromFile(
        "Engine/Shaders/Object.hlsl",
        "PSMain",
        IShaderCompiler::CompileProfile::PS_5_0);
    if (!psShader)
        throw std::runtime_error("Failed to compile object pixel shader: " + shaderCompiler->GetLastError());

    // Define input layout: POSITION (float3) + NORMAL (float3)
    IPipelineStateBuilder::VertexElement layout[] =
    {
        { "POSITION", 0, 30, 0,  0, false },  // DXGI_FORMAT_R32G32B32_FLOAT = 30
        { "NORMAL",   0, 30, 0, 12, false },  // DXGI_FORMAT_R32G32B32_FLOAT = 30, offset 12
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
        .SetDepthFunc(4)                       // D3D12_COMPARISON_FUNC_LESS
        .SetInputLayout(layout, 2)             // 2 elements: POSITION, NORMAL
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 30)         // DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT
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
        return;

    if (!m_graphicsProvider || !m_gridPipeline || !m_objectPipeline)
        return;

    // Priority: explicit override > selected object's camera > editor camera
    Camera* cam = cameraOverride;
    if (!cam && m_selectedObject)
        cam = m_selectedObject->GetComponent<Camera>();
    if (!cam)
        cam = editorCamera.GetComponent<Camera>();
    if (!cam)
        return;

    XMMATRIX view = cam->GetViewMatrix();
    XMMATRIX proj = cam->GetProjectionMatrix(aspect);

    // Draw all scene objects that have a Mesh component
    UINT slot = 0;
    for (const auto& objPtr : m_objects)
    {
        if (slot >= kMaxObjects)
            break;

        Object* obj = objPtr.get();
        Mesh* mesh = obj->GetComponent<Mesh>();
        if (!mesh || !mesh->IsReady())
            continue;

        Material* mat = obj->GetComponent<Material>();
        glm::mat4 glmWorld = obj->transform.GetWorldMatrix();
        XMMATRIX worldMat = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(&glmWorld));
        UINT64 offset = static_cast<UINT64>(slot) * kCBStride;

        // Prepare constant buffer data
        CBData cbData{};
        XMStoreFloat4x4(&cbData.mvp, worldMat * view * proj);
        XMStoreFloat4x4(&cbData.world, worldMat);

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
        XMMATRIX vp = view * proj;
        const glm::vec3& cp = cam->Owner->transform.position;

        // Prepare grid constant buffer data
        GridCBData gridData{};
        XMStoreFloat4x4(&gridData.invVP, XMMatrixInverse(nullptr, vp));
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
