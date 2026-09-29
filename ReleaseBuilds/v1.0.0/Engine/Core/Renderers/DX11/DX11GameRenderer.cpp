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
    m_captureStaging.Reset();
    m_exportTexture.Reset();
    m_exportRtv.Reset();
    m_exportReadbacks.clear();
    m_exportSubmitIndex = m_exportReadIndex = m_exportPendingCount = 0u;
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
    m_postProcess.Create(m_device.Get(), m_width, m_height);
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
    m_postProcess.BindScene(m_context.Get(), m_dsv.Get(), m_clearColor);
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
    memcpy(m_clearColor, color, sizeof(color));
    m_postProcess.BindScene(m_context.Get(), m_dsv.Get(), m_clearColor);
}

void DX11GameRenderer::EndFrame()
{
    ID3D11RenderTargetView* output = m_exportRtv
        ? m_exportRtv.Get() : m_rtv.Get();
    m_postProcess.Compose(m_context.Get(), output);
    if (m_graphicsProvider && m_graphicsProvider->GetContextFactory())
    {
        auto* factory = m_graphicsProvider->GetContextFactory();
        factory->FinalizeFrame();
        m_frameTelemetry = factory->GetFrameTimingTelemetry();
    }
    if (m_exportTexture && !m_exportReadbacks.empty())
    {
        if (m_exportPendingCount >= m_exportReadbacks.size())
            throw std::runtime_error("DX11 export readback queue is full");
        ExportReadbackSlot& slot = m_exportReadbacks[m_exportSubmitIndex];
        m_context->CopyResource(slot.staging.Get(), m_exportTexture.Get());
        m_context->End(slot.completion.Get());
        slot.pending = true;
        m_exportSubmitIndex = (m_exportSubmitIndex + 1u) %
            m_exportReadbacks.size();
        ++m_exportPendingCount;
        m_context->Flush();
        m_frameTelemetry.cpuPresentationMilliseconds = 0.0;
    }
    else
    {
        const auto presentationStart = std::chrono::steady_clock::now();
        ThrowIfFailed(m_swapChain->Present(0,
            m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0));
        m_frameTelemetry.cpuPresentationMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - presentationStart).count();
    }
    m_frameTelemetry.flipModelSwapChain = m_flipModelSwapChain;
}

bool DX11GameRenderer::EnableOffscreenExport(uint32_t readbackQueueDepth)
{
    if (!m_device || !m_context || !m_width || !m_height)
        return false;
    readbackQueueDepth = std::clamp(readbackQueueDepth, 2u, 8u);
    D3D11_TEXTURE2D_DESC output{};
    output.Width = m_width;
    output.Height = m_height;
    output.MipLevels = 1;
    output.ArraySize = 1;
    output.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    output.SampleDesc.Count = 1;
    output.Usage = D3D11_USAGE_DEFAULT;
    output.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_device->CreateTexture2D(&output, nullptr, &m_exportTexture)) ||
        FAILED(m_device->CreateRenderTargetView(
            m_exportTexture.Get(), nullptr, &m_exportRtv)))
        return false;

    D3D11_TEXTURE2D_DESC staging = output;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    m_exportReadbacks.clear();
    m_exportReadbacks.resize(readbackQueueDepth);
    for (ExportReadbackSlot& slot : m_exportReadbacks)
    {
        D3D11_QUERY_DESC query{};
        query.Query = D3D11_QUERY_EVENT;
        if (FAILED(m_device->CreateTexture2D(
                &staging, nullptr, &slot.staging)) ||
            FAILED(m_device->CreateQuery(&query, &slot.completion)))
        {
            m_exportTexture.Reset();
            m_exportRtv.Reset();
            m_exportReadbacks.clear();
            return false;
        }
    }
    m_exportSubmitIndex = m_exportReadIndex = m_exportPendingCount = 0u;
    return true;
}

bool DX11GameRenderer::ReadExportFrameRGBA(
    std::vector<uint8_t>& pixels, bool wait)
{
    if (!m_context || m_exportPendingCount == 0u ||
        m_exportReadbacks.empty())
        return false;
    ExportReadbackSlot& slot = m_exportReadbacks[m_exportReadIndex];
    if (!slot.pending)
        return false;
    HRESULT status = m_context->GetData(slot.completion.Get(), nullptr, 0,
        wait ? 0u : D3D11_ASYNC_GETDATA_DONOTFLUSH);
    while (wait && status == S_FALSE)
    {
        SwitchToThread();
        status = m_context->GetData(slot.completion.Get(), nullptr, 0, 0u);
    }
    if (status != S_OK)
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(slot.staging.Get(), 0,
        D3D11_MAP_READ, 0, &mapped)))
        return false;
    const size_t rowBytes = static_cast<size_t>(m_width) * 4u;
    pixels.resize(rowBytes * m_height);
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t row = 0; row < m_height; ++row)
        std::memcpy(pixels.data() + static_cast<size_t>(row) * rowBytes,
            source + static_cast<size_t>(row) * mapped.RowPitch, rowBytes);
    m_context->Unmap(slot.staging.Get(), 0);
    slot.pending = false;
    m_exportReadIndex = (m_exportReadIndex + 1u) % m_exportReadbacks.size();
    --m_exportPendingCount;
    return true;
}

bool DX11GameRenderer::CaptureFrameRGBA(std::vector<uint8_t>& pixels)
{
    if (!m_device || !m_context || !m_swapChain || !m_width || !m_height)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return false;
    D3D11_TEXTURE2D_DESC sourceDescription{};
    backBuffer->GetDesc(&sourceDescription);
    if (!m_captureStaging)
    {
        D3D11_TEXTURE2D_DESC staging = sourceDescription;
        staging.BindFlags = 0;
        staging.MiscFlags = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateTexture2D(&staging, nullptr,
            &m_captureStaging)))
            return false;
    }
    m_context->CopyResource(m_captureStaging.Get(), backBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_captureStaging.Get(), 0,
        D3D11_MAP_READ, 0, &mapped)))
        return false;
    const size_t rowBytes = static_cast<size_t>(m_width) * 4u;
    pixels.resize(rowBytes * m_height);
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t row = 0; row < m_height; ++row)
        std::memcpy(pixels.data() + static_cast<size_t>(row) * rowBytes,
            source + static_cast<size_t>(row) * mapped.RowPitch, rowBytes);
    m_context->Unmap(m_captureStaging.Get(), 0);
    return true;
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
