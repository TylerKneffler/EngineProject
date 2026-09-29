#include "pch.h"
#include "DX12PostProcess.h"
#include "Core/Graphics/PostProcess.h"
#include <d3dcompiler.h>

namespace Engine::Renderers
{
namespace
{
constexpr char ShaderSource[] = R"(
Texture2D<float4> sceneColor : register(t0); SamplerState sceneSampler : register(s0);
cbuffer Params : register(b0) { float exposure; uint op; float2 padding; };
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID) { V o; o.uv=float2((id<<1)&2,id&2); o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
float3 srgb(float3 c) { c=max(c,0); return lerp(c*12.92,1.055*pow(c,1.0/2.4)-0.055,step(0.0031308,c)); }
float4 PSMain(V i) : SV_Target { float4 s=sceneColor.Sample(sceneSampler,i.uv); float3 c=max(s.rgb*max(exposure,0),0); if(op==1)c=saturate((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14)); else if(op==2)c=c/(1+c); return float4(srgb(c),s.a); }
)";
Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    ThrowIfFailed(D3DCompile(ShaderSource, sizeof(ShaderSource) - 1,
        "PostProcess", nullptr, nullptr, entry, target, 0, 0, &blob, &errors));
    return blob;
}
}

void DX12PostProcess::Create(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_sceneColor.Reset();
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width; texture.Height = height; texture.DepthOrArraySize = 1;
    texture.MipLevels = 1; texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture.SampleDesc.Count = 1; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{}; clear.Format = texture.Format; clear.Color[3] = 1.f;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
        IID_PPV_ARGS(&m_sceneColor)));
    if (!m_rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtv{}; rtv.NumDescriptors = 1; rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&m_rtvHeap)));
        D3D12_DESCRIPTOR_HEAP_DESC srv{}; srv.NumDescriptors = 1; srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&m_srvHeap)));
    }
    device->CreateRenderTargetView(m_sceneColor.Get(), nullptr, m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = texture.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_sceneColor.Get(), &srv, m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    if (!m_rootSignature)
    {
        D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1; range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = 4;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters = 2; root.pParameters = parameters;
        root.NumStaticSamplers = 1; root.pStaticSamplers = &sampler;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> signature, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
        const auto vs = Compile("VSMain", "vs_5_0"), ps = Compile("PSMain", "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = m_rootSignature.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() }; pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX; pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthEnable = FALSE; pso.DepthStencilState.StencilEnable = FALSE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pso.SampleDesc.Count = 1;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipeline)));
    }
}

void DX12PostProcess::BindScene(ID3D12GraphicsCommandList* list,
    D3D12_CPU_DESCRIPTOR_HANDLE depth, const float clearColor[4])
{
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sceneColor.Get(); barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    auto target = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &target, FALSE, depth.ptr ? &depth : nullptr);
    list->ClearRenderTargetView(target, clearColor, 0, nullptr);
}

void DX12PostProcess::ClearScene(ID3D12GraphicsCommandList* list,
    const float clearColor[4])
{
    list->ClearRenderTargetView(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), clearColor, 0, nullptr);
}

void DX12PostProcess::Compose(ID3D12GraphicsCommandList* list,
    D3D12_CPU_DESCRIPTOR_HANDLE output)
{
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sceneColor.Get(); barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier); list->OMSetRenderTargets(1, &output, FALSE, nullptr);
    const auto settings = Engine::Graphics::GetPostProcessSettings();
    struct Constants { float exposure; uint32_t op; float padding[2]; } values{ settings.exposure, settings.toneMapping, {} };
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() }; list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(m_rootSignature.Get()); list->SetPipelineState(m_pipeline.Get());
    list->SetGraphicsRoot32BitConstants(0, 4, &values, 0);
    list->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->DrawInstanced(3, 1, 0, 0);
}
}
