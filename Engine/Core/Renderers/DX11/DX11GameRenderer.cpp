#include "pch.h"
#include "DX11GameRenderer.h"
#include <chrono>

namespace Engine::Renderers
{
DX11GameRenderer::~DX11GameRenderer()
{
    if (m_context) m_context->ClearState();
}

bool DX11GameRenderer::Init(void* hwndHandle, uint32_t width, uint32_t height)
{
    try
    {
        DXGI_SWAP_CHAIN_DESC swap{};
        swap.BufferDesc.Width = width;
        swap.BufferDesc.Height = height;
        swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap.SampleDesc.Count = 1;
        swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap.BufferCount = 2;
        swap.OutputWindow = static_cast<HWND>(hwndHandle);
        swap.Windowed = TRUE;
        swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
        BOOL tearingSupported = FALSE;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory5))) &&
            SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearingSupported,
                sizeof(tearingSupported))) && tearingSupported)
        {
            swap.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            m_allowTearing = true;
        }
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL selected{};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &swap, &m_swapChain,
            &m_device, &selected, &m_context);
#if defined(_DEBUG)
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                &swap, &m_swapChain, &m_device, &selected, &m_context);
#endif
        if (hr == E_INVALIDARG)
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                flags & ~D3D11_CREATE_DEVICE_DEBUG, levels + 1, 1, D3D11_SDK_VERSION,
                &swap, &m_swapChain, &m_device, &selected, &m_context);
        m_flipModelSwapChain = SUCCEEDED(hr);
        if (FAILED(hr))
        {
            // Older DXGI runtimes do not accept flip-discard through the
            // legacy creation helper. Preserve compatibility with blt-model.
            m_swapChain.Reset(); m_context.Reset(); m_device.Reset();
            swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            swap.Flags = 0;
            hr = D3D11CreateDeviceAndSwapChain(nullptr,
                D3D_DRIVER_TYPE_HARDWARE, nullptr,
                flags & ~D3D11_CREATE_DEVICE_DEBUG, levels + 1, 1,
                D3D11_SDK_VERSION, &swap, &m_swapChain, &m_device,
                &selected, &m_context);
            m_flipModelSwapChain = false;
            m_allowTearing = false;
        }
        ThrowIfFailed(hr);
        m_swapChainFlags = swap.Flags;
        m_width = width;
        m_height = height;
        CreateTargets();
        m_graphicsProvider = std::make_unique<D3D11GraphicsProvider>(m_device.Get(), m_context.Get());
        return true;
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("DX11 game initialization failed: ") + error.what() + "\n").c_str());
        return false;
    }
}

void DX11GameRenderer::CreateTargets()
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    ThrowIfFailed(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv));
    D3D11_TEXTURE2D_DESC depth{};
    depth.Width = m_width;
    depth.Height = m_height;
    depth.MipLevels = 1;
    depth.ArraySize = 1;
    depth.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    depth.SampleDesc.Count = 1;
    depth.Usage = D3D11_USAGE_DEFAULT;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ThrowIfFailed(m_device->CreateTexture2D(&depth, nullptr, &m_depthTexture));
    ThrowIfFailed(m_device->CreateDepthStencilView(m_depthTexture.Get(), nullptr, &m_dsv));
}

void DX11GameRenderer::Resize(uint32_t width, uint32_t height)
{
    if (!m_swapChain || width == 0 || height == 0 || (width == m_width && height == m_height)) return;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_dsv.Reset(); m_depthTexture.Reset(); m_rtv.Reset();
    ThrowIfFailed(m_swapChain->ResizeBuffers(0, width, height,
        DXGI_FORMAT_UNKNOWN, m_swapChainFlags));
    m_width = width; m_height = height;
    CreateTargets();
}

void DX11GameRenderer::BeginFrame()
{
    if (m_graphicsProvider)
        m_graphicsProvider->GetContextFactory()->PrepareFrame(0);
    ID3D11RenderTargetView* target = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &target, m_dsv.Get());
    D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_context->RSSetViewports(1, &viewport);
    D3D11_RECT scissor{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    m_context->RSSetScissorRects(1, &scissor);
    m_context->ClearDepthStencilView(
        m_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void DX11GameRenderer::Clear(float r, float g, float b, float a)
{
    const float color[4] = { r, g, b, a };
    m_context->ClearRenderTargetView(m_rtv.Get(), color);
}

void DX11GameRenderer::EndFrame()
{
    if (m_graphicsProvider && m_graphicsProvider->GetContextFactory())
    {
        auto* factory = m_graphicsProvider->GetContextFactory();
        factory->FinalizeFrame();
        m_frameTelemetry = factory->GetFrameTimingTelemetry();
    }
    const auto presentationStart = std::chrono::steady_clock::now();
    ThrowIfFailed(m_swapChain->Present(0,
        m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0));
    m_frameTelemetry.cpuPresentationMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - presentationStart).count();
    m_frameTelemetry.flipModelSwapChain = m_flipModelSwapChain;
}

std::unique_ptr<Engine::Graphics::IGraphicsContext> DX11GameRenderer::CreateFrameGraphicsContext()
{
    return m_graphicsProvider && m_graphicsProvider->GetContextFactory()
        ? m_graphicsProvider->GetContextFactory()->CreateContext()
        : nullptr;
}

void DX11GameRenderer::WaitIdle()
{
    if (!m_device || !m_context) return;
    D3D11_QUERY_DESC descriptor{};
    descriptor.Query = D3D11_QUERY_EVENT;
    Microsoft::WRL::ComPtr<ID3D11Query> completion;
    if (FAILED(m_device->CreateQuery(&descriptor, &completion))) return;
    m_context->End(completion.Get());
    m_context->Flush();
    while (m_context->GetData(completion.Get(), nullptr, 0, 0) == S_FALSE)
        SwitchToThread();
}
}
