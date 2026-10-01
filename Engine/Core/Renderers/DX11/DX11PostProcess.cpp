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
float3 linearToPq(float3 c) { const float m1=2610.0/16384.0, m2=2523.0/32.0; const float c1=3424.0/4096.0, c2=2413.0/128.0, c3=2392.0/128.0; float3 l=pow(saturate(c*0.1),m1); return pow((c1+c2*l)/(1+c3*l),m2); }
float4 PSMain(V i) : SV_Target { float4 s=sceneColor.Sample(sceneSampler,i.uv); if(op==4)return s*padding.x; float3 c=max(s.rgb*max(exposure,0),0); if(op==5)return float4(c,s.a); if(op==3) { c=mul(float3x3(0.6274,0.3293,0.0433,0.0691,0.9195,0.0114,0.0164,0.0880,0.8956),c); return float4(linearToPq(c),1); } if(op==1)c=saturate((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14)); else if(op==2)c=c/(1+c); return float4(linearToSrgb(c),s.a); }
)";

Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    ThrowIfFailed(D3DCompile(ShaderSource, sizeof(ShaderSource) - 1,
        "PostProcess", nullptr, nullptr, entry, target, 0, 0, &blob, &errors));
    return blob;
}
}

void DX11PostProcess::Create(ID3D11Device* device, uint32_t width, uint32_t height,
    uint32_t sampleCount, uint32_t hdrBits)
{
    m_sceneColor.Reset(); m_sceneRtv.Reset(); m_sceneSrv.Reset();
    m_resolvedColor.Reset(); m_resolvedRtv.Reset();
    m_accumulationColor.Reset(); m_accumulationRtv.Reset(); m_accumulationSrv.Reset();
    m_width = width; m_height = height;
    m_sampleCount = std::max(1u, sampleCount);
    m_sceneFormat = hdrBits >= 32u ? DXGI_FORMAT_R32G32B32A32_FLOAT
        : DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = width; texture.Height = height; texture.MipLevels = 1;
    texture.ArraySize = 1; texture.Format = m_sceneFormat;
    texture.SampleDesc.Count = m_sampleCount; texture.Usage = D3D11_USAGE_DEFAULT;
    // The single-sample scene target is sampled directly by the compose pass.
    // Multisampled targets are resolved into a separate shader-readable image.
    texture.BindFlags = D3D11_BIND_RENDER_TARGET |
        (m_sampleCount == 1u ? D3D11_BIND_SHADER_RESOURCE : 0u);
    ThrowIfFailed(device->CreateTexture2D(&texture, nullptr, &m_sceneColor));
    ThrowIfFailed(device->CreateRenderTargetView(m_sceneColor.Get(), nullptr, &m_sceneRtv));
    if (m_sampleCount == 1u)
    {
        ThrowIfFailed(device->CreateShaderResourceView(m_sceneColor.Get(), nullptr, &m_sceneSrv));
    }
    else
    {
        texture.SampleDesc.Count = 1;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(device->CreateTexture2D(&texture, nullptr, &m_resolvedColor));
        ThrowIfFailed(device->CreateRenderTargetView(m_resolvedColor.Get(), nullptr, &m_resolvedRtv));
        ThrowIfFailed(device->CreateShaderResourceView(m_resolvedColor.Get(), nullptr, &m_sceneSrv));
    }
    D3D11_TEXTURE2D_DESC accumulation = texture;
    accumulation.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    accumulation.SampleDesc.Count = 1;
    accumulation.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device->CreateTexture2D(&accumulation, nullptr, &m_accumulationColor));
    ThrowIfFailed(device->CreateRenderTargetView(m_accumulationColor.Get(), nullptr,
        &m_accumulationRtv));
    ThrowIfFailed(device->CreateShaderResourceView(m_accumulationColor.Get(), nullptr,
        &m_accumulationSrv));

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
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ThrowIfFailed(device->CreateBlendState(&blend, &m_additiveBlend));
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

void DX11PostProcess::ClearAccumulation(ID3D11DeviceContext* context)
{
    const float clear[4] = {};
    context->ClearRenderTargetView(m_accumulationRtv.Get(), clear);
}

void DX11PostProcess::AccumulateScene(ID3D11DeviceContext* context,
    float sampleWeight)
{
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (m_sampleCount > 1u)
        context->ResolveSubresource(m_resolvedColor.Get(), 0, m_sceneColor.Get(), 0,
            m_sceneFormat);
    ID3D11RenderTargetView* target = m_accumulationRtv.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetDepthStencilState(m_depthDisabled.Get(), 0);
    const float blendFactor[4] = {};
    context->OMSetBlendState(m_additiveBlend.Get(), blendFactor, 0xffffffffu);
    D3D11_VIEWPORT viewport{ 0.f, 0.f, static_cast<float>(m_width),
        static_cast<float>(m_height), 0.f, 1.f };
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    struct Constants { float exposure; uint32_t op; float padding[2]; } values{
        1.f, 4u, { sampleWeight, 0.f } };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD,
        0, &mapped));
    memcpy(mapped.pData, &values, sizeof(values));
    context->Unmap(m_constants.Get(), 0);
    ID3D11Buffer* cb = m_constants.Get();
    ID3D11ShaderResourceView* srv = m_sceneSrv.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetConstantBuffers(0, 1, &cb);
    context->PSSetShaderResources(0, 1, &srv);
    context->PSSetSamplers(0, 1, &sampler);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* nullView = nullptr;
    context->PSSetShaderResources(0, 1, &nullView);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
}

void DX11PostProcess::Compose(ID3D11DeviceContext* context,
    ID3D11RenderTargetView* output, bool useAccumulation)
{
    if (m_sampleCount > 1u)
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->ResolveSubresource(m_resolvedColor.Get(), 0, m_sceneColor.Get(), 0,
            m_sceneFormat);
    }
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
    ID3D11Buffer* cb = m_constants.Get(); ID3D11ShaderResourceView* srv =
        useAccumulation ? m_accumulationSrv.Get() : m_sceneSrv.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetConstantBuffers(0, 1, &cb); context->PSSetShaderResources(0, 1, &srv);
    context->PSSetSamplers(0, 1, &sampler); context->Draw(3, 0);
    ID3D11ShaderResourceView* nullView = nullptr; context->PSSetShaderResources(0, 1, &nullView);
}
}
