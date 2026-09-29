#include "pch.h"
#include "DX11PostProcess.h"
#include "Core/Graphics/PostProcess.h"
#include <d3dcompiler.h>

namespace Engine::Renderers
{
namespace
{
constexpr char ShaderSource[] = R"(
Texture2D<float4> sceneColor : register(t0);
SamplerState sceneSampler : register(s0);
cbuffer Params : register(b0) { float exposure; uint op; float2 padding; };
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID) { V o; o.uv=float2((id<<1)&2,id&2); o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
float3 linearToSrgb(float3 c) { c=max(c,0); return lerp(c*12.92,1.055*pow(c,1.0/2.4)-0.055,step(0.0031308,c)); }
float4 PSMain(V i) : SV_Target { float4 s=sceneColor.Sample(sceneSampler,i.uv); float3 c=max(s.rgb*max(exposure,0),0); if(op==1)c=saturate((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14)); else if(op==2)c=c/(1+c); return float4(linearToSrgb(c),s.a); }
)";

Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    ThrowIfFailed(D3DCompile(ShaderSource, sizeof(ShaderSource) - 1,
        "PostProcess", nullptr, nullptr, entry, target, 0, 0, &blob, &errors));
    return blob;
}
}

void DX11PostProcess::Create(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_sceneColor.Reset(); m_sceneRtv.Reset(); m_sceneSrv.Reset();
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = width; texture.Height = height; texture.MipLevels = 1;
    texture.ArraySize = 1; texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture.SampleDesc.Count = 1; texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device->CreateTexture2D(&texture, nullptr, &m_sceneColor));
    ThrowIfFailed(device->CreateRenderTargetView(m_sceneColor.Get(), nullptr, &m_sceneRtv));
    ThrowIfFailed(device->CreateShaderResourceView(m_sceneColor.Get(), nullptr, &m_sceneSrv));

    if (!m_vertexShader)
    {
        const auto vs = Compile("VSMain", "vs_5_0");
        const auto ps = Compile("PSMain", "ps_5_0");
        ThrowIfFailed(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vertexShader));
        ThrowIfFailed(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_pixelShader));
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        ThrowIfFailed(device->CreateSamplerState(&sampler, &m_sampler));
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 16; cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(device->CreateBuffer(&cb, nullptr, &m_constants));
        D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthEnable = FALSE; depth.StencilEnable = FALSE;
        ThrowIfFailed(device->CreateDepthStencilState(&depth, &m_depthDisabled));
    }
}

void DX11PostProcess::BindScene(ID3D11DeviceContext* context,
    ID3D11DepthStencilView* depth, const float clearColor[4])
{
    ID3D11ShaderResourceView* nullView = nullptr;
    context->PSSetShaderResources(0, 1, &nullView);
    ID3D11RenderTargetView* target = m_sceneRtv.Get();
    context->OMSetRenderTargets(1, &target, depth);
    context->ClearRenderTargetView(target, clearColor);
}

void DX11PostProcess::Compose(ID3D11DeviceContext* context,
    ID3D11RenderTargetView* output)
{
    context->OMSetRenderTargets(1, &output, nullptr);
    context->OMSetDepthStencilState(m_depthDisabled.Get(), 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    const auto settings = Engine::Graphics::GetPostProcessSettings();
    struct Constants { float exposure; uint32_t op; float padding[2]; } values{
        settings.exposure, settings.toneMapping, {} };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, &values, sizeof(values)); context->Unmap(m_constants.Get(), 0);
    ID3D11Buffer* cb = m_constants.Get(); ID3D11ShaderResourceView* srv = m_sceneSrv.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetConstantBuffers(0, 1, &cb); context->PSSetShaderResources(0, 1, &srv);
    context->PSSetSamplers(0, 1, &sampler); context->Draw(3, 0);
    ID3D11ShaderResourceView* nullView = nullptr; context->PSSetShaderResources(0, 1, &nullView);
}
}
