#include "pch.h"
#include "DX11PostProcess.h"
#include "Core/Graphics/PostProcess.h"
#include <d3dcompiler.h>
#include <stdexcept>

namespace Engine::Renderers
{
namespace
{
Microsoft::WRL::ComPtr<ID3DBlob> Compile(const std::string& source,
    const char* entry, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const HRESULT result = D3DCompile(source.data(), source.size(),
        "PostProcess.hlsl", nullptr, nullptr, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(result))
        throw std::runtime_error(std::string("Post-process shader ") + entry + ": " +
            (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed"));
    return blob;
}
}

void DX11PostProcess::Create(ID3D11Device* device, uint32_t width, uint32_t height,
    uint32_t sampleCount, uint32_t hdrBits)
{
    m_sceneColor.Reset(); m_sceneRtv.Reset(); m_sceneSrv.Reset();
    m_resolvedColor.Reset(); m_resolvedRtv.Reset();
    m_accumulationColor.Reset(); m_accumulationRtv.Reset(); m_accumulationSrv.Reset();
    m_bloom.Reset(); m_bloomRtvs.clear(); m_bloomSrvs.clear();
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

    // Bloom chain: half resolution, one render target and view per level.
    m_bloomLevels = Engine::Graphics::BloomLevelCount(width, height);
    D3D11_TEXTURE2D_DESC bloom{};
    bloom.Width = Engine::Graphics::BloomLevelExtent(width, 0);
    bloom.Height = Engine::Graphics::BloomLevelExtent(height, 0);
    bloom.MipLevels = m_bloomLevels; bloom.ArraySize = 1;
    bloom.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; bloom.SampleDesc.Count = 1;
    bloom.Usage = D3D11_USAGE_DEFAULT;
    bloom.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device->CreateTexture2D(&bloom, nullptr, &m_bloom));
    m_bloomRtvs.resize(m_bloomLevels);
    m_bloomSrvs.resize(m_bloomLevels);
    for (uint32_t level = 0; level < m_bloomLevels; ++level)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = bloom.Format; rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtv.Texture2D.MipSlice = level;
        ThrowIfFailed(device->CreateRenderTargetView(m_bloom.Get(), &rtv, &m_bloomRtvs[level]));
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = bloom.Format; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = level; srv.Texture2D.MipLevels = 1;
        ThrowIfFailed(device->CreateShaderResourceView(m_bloom.Get(), &srv, &m_bloomSrvs[level]));
    }

    if (!m_vertexShader)
    {
        const std::string source = Engine::Graphics::LoadPostProcessShaderSource();
        const auto vs = Compile(source, "VSMain", "vs_5_0");
        const auto ps = Compile(source, "PSMain", "ps_5_0");
        const auto down = Compile(source, "BloomDownsamplePS", "ps_5_0");
        const auto up = Compile(source, "BloomUpsamplePS", "ps_5_0");
        ThrowIfFailed(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vertexShader));
        ThrowIfFailed(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_pixelShader));
        ThrowIfFailed(device->CreatePixelShader(down->GetBufferPointer(), down->GetBufferSize(), nullptr, &m_bloomDownsampleShader));
        ThrowIfFailed(device->CreatePixelShader(up->GetBufferPointer(), up->GetBufferSize(), nullptr, &m_bloomUpsampleShader));
        const auto luminance = Compile(source, "ExposureLuminancePS", "ps_5_0");
        const auto histogram = Compile(source, "ExposureHistogramPS", "ps_5_0");
        const auto adapt = Compile(source, "ExposureAdaptPS", "ps_5_0");
        ThrowIfFailed(device->CreatePixelShader(luminance->GetBufferPointer(), luminance->GetBufferSize(), nullptr, &m_exposureLuminanceShader));
        ThrowIfFailed(device->CreatePixelShader(histogram->GetBufferPointer(), histogram->GetBufferSize(), nullptr, &m_exposureHistogramShader));
        ThrowIfFailed(device->CreatePixelShader(adapt->GetBufferPointer(), adapt->GetBufferSize(), nullptr, &m_exposureAdaptShader));
        CreateExposureTargets(device);
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        ThrowIfFailed(device->CreateSamplerState(&sampler, &m_sampler));
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Engine::Graphics::PostProcessConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
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

void DX11PostProcess::CreateExposureTargets(ID3D11Device* device)
{
    using namespace Engine::Graphics;
    const auto create = [&](uint32_t width, uint32_t height, DXGI_FORMAT format,
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, &texture));
        ThrowIfFailed(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
        ThrowIfFailed(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
    };
    create(ExposureMapWidth, ExposureMapHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
        m_exposureMapRtv, m_exposureMapSrv);
    create(ExposureBins, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
        m_exposureHistogramRtv, m_exposureHistogramSrv);
    // Full float: per-frame adaptation steps are far below half precision.
    for (uint32_t index = 0; index < 2; ++index)
        create(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, m_exposureRtvs[index], m_exposureSrvs[index]);
    m_exposure.Reset();
}

ID3D11ShaderResourceView* DX11PostProcess::RenderExposure(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* source, const Engine::Graphics::PostProcessSettings& settings)
{
    using namespace Engine::Graphics;
    UINT savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&savedViewportCount, savedViewports);
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> savedRasterizer;
    context->RSGetState(&savedRasterizer);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);

    const ExposureFrame frame = m_exposure.Next();
    const auto values = MakeExposureConstants(settings, frame.deltaSeconds, frame.history);
    UploadConstants(context, &values);
    ID3D11Buffer* cb = m_constants.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetConstantBuffers(0, 1, &cb);
    context->PSSetSamplers(0, 1, &sampler);
    const auto pass = [&](ID3D11PixelShader* shader, ID3D11RenderTargetView* target,
        uint32_t width, uint32_t height, ID3D11ShaderResourceView* input0,
        ID3D11ShaderResourceView* input1)
    {
        ID3D11ShaderResourceView* nullViews[2] = {};
        context->PSSetShaderResources(0, 2, nullViews);
        context->OMSetRenderTargets(1, &target, nullptr);
        const D3D11_VIEWPORT viewport{ 0.f, 0.f, float(width), float(height), 0.f, 1.f };
        context->RSSetViewports(1, &viewport);
        context->PSSetShader(shader, nullptr, 0);
        ID3D11ShaderResourceView* inputs[2] = { input0, input1 };
        context->PSSetShaderResources(0, 2, inputs);
        context->Draw(3, 0);
    };
    pass(m_exposureLuminanceShader.Get(), m_exposureMapRtv.Get(),
        ExposureMapWidth, ExposureMapHeight, source, nullptr);
    pass(m_exposureHistogramShader.Get(), m_exposureHistogramRtv.Get(),
        ExposureBins, 1, m_exposureMapSrv.Get(), nullptr);
    pass(m_exposureAdaptShader.Get(), m_exposureRtvs[frame.target].Get(), 1, 1,
        m_exposureHistogramSrv.Get(), m_exposureSrvs[1u - frame.target].Get());

    ID3D11ShaderResourceView* nullViews[2] = {};
    context->PSSetShaderResources(0, 2, nullViews);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->RSSetState(savedRasterizer.Get());
    if (savedViewportCount > 0)
        context->RSSetViewports(savedViewportCount, savedViewports);
    return m_exposureSrvs[frame.target].Get();
}

void DX11PostProcess::UploadConstants(ID3D11DeviceContext* context, const void* data)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, data, sizeof(Engine::Graphics::PostProcessConstants));
    context->Unmap(m_constants.Get(), 0);
}

void DX11PostProcess::BindScene(ID3D11DeviceContext* context,
    ID3D11DepthStencilView* depth, const float clearColor[4])
{
    ID3D11ShaderResourceView* nullViews[2] = {};
    context->PSSetShaderResources(0, 2, nullViews);
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
    Engine::Graphics::PostProcessSettings accumulate{};
    accumulate.toneMapping = 4u;
    const auto values = Engine::Graphics::MakeComposeConstants(accumulate, 1u, sampleWeight);
    UploadConstants(context, &values);
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

void DX11PostProcess::RenderBloom(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* source)
{
    // Bloom passes change viewport and rasterizer state; restore both so the
    // caller's compose viewport is unaffected.
    UINT savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&savedViewportCount, savedViewports);
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> savedRasterizer;
    context->RSGetState(&savedRasterizer);
    context->RSSetState(nullptr);

    ID3D11Buffer* cb = m_constants.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetConstantBuffers(0, 1, &cb);
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* nullView = nullptr;
    const uint32_t bloomWidth = Engine::Graphics::BloomLevelExtent(m_width, 0);
    const uint32_t bloomHeight = Engine::Graphics::BloomLevelExtent(m_height, 0);
    const auto drawLevel = [&](uint32_t target, ID3D11ShaderResourceView* input,
        uint32_t inputWidth, uint32_t inputHeight)
    {
        context->PSSetShaderResources(0, 1, &nullView);
        ID3D11RenderTargetView* rtv = m_bloomRtvs[target].Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        const D3D11_VIEWPORT viewport{ 0.f, 0.f,
            static_cast<float>(std::max(1u, bloomWidth >> target)),
            static_cast<float>(std::max(1u, bloomHeight >> target)), 0.f, 1.f };
        context->RSSetViewports(1, &viewport);
        const auto values = Engine::Graphics::MakeBloomPassConstants(inputWidth, inputHeight);
        UploadConstants(context, &values);
        context->PSSetShaderResources(0, 1, &input);
        context->Draw(3, 0);
    };

    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context->PSSetShader(m_bloomDownsampleShader.Get(), nullptr, 0);
    drawLevel(0, source, m_width, m_height);
    for (uint32_t level = 1; level < m_bloomLevels; ++level)
        drawLevel(level, m_bloomSrvs[level - 1].Get(),
            std::max(1u, bloomWidth >> (level - 1)), std::max(1u, bloomHeight >> (level - 1)));

    const float blendFactor[4] = {};
    context->OMSetBlendState(m_additiveBlend.Get(), blendFactor, 0xffffffffu);
    context->PSSetShader(m_bloomUpsampleShader.Get(), nullptr, 0);
    for (uint32_t level = m_bloomLevels; level-- > 1;)
        drawLevel(level - 1, m_bloomSrvs[level].Get(),
            std::max(1u, bloomWidth >> level), std::max(1u, bloomHeight >> level));

    context->PSSetShaderResources(0, 1, &nullView);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context->RSSetState(savedRasterizer.Get());
    if (savedViewportCount > 0)
        context->RSSetViewports(savedViewportCount, savedViewports);
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
    const auto settings = Engine::Graphics::GetPostProcessSettings();
    ID3D11ShaderResourceView* srv =
        useAccumulation ? m_accumulationSrv.Get() : m_sceneSrv.Get();
    context->OMSetDepthStencilState(m_depthDisabled.Get(), 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    const bool bloom = settings.bloomIntensity > 0.f && !m_bloomRtvs.empty();
    if (bloom)
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        RenderBloom(context, srv);
    }
    ID3D11ShaderResourceView* exposure = nullptr;
    if (settings.autoExposure && m_exposureMapRtv)
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        exposure = RenderExposure(context, srv, settings);
    }
    else
        m_exposure.Reset();
    context->OMSetRenderTargets(1, &output, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    auto composeSettings = settings;
    if (!bloom) composeSettings.bloomIntensity = 0.f;
    composeSettings.autoExposure = exposure != nullptr;
    const auto values = Engine::Graphics::MakeComposeConstants(composeSettings, m_bloomLevels);
    UploadConstants(context, &values);
    ID3D11Buffer* cb = m_constants.Get();
    ID3D11ShaderResourceView* views[3] = { srv, bloom ? m_bloomSrvs[0].Get() : nullptr, exposure };
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetConstantBuffers(0, 1, &cb); context->PSSetShaderResources(0, 3, views);
    context->PSSetSamplers(0, 1, &sampler); context->Draw(3, 0);
    ID3D11ShaderResourceView* nullViews[3] = {};
    context->PSSetShaderResources(0, 3, nullViews);
}
}
