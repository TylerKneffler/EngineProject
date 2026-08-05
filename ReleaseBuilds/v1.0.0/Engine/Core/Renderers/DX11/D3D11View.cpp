#include "pch.h"
#include "D3D11View.h"

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

    D3D11_TEXTURE2D_DESC depth = color;
    depth.Format = DXGI_FORMAT_D32_FLOAT;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ThrowIfFailed(device->CreateTexture2D(&depth, nullptr, &m_depthTexture));
    ThrowIfFailed(device->CreateDepthStencilView(m_depthTexture.Get(), nullptr, &m_dsv));
}

void D3D11View::Render(void* contextHandle, void* mainRtvHandle,
                       std::function<void(void*)> drawFn)
{
    auto* context = static_cast<ID3D11DeviceContext*>(contextHandle);
    auto* mainRtv = static_cast<ID3D11RenderTargetView*>(mainRtvHandle);
    if (!context || !mainRtv || !m_rtv) return;

    ID3D11RenderTargetView* target = m_rtv.Get();
    context->OMSetRenderTargets(1, &target, m_dsv.Get());
    D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    D3D11_RECT scissor{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    context->RSSetScissorRects(1, &scissor);
    const float clearColor[4] = { 0.0f, 0.0f, 0.502f, 1.0f };
    context->ClearRenderTargetView(target, clearColor);
    context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    if (drawFn) drawFn(context);

    context->OMSetRenderTargets(1, &mainRtv, nullptr);
}
