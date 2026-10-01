#include "pch.h"
#include "DX11GameRenderer.h"
#include "Core/Graphics/PostProcess.h"
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
    depth.SampleDesc.Count = m_msaaSamples;
    depth.Usage = D3D11_USAGE_DEFAULT;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ThrowIfFailed(m_device->CreateTexture2D(&depth, nullptr, &m_depthTexture));
    ThrowIfFailed(m_device->CreateDepthStencilView(m_depthTexture.Get(), nullptr, &m_dsv));
    m_postProcess.Create(m_device.Get(), m_width, m_height, m_msaaSamples,
        m_hdrBits);
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
    const bool temporalFrame = m_temporalSampleTarget > 1u;
    if (temporalFrame)
    {
        m_postProcess.AccumulateScene(m_context.Get(),
            1.f / static_cast<float>(m_temporalSampleTarget));
        ++m_temporalSamplesAccumulated;
        if (m_temporalSamplesAccumulated < m_temporalSampleTarget)
        {
            if (m_graphicsProvider && m_graphicsProvider->GetContextFactory())
            {
                auto* factory = m_graphicsProvider->GetContextFactory();
                factory->FinalizeFrame();
                m_frameTelemetry = factory->GetFrameTimingTelemetry();
            }
            m_frameTelemetry.cpuPresentationMilliseconds = 0.0;
            m_frameTelemetry.flipModelSwapChain = m_flipModelSwapChain;
            return;
        }
    }
    ID3D11RenderTargetView* output = m_exportRtv
        ? m_exportRtv.Get() : m_rtv.Get();
    if (m_exportRtv)
    {
        D3D11_VIEWPORT viewport{ 0.f, 0.f,
            static_cast<float>(m_exportWidth),
            static_cast<float>(m_exportHeight), 0.f, 1.f };
        m_context->RSSetViewports(1, &viewport);
    }
    if (m_exportHdr)
    {
        auto postProcess = Engine::Graphics::GetPostProcessSettings();
        postProcess.toneMapping = m_exportLinear ? 5u : 3u;
        Engine::Graphics::SetPostProcessSettings(postProcess);
    }
    m_postProcess.Compose(m_context.Get(), output, temporalFrame);
    m_temporalSampleTarget = 1u;
    m_temporalSamplesAccumulated = 0u;
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

void DX11GameRenderer::BeginExportFrame(uint32_t samplesPerFrame)
{
    if (!m_exportTexture)
        throw std::logic_error("Export render target is not enabled");
    m_temporalSampleTarget = std::clamp(samplesPerFrame, 1u, 64u);
    m_temporalSamplesAccumulated = 0u;
    if (m_temporalSampleTarget > 1u)
        m_postProcess.ClearAccumulation(m_context.Get());
}

bool DX11GameRenderer::EnableOffscreenExport(uint32_t outputWidth,
    uint32_t outputHeight, uint32_t msaaSamples, uint32_t readbackQueueDepth,
    bool hdrOutput, uint32_t hdrBits, bool linearOutput)
{
    if (!m_device || !m_context || !m_width || !m_height ||
        !outputWidth || !outputHeight)
        return false;
    if (msaaSamples != 1u && msaaSamples != 2u && msaaSamples != 4u && msaaSamples != 8u)
        return false;
    m_msaaSamples = msaaSamples;
    m_exportWidth = outputWidth;
    m_exportHeight = outputHeight;
    m_exportHdr = hdrOutput;
    m_exportLinear = linearOutput;
    m_hdrBits = hdrBits >= 32u ? 32u : 16u;
    if (m_msaaSamples > 1u)
    {
        UINT qualityLevels = 0;
        UINT depthQualityLevels = 0;
        if (FAILED(m_device->CheckMultisampleQualityLevels(
            (m_hdrBits == 32u ? DXGI_FORMAT_R32G32B32A32_FLOAT
                : DXGI_FORMAT_R16G16B16A16_FLOAT),
                m_msaaSamples, &qualityLevels)) ||
            qualityLevels == 0u ||
            FAILED(m_device->CheckMultisampleQualityLevels(
                DXGI_FORMAT_D32_FLOAT_S8X24_UINT, m_msaaSamples,
                &depthQualityLevels)) || depthQualityLevels == 0u)
        {
            m_msaaSamples = 1u;
            return false;
        }
        m_dsv.Reset(); m_depthTexture.Reset();
        D3D11_TEXTURE2D_DESC depth{};
        depth.Width = m_width; depth.Height = m_height; depth.MipLevels = 1;
        depth.ArraySize = 1; depth.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth.SampleDesc.Count = m_msaaSamples; depth.Usage = D3D11_USAGE_DEFAULT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(m_device->CreateTexture2D(&depth, nullptr, &m_depthTexture)) ||
            FAILED(m_device->CreateDepthStencilView(m_depthTexture.Get(), nullptr, &m_dsv)))
            return false;
    }
    m_postProcess.Create(m_device.Get(), m_width, m_height, m_msaaSamples,
        m_hdrBits);
    readbackQueueDepth = std::clamp(readbackQueueDepth, 2u, 8u);
    D3D11_TEXTURE2D_DESC output{};
    output.Width = m_exportWidth;
    output.Height = m_exportHeight;
    output.MipLevels = 1;
    output.ArraySize = 1;
    output.Format = m_exportHdr ? (m_hdrBits == 32u
        ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT)
        : DXGI_FORMAT_R8G8B8A8_UNORM;
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
    const size_t rowBytes = static_cast<size_t>(m_exportWidth) *
        (m_exportHdr ? 8u : 4u);
    pixels.resize(rowBytes * m_exportHeight);
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    if (!m_exportHdr)
    {
        for (uint32_t row = 0; row < m_exportHeight; ++row)
            std::memcpy(pixels.data() + static_cast<size_t>(row) * rowBytes,
                source + static_cast<size_t>(row) * mapped.RowPitch, rowBytes);
    }
    else
    {
        const auto halfToFloat = [](uint16_t half)
        {
            const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16u;
            int32_t exponent = static_cast<int32_t>((half >> 10u) & 0x1fu);
            uint32_t mantissa = half & 0x3ffu;
            uint32_t bits = 0;
            if (exponent == 0u)
            {
                if (mantissa == 0u) bits = sign;
                else
                {
                    exponent = 1u;
                    while ((mantissa & 0x400u) == 0u)
                    { mantissa <<= 1u; --exponent; }
                    bits = sign | (static_cast<uint32_t>(exponent + 112) << 23u) |
                        ((mantissa & 0x3ffu) << 13u);
                }
            }
            else if (exponent == 31u)
                bits = sign | 0x7f800000u | (mantissa << 13u);
            else
                bits = sign | (static_cast<uint32_t>(exponent + 112) << 23u) |
                    (mantissa << 13u);
            float value = 0.f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        };
        for (uint32_t row = 0; row < m_exportHeight; ++row)
        {
            const auto* input = static_cast<const uint8_t*>(
                source + static_cast<size_t>(row) * mapped.RowPitch);
            auto* output = pixels.data() +
                static_cast<size_t>(row) * rowBytes;
            for (uint32_t column = 0; column < m_exportWidth; ++column)
            {
                for (uint32_t channel = 0; channel < 4u; ++channel)
                {
                    float channelValue = 0.f;
                    if (m_hdrBits == 32u)
                        std::memcpy(&channelValue, input +
                            (column * 4u + channel) * sizeof(float), sizeof(float));
                    else
                    {
                        uint16_t half = 0;
                        std::memcpy(&half, input +
                            (column * 4u + channel) * sizeof(uint16_t), sizeof(half));
                        channelValue = halfToFloat(half);
                    }
                    const float value = std::clamp(channelValue, 0.f, 1.f);
                    const uint16_t encoded = static_cast<uint16_t>(
                        std::lround(value * 65535.f));
                    std::memcpy(output + (column * 4u + channel) *
                        sizeof(encoded), &encoded, sizeof(encoded));
                }
            }
        }
    }
    m_context->Unmap(slot.staging.Get(), 0);
    slot.pending = false;
    m_exportReadIndex = (m_exportReadIndex + 1u) % m_exportReadbacks.size();
    --m_exportPendingCount;
    return true;
}

bool DX11GameRenderer::ReadExportFrameFloatRGBA(
    std::vector<float>& pixels, bool wait)
{
    if (!m_exportHdr || !m_context || m_exportPendingCount == 0u ||
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
    pixels.resize(static_cast<size_t>(m_exportWidth) * m_exportHeight * 4u);
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    const auto halfToFloat = [](uint16_t half)
    {
        const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16u;
        int32_t exponent = static_cast<int32_t>((half >> 10u) & 0x1fu);
        uint32_t mantissa = half & 0x3ffu;
        uint32_t bits = 0;
        if (exponent == 0)
        {
            if (!mantissa) bits = sign;
            else
            {
                exponent = 1;
                while ((mantissa & 0x400u) == 0u)
                { mantissa <<= 1u; --exponent; }
                bits = sign | (static_cast<uint32_t>(exponent + 112) << 23u) |
                    ((mantissa & 0x3ffu) << 13u);
            }
        }
        else if (exponent == 31)
            bits = sign | 0x7f800000u | (mantissa << 13u);
        else
            bits = sign | (static_cast<uint32_t>(exponent + 112) << 23u) |
                (mantissa << 13u);
        float value = 0.f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    for (uint32_t row = 0; row < m_exportHeight; ++row)
    {
        const uint8_t* rowSource = source +
            static_cast<size_t>(row) * mapped.RowPitch;
        for (uint32_t column = 0; column < m_exportWidth * 4u; ++column)
        {
            if (m_hdrBits == 32u)
                std::memcpy(&pixels[static_cast<size_t>(row) * m_exportWidth * 4u + column],
                    rowSource + column * sizeof(float), sizeof(float));
            else
            {
                uint16_t half = 0;
                std::memcpy(&half, rowSource + column * sizeof(half), sizeof(half));
                pixels[static_cast<size_t>(row) * m_exportWidth * 4u + column] =
                    halfToFloat(half);
            }
        }
    }
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
