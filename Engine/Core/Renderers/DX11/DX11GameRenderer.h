#pragma once
#include "../IGameRenderer.h"
#include "DX11GraphicsProvider.h"
#include "DX11PostProcess.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <vector>
#include <cstddef>

namespace Engine::Renderers
{
class DX11GameRenderer : public IGameRenderer
{
public:
    ~DX11GameRenderer() override;
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    Engine::Graphics::IGraphicsProvider* GetGraphicsProvider() override { return m_graphicsProvider.get(); }
    void WaitIdle() override;
    void BeginFrame() override;
    void EndFrame() override;
    std::unique_ptr<Engine::Graphics::IGraphicsContext> CreateFrameGraphicsContext() override;
    Engine::Graphics::FrameTimingTelemetry GetFrameTimingTelemetry() const override
    { return m_frameTelemetry; }

    // Legacy synchronous swap-chain capture retained for diagnostics. Video
    // export uses the off-screen asynchronous queue below.
    bool CaptureFrameRGBA(std::vector<uint8_t>& pixels);
    bool EnableOffscreenExport(uint32_t outputWidth, uint32_t outputHeight,
        uint32_t msaaSamples = 1u, uint32_t readbackQueueDepth = 3u,
        bool hdrOutput = false, uint32_t hdrBits = 16u,
        bool linearOutput = false);
    void BeginExportFrame(uint32_t samplesPerFrame);
    bool ReadExportFrameRGBA(std::vector<uint8_t>& pixels, bool wait);
    bool ReadExportFrameFloatRGBA(std::vector<float>& pixels, bool wait);
    size_t GetPendingExportFrameCount() const { return m_exportPendingCount; }
    size_t GetExportReadbackCapacity() const { return m_exportReadbacks.size(); }

private:
    void CreateTargets();
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_captureStaging;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_exportTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_exportRtv;
    struct ExportReadbackSlot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        Microsoft::WRL::ComPtr<ID3D11Query> completion;
        bool pending = false;
    };
    std::vector<ExportReadbackSlot> m_exportReadbacks;
    size_t m_exportSubmitIndex = 0u;
    size_t m_exportReadIndex = 0u;
    size_t m_exportPendingCount = 0u;
    DX11PostProcess m_postProcess;
    float m_clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    std::unique_ptr<D3D11GraphicsProvider> m_graphicsProvider;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_exportWidth = 0;
    uint32_t m_exportHeight = 0;
    uint32_t m_msaaSamples = 1u;
    bool m_exportHdr = false;
    bool m_exportLinear = false;
    uint32_t m_hdrBits = 16u;
    uint32_t m_temporalSampleTarget = 1u;
    uint32_t m_temporalSamplesAccumulated = 0u;
    bool m_flipModelSwapChain = false;
    bool m_allowTearing = false;
    UINT m_swapChainFlags = 0;
    Engine::Graphics::FrameTimingTelemetry m_frameTelemetry{};
};
}
