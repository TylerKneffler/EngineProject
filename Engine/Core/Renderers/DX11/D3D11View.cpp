#include "pch.h"
#include "Core/Graphics/DepthConvention.h"
#include "D3D11View.h"
#include "Core/Rendering/RenderingEffects.h"

namespace Engine::Renderers
{
void D3D11View::Init(void* device, uint32_t width, uint32_t height,
                     void*, void*, uint32_t srvSlotIndex)
{
    m_srvSlotIndex = srvSlotIndex;
    CreateResources(static_cast<ID3D11Device*>(device), width, height);
}

void D3D11View::Resize(void* device, uint32_t width, uint32_t height)
{
    if (!device || width == 0 || height == 0 || (width == m_width && height == m_height)) return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    static_cast<ID3D11Device*>(device)->GetImmediateContext(&context);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(0, 1, &nullSrv);
    m_dsv.Reset();
    m_depthSrv.Reset();
    m_depthTexture.Reset();
    m_srv.Reset();
    m_rtv.Reset();
    m_texture.Reset();
    CreateResources(static_cast<ID3D11Device*>(device), width, height);
}

void D3D11View::CreateResources(ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (!device || width == 0 || height == 0)
        throw std::runtime_error("Invalid DX11 view dimensions or device");
    m_width = width;
    m_height = height;
    m_aspect = static_cast<float>(width) / static_cast<float>(height);

    D3D11_TEXTURE2D_DESC color{};
    color.Width = width;
    color.Height = height;
    color.MipLevels = 1;
    color.ArraySize = 1;
    color.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color.SampleDesc.Count = 1;
    color.Usage = D3D11_USAGE_DEFAULT;
    color.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device->CreateTexture2D(&color, nullptr, &m_texture));
    ThrowIfFailed(device->CreateRenderTargetView(m_texture.Get(), nullptr, &m_rtv));
    ThrowIfFailed(device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv));

    // The project's MSAA count smooths thin geometry in editor views; the UI
    // texture stays single-sampled (the post-process resolves into it).
    const uint32_t samples = DesiredSamples(device);
    m_samples = samples;

    D3D11_TEXTURE2D_DESC depth = color;
    depth.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    depth.SampleDesc.Count = samples;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device->CreateTexture2D(&depth, nullptr, &m_depthTexture));
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsv.ViewDimension = samples > 1u ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    ThrowIfFailed(device->CreateDepthStencilView(m_depthTexture.Get(), &dsv, &m_dsv));
    m_depthSrv.Reset();
    if (samples == 1u)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(m_depthTexture.Get(), &srv, &m_depthSrv)))
            m_depthSrv.Reset();
    }
    m_postProcess.Create(device, width, height, samples);
}

uint32_t D3D11View::DesiredSamples(ID3D11Device* device)
{
    uint32_t samples = Engine::Rendering::RenderingEffects::Get().Settings().antiAliasing.msaaSamples;
    for (; samples > 1u; samples /= 2u)
    {
        UINT colorQuality = 0, depthQuality = 0;
        if (SUCCEEDED(device->CheckMultisampleQualityLevels(
                DXGI_FORMAT_R16G16B16A16_FLOAT, samples, &colorQuality)) && colorQuality > 0 &&
            SUCCEEDED(device->CheckMultisampleQualityLevels(
                DXGI_FORMAT_D32_FLOAT_S8X24_UINT, samples, &depthQuality)) && depthQuality > 0)
            break;
    }
    return std::max(1u, samples);
}

void D3D11View::Render(void* contextHandle, void* mainRtvHandle,
                       std::function<void(void*)> drawFn,
                       std::function<void(void*)> preDrawFn)
{
    auto* context = static_cast<ID3D11DeviceContext*>(contextHandle);
    auto* mainRtv = static_cast<ID3D11RenderTargetView*>(mainRtvHandle);
    if (!context || !mainRtv || !m_rtv) return;
    {
        // Follow MSAA changes made in Preferences without waiting for a resize.
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (device && DesiredSamples(device.Get()) != m_samples)
        {
            const uint32_t width = m_width, height = m_height;
            m_width = m_height = 0;
            Resize(device.Get(), width, height);
            if (!m_rtv) return;
        }
    }

    if (preDrawFn) preDrawFn(contextHandle);

    ID3D11RenderTargetView* target = m_rtv.Get();
    D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    D3D11_RECT scissor{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    context->RSSetScissorRects(1, &scissor);
    m_postProcess.BindScene(context, m_dsv.Get(), m_clearColor);
    context->ClearDepthStencilView(
        m_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, Engine::Graphics::Depth::ClearValue, 0);

    if (drawFn) drawFn(context);

    m_postProcess.SetSceneDepth(m_depthSrv.Get());
    m_postProcess.Compose(context, target);
    context->OMSetRenderTargets(1, &mainRtv, nullptr);
}
}
