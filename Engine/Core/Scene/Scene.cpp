#include "Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Serialization/SceneSerializer.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>
#include <fstream>
#include <shellapi.h>
#include <windows.h>  // OutputDebugStringA, ShellExecuteA

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Shader loading helper
// ---------------------------------------------------------------------------

static ComPtr<ID3DBlob> CompileShaderFromFile(
    const std::string& path, const char* entry, const char* profile)
{
    std::wstring wpath(path.begin(), path.end());
    ComPtr<ID3DBlob> blob, errBlob;
    HRESULT hr = D3DCompileFromFile(
        wpath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, profile, 0, 0, &blob, &errBlob);
    if (FAILED(hr))
    {
        std::string msg = "[Shader] Failed to compile " + path + " (" + entry + ")\n";
        if (errBlob)
            msg += static_cast<char*>(errBlob->GetBufferPointer());
        else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            msg += "File not found.";
        else
        {
            char buf[32]; sprintf_s(buf, "HRESULT 0x%08X", static_cast<unsigned>(hr));
            msg += buf;
        }
        OutputDebugStringA((msg + "\n").c_str());

        // Write to shader_errors.log beside the exe so the full message is
        // copyable, then open it in the default text editor.
        /*{
            char exePath[MAX_PATH]{};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            std::string logPath(exePath);
            auto slash = logPath.find_last_of("\\/");
            if (slash != std::string::npos) logPath = logPath.substr(0, slash + 1);
            logPath += "shader_errors.log";
            std::ofstream log(logPath, std::ios::app);
            log << msg << "\n\n";
            ShellExecuteA(nullptr, "open", logPath.c_str(), nullptr, nullptr, SW_SHOW);
        }*/

        throw std::runtime_error(msg);
    }
    return blob;
}

// GridCBData — matches the GridCB cbuffer layout in the HLSL above.
struct GridCBData
{
    XMFLOAT4X4 invVP;         // 64 bytes — inverse of (view*proj), row-major
    XMFLOAT3   cameraPos;     // 12 bytes
    float      cellSize;      //  4 bytes
    XMFLOAT4   gridColor;     // 16 bytes  (rgb + opacity)
    XMFLOAT4   axisColor;     // 16 bytes  (rgb + 1)
    float      fadeDistance;  //  4 bytes
    float      _pad[3];       // 12 bytes  — total: 128 bytes
};
static_assert(sizeof(GridCBData) <= 256, "GridCBData exceeds 256-byte CB alignment");

struct CBData
{
    DirectX::XMFLOAT4X4 mvp;
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4   diffuseColor;
    DirectX::XMFLOAT4   ambientColor;
    DirectX::XMFLOAT4   specularColor; // xyz = color, w = shininess
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static ComPtr<ID3D12Resource> CreateUploadBuffer(
    ID3D12Device* device, UINT64 byteSize, void** mappedOut)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC   rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = byteSize;
    rd.Height           = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> buf;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
    if (mappedOut)
        buf->Map(0, nullptr, mappedOut);
    return buf;
}

// ---------------------------------------------------------------------------
// Scene::Init
// ---------------------------------------------------------------------------

void Scene::Init(ID3D12Device* device)
{
    m_device = device;
    BuildGridPipeline(device);

    // Constant buffer for per-frame grid data (256 bytes).
    constexpr UINT kCBSize = 256;
    m_gridCB = CreateUploadBuffer(device, kCBSize, &m_gridCBMapped);

    // Set up the default editor camera.
    // Position is owned by the Transform; target defaults to the origin.
    editorCamera.AddComponent<Camera>();
    editorCamera.transform.position = { 0.f, 1.5f, -3.f };

    BuildObjectPipeline(device);
}

// ---------------------------------------------------------------------------
// Scene::BuildGridPipeline
// ---------------------------------------------------------------------------

void Scene::BuildGridPipeline(ID3D12Device* device)
{
    // Compile shaders from disk.
    auto vsBlob = CompileShaderFromFile(ENGINE_SHADERS_PATH "Grid.hlsl", "VSMain", "vs_5_0");
    auto psBlob = CompileShaderFromFile(ENGINE_SHADERS_PATH "Grid.hlsl", "PSMain", "ps_5_0");

    // Root signature: one root CBV at b0, visible to all shader stages.
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters   = &rp;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE; // no IA, no vertex buffer

    ComPtr<ID3DBlob> sigBlob, sigErr;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
        sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_gridRootSig));

    // No input layout — vertices are generated from SV_VertexID in the VS.

    // Rasterizer — no culling for lines.
    D3D12_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode  = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode  = D3D12_CULL_MODE_NONE;
    rasterDesc.DepthClipEnable = TRUE;

    // Alpha blend so the grid is semi-transparent.
    D3D12_RENDER_TARGET_BLEND_DESC rtb{};
    rtb.BlendEnable           = TRUE;
    rtb.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    rtb.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rtb.BlendOp               = D3D12_BLEND_OP_ADD;
    rtb.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtb.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtb.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0] = rtb;

    // Depth — test at the far plane so opaque objects (depth < 1.0) occlude the
    // grid, while empty background (cleared to 1.0) lets it through.
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable    = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_gridRootSig.Get();
    psoDesc.VS                    = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS                    = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout           = { nullptr, 0 };  // no vertex buffer
    psoDesc.RasterizerState       = rasterDesc;
    psoDesc.BlendState            = blendDesc;
    psoDesc.DepthStencilState     = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { 1, 0 };
    psoDesc.SampleMask            = UINT_MAX;

    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_gridPSO));
}

// ---------------------------------------------------------------------------
// Scene::BuildObjectPipeline
// ---------------------------------------------------------------------------

void Scene::BuildObjectPipeline(ID3D12Device* device)
{
    // Compile shaders from disk.
    auto vsBlob = CompileShaderFromFile(ENGINE_SHADERS_PATH "Object.hlsl", "VSMain", "vs_5_0");
    auto psBlob = CompileShaderFromFile(ENGINE_SHADERS_PATH "Object.hlsl", "PSMain", "ps_5_0");

    // Root signature: one root CBV at b0, visible to all stages.
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters   = &rp;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, sigErr;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
        sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_objectRootSig));

    // Input layout: POSITION (float3) + NORMAL (float3).
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode              = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode              = D3D12_CULL_MODE_BACK;
    rasterDesc.FrontCounterClockwise = TRUE;
    rasterDesc.DepthClipEnable       = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC rtb{};
    rtb.BlendEnable           = FALSE;
    rtb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0] = rtb;

    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable    = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_objectRootSig.Get();
    psoDesc.VS                    = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS                    = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout           = { layout, _countof(layout) };
    psoDesc.RasterizerState       = rasterDesc;
    psoDesc.BlendState            = blendDesc;
    psoDesc.DepthStencilState     = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { 1, 0 };
    psoDesc.SampleMask            = UINT_MAX;
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_objectPSO));

    // Constant buffer: kMaxObjects slots, each kCBStride bytes apart.
    constexpr UINT64 kTotalCBSize = static_cast<UINT64>(kMaxObjects) * kCBStride;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = kTotalCBSize;
    rd.Height           = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_objectCB));
    m_objectCB->Map(0, nullptr, &m_objectCBMapped);
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

void Scene::Render(ID3D12GraphicsCommandList* cmd, float aspect, Camera* cameraOverride)
{
    // Priority: explicit override > selected object's camera > editor camera.
    Camera* cam = cameraOverride;
    if (!cam && m_selectedObject)
        cam = m_selectedObject->GetComponent<Camera>();
    if (!cam)
        cam = editorCamera.GetComponent<Camera>();
    if (!cam) return;

    XMMATRIX view = cam->GetViewMatrix();
    XMMATRIX proj = cam->GetProjectionMatrix(aspect);

    // Draw all scene objects that have a Mesh component.
    UINT slot = 0;
    for (const auto& objPtr : m_objects)
    {
        if (slot >= kMaxObjects) break;
        Object* obj  = objPtr.get();
        Mesh*   mesh = obj->GetComponent<Mesh>();
        if (!mesh || !mesh->IsReady()) continue;

        Material* mat      = obj->GetComponent<Material>();
        glm::mat4 glmWorld = obj->transform.GetWorldMatrix();
        XMMATRIX  worldMat = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(&glmWorld));
        UINT64    offset   = static_cast<UINT64>(slot) * kCBStride;

        CBData cbData{};
        XMStoreFloat4x4(&cbData.mvp,   worldMat * view * proj);
        XMStoreFloat4x4(&cbData.world, worldMat);
        if (mat)
        {
            cbData.diffuseColor  = { mat->diffuseColor.r,  mat->diffuseColor.g,  mat->diffuseColor.b,  1.f };
            cbData.ambientColor  = { mat->ambientColor.r,  mat->ambientColor.g,  mat->ambientColor.b,  1.f };
            cbData.specularColor = { mat->specularColor.r, mat->specularColor.g, mat->specularColor.b, mat->shininess };
        }
        else
        {
            cbData.diffuseColor  = { 0.8f, 0.8f, 0.8f, 1.f };
            cbData.ambientColor  = { 0.1f, 0.1f, 0.1f, 1.f };
            cbData.specularColor = { 1.f,  1.f,  1.f,  32.f };
        }
        memcpy(static_cast<uint8_t*>(m_objectCBMapped) + offset, &cbData, sizeof(cbData));

        cmd->SetGraphicsRootSignature(m_objectRootSig.Get());
        cmd->SetPipelineState(m_objectPSO.Get());
        cmd->SetGraphicsRootConstantBufferView(0, m_objectCB->GetGPUVirtualAddress() + offset);
        D3D12_VERTEX_BUFFER_VIEW vbv = mesh->GetVertexBufferView();
        cmd->IASetVertexBuffers(0, 1, &vbv);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(mesh->GetVertexCount(), 1, 0, 0);

        ++slot;
    }

    // Draw scene helpers (grid) after opaque objects so blending works correctly.
    if (settings.showGrid && m_gridPSO)
    {
        XMMATRIX vp = view * proj;

        const glm::vec3& cp = cam->Owner->transform.position;
        GridCBData gridData{};
        XMStoreFloat4x4(&gridData.invVP, XMMatrixInverse(nullptr, vp));
        gridData.cameraPos    = { cp.x, cp.y, cp.z };
        gridData.cellSize     = settings.gridCellSize;
        gridData.gridColor    = { settings.gridColor.x, settings.gridColor.y,
                                   settings.gridColor.z, settings.gridOpacity };
        gridData.axisColor    = { settings.gridOriginColor.x, settings.gridOriginColor.y,
                                   settings.gridOriginColor.z, 1.f };
        gridData.fadeDistance = settings.gridFadeDistance;
        memcpy(m_gridCBMapped, &gridData, sizeof(GridCBData));

        cmd->SetGraphicsRootSignature(m_gridRootSig.Get());
        cmd->SetPipelineState(m_gridPSO.Get());
        cmd->SetGraphicsRootConstantBufferView(0, m_gridCB->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);  // fullscreen triangle, no vertex buffer
    }
}

// ---------------------------------------------------------------------------
// Scene::Object management
// ---------------------------------------------------------------------------

Object* Scene::AddObject()
{
    auto obj = std::make_unique<Object>();
    Object* raw = obj.get();
    m_objects.push_back(std::move(obj));
    return raw;
}

Object* Scene::AddObject(const std::string& name)
{
    Object* obj = AddObject();
    obj->name = name;
    return obj;
}

void Scene::RemoveObject(Object* obj)
{
    m_objects.erase(
        std::remove_if(m_objects.begin(), m_objects.end(),
            [obj](const std::unique_ptr<Object>& p){ return p.get() == obj; }),
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
    return SceneSerializer::Load(*this, path, m_device);
}
