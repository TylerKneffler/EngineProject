#include "Scene.h"
#include "Core/Compoonents/Camera.h"
#include <d3dcompiler.h>
#include <stdexcept>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Grid HLSL — minimal vertex-colored, alpha-blended line shader.
// cbuffer b0: float4x4 mvp only.
// ---------------------------------------------------------------------------

static constexpr char kGridVS[] = R"(
cbuffer CB : register(b0) { float4x4 mvp; };
void VSMain(float3 pos   : POSITION,
            float4 color : COLOR,
            out float4 oPos   : SV_POSITION,
            out float4 oColor : COLOR)
{
    oPos   = mul(mvp, float4(pos, 1.0));
    oColor = color;
}
)";

static constexpr char kGridPS[] = R"(
float4 PSMain(float4 pos   : SV_POSITION,
              float4 color : COLOR) : SV_TARGET
{
    return color;
}
)";

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
    BuildGridBuffers(device);
    BuildGridPipeline(device);

    // Constant buffer for the grid (just MVP, aligned to 256 B).
    constexpr UINT kCBSize = 256;
    m_gridCB = CreateUploadBuffer(device, kCBSize, &m_gridCBMapped);

    // Set up the default editor camera.
    // Position is owned by the Transform; target defaults to the origin.
    editorCamera.AddComponent<Camera>();
    editorCamera.transform.position = { 0.f, 1.5f, -3.f };
}

// ---------------------------------------------------------------------------
// Scene::BuildGridBuffers
// Generates line-list vertices for the XZ grid.
// ---------------------------------------------------------------------------

void Scene::BuildGridBuffers(ID3D12Device* device)
{
    const int   half = settings.gridHalfSize;
    const float step = settings.gridCellSize;
    const float ext  = half * step;

    auto gc = settings.gridColor;
    auto oc = settings.gridOriginColor;
    float op = settings.gridOpacity;

    std::vector<GridVertex> verts;
    // lines parallel to Z (vary X)
    for (int i = -half; i <= half; ++i)
    {
        float x = i * step;
        bool  origin = (i == 0);
        float r = origin ? oc.x : gc.x;
        float g = origin ? oc.y : gc.y;
        float b = origin ? oc.z : gc.z;
        verts.push_back({ x, 0.f, -ext, r, g, b, op });
        verts.push_back({ x, 0.f,  ext, r, g, b, op });
    }
    // lines parallel to X (vary Z)
    for (int i = -half; i <= half; ++i)
    {
        float z = i * step;
        bool  origin = (i == 0);
        float r = origin ? oc.x : gc.x;
        float g = origin ? oc.y : gc.y;
        float b = origin ? oc.z : gc.z;
        verts.push_back({ -ext, 0.f, z, r, g, b, op });
        verts.push_back({  ext, 0.f, z, r, g, b, op });
    }

    m_gridVertexCount = static_cast<uint32_t>(verts.size());
    const UINT byteSize = m_gridVertexCount * sizeof(GridVertex);

    m_gridVB = CreateUploadBuffer(device, byteSize, nullptr);
    void* mapped = nullptr;
    m_gridVB->Map(0, nullptr, &mapped);
    memcpy(mapped, verts.data(), byteSize);
    m_gridVB->Unmap(0, nullptr);

    m_gridVBV.BufferLocation = m_gridVB->GetGPUVirtualAddress();
    m_gridVBV.SizeInBytes    = byteSize;
    m_gridVBV.StrideInBytes  = sizeof(GridVertex);
}

// ---------------------------------------------------------------------------
// Scene::BuildGridPipeline
// ---------------------------------------------------------------------------

void Scene::BuildGridPipeline(ID3D12Device* device)
{
    // Compile shaders.
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    if (FAILED(D3DCompile(kGridVS, sizeof(kGridVS) - 1, "GridVS",
            nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob)))
        throw std::runtime_error(static_cast<char*>(errBlob->GetBufferPointer()));
    if (FAILED(D3DCompile(kGridPS, sizeof(kGridPS) - 1, "GridPS",
            nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob)))
        throw std::runtime_error(static_cast<char*>(errBlob->GetBufferPointer()));

    // Root signature: one root CBV at b0.
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters   = &rp;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, sigErr;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr);
    device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
        sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_gridRootSig));

    // Input layout: POSITION (float3) + COLOR (float4).
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

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

    // Depth — read but do not write so objects render correctly on top.
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable    = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_gridRootSig.Get();
    psoDesc.VS                    = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS                    = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout           = { layout, _countof(layout) };
    psoDesc.RasterizerState       = rasterDesc;
    psoDesc.BlendState            = blendDesc;
    psoDesc.DepthStencilState     = dsDesc;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { 1, 0 };
    psoDesc.SampleMask            = UINT_MAX;

    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_gridPSO));
}

// ---------------------------------------------------------------------------
// Scene::Render
// ---------------------------------------------------------------------------

void Scene::Render(ID3D12GraphicsCommandList* cmd,
                   const XMMATRIX& view,
                   const XMMATRIX& proj)
{
    // --- Render grid (after opaque objects so blending works correctly) ---
    if (settings.showGrid && m_gridPSO)
    {
        XMFLOAT4X4 mvp;
        XMStoreFloat4x4(&mvp, view * proj);
        memcpy(m_gridCBMapped, &mvp, sizeof(XMFLOAT4X4));

        cmd->SetGraphicsRootSignature(m_gridRootSig.Get());
        cmd->SetPipelineState(m_gridPSO.Get());
        cmd->SetGraphicsRootConstantBufferView(0, m_gridCB->GetGPUVirtualAddress());
        cmd->IASetVertexBuffers(0, 1, &m_gridVBV);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        cmd->DrawInstanced(m_gridVertexCount, 1, 0, 0);
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
