#pragma once
#include "pch.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "../IEditorRenderer.h"
#include "DX12GraphicsProvider.h"
#include <vector>
#include <memory>

// ---------------------------------------------------------------------------
// DX12EditorRenderer
//
// Concrete implementation of IEditorRenderer using DirectX 12.
//
// An on-demand renderer for the editor. Unlike DX12GameRenderer, which redraws
// every tick unconditionally, this renderer only submits GPU work when something
// has actually changed (a dirty flag). This keeps the editor idle on the CPU
// and GPU when the user is not interacting.
//
// ---- Key differences from DX12GameRenderer ----
//
//   Present mode
//     Game:   Present(0, DXGI_PRESENT_ALLOW_TEARING) — uncapped, no vsync.
//     Editor: Present(1, 0) — vsync, one frame per vblank when dirty.
//             The editor does not need to push frames faster than the monitor
//             refresh rate; vsync prevents tearing at no meaningful cost.
//
//   Dirty flag
//     Call MarkDirty() whenever the display needs updating — on resize, input,
//     property changes, scene edits, etc. RenderIfNeeded() is a no-op until
//     then, so the GPU sits completely idle between interactions.
//
//   Public API
//     BeginFrame / Clear / EndFrame are private; only RenderIfNeeded() is
//     exposed. This prevents callers from forgetting to balance Begin/End and
//     makes the dirty-gate the single point of control.
//
// ---- Typical usage in the editor main loop ----
//
//   window->OnResize = [&](uint32_t w, uint32_t h) {
//       renderer->Resize(w, h);
//       renderer->MarkDirty();
//   };
//
//   window->OnUpdate = [&]() {
//       renderer->RenderIfNeeded([&]() {
//           renderer->Clear(0.18f, 0.18f, 0.18f);   // draw editor panels here
//       });
//   };
// ---------------------------------------------------------------------------

class DX12EditorRenderer : public IEditorRenderer
{
public:
    DX12EditorRenderer() = default;
    ~DX12EditorRenderer();

    // IRenderer interface
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    IGraphicsProvider* GetGraphicsProvider() override;

    // IEditorRenderer interface
    void MarkDirty() override { m_dirty = true; }
    void RenderIfNeeded(std::function<void()> drawFn = nullptr) override;
    std::pair<std::pair<void*, void*>, uint32_t> AllocateSrvSlot() override;
    void FreeSrvSlot(uint32_t slotIndex) override;
    bool CanAllocateSrvSlot() const override;
    uint32_t GetAvailableSrvSlots() const override;

    // ---------- D3D12-specific accessors (for internal use) ----------
    ID3D12Device*              GetDevice()      const { return m_device.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const;

    // Returns CPU and GPU descriptor handles for the given slot in the
    // shader-visible SRV heap. Slot 0 is reserved for the ImGui font atlas;
    // additional slots (1, 2, …) are available for scene textures etc.
    std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE>
        GetSrvSlot(uint32_t slot) const;

    // Number of back-buffer slots (double-buffering). Declared before member
    // arrays that use it as a size.
    static constexpr uint32_t FRAME_COUNT = 2;

private:
    // These are kept private so all rendering is gated through RenderIfNeeded.
    void BeginFrame();
    void EndFrame();

    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height);
    void CreateRTVHeap();
    void CreateRenderTargets();
    void FlushGPU();

    // -- Core device objects --
    ComPtr<ID3D12Device>              m_device;
    ComPtr<IDXGIFactory6>             m_factory;
    ComPtr<IDXGISwapChain3>           m_swapChain;

    // -- Command objects --
    ComPtr<ID3D12CommandQueue>        m_commandQueue;
    ComPtr<ID3D12CommandAllocator>    m_commandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // -- Render targets --
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    ComPtr<ID3D12Resource>            m_renderTargets[FRAME_COUNT];
    uint32_t                          m_rtvDescriptorSize = 0;

    // -- Synchronisation --
    ComPtr<ID3D12Fence>               m_fence;
    HANDLE                            m_fenceEvent  = nullptr;
    uint64_t                          m_fenceValue  = 0;
    uint64_t                          m_fenceValues[FRAME_COUNT]{};

    // -- imgui integration --
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;   // shader-visible, for ImGui font texture

    // -- SRV slot management --
    std::vector<uint32_t> m_freeSrvSlots;  // available slots for views
    uint32_t m_srvDescriptorSize = 0;

    // -- Viewport / scissor --
    D3D12_VIEWPORT                    m_viewport{};
    D3D12_RECT                        m_scissorRect{};

    uint32_t m_frameIndex = 0;
    uint32_t m_width      = 0;
    uint32_t m_height     = 0;

    // Initialized true so the very first call to RenderIfNeeded always draws.
    bool m_dirty = true;

    // Graphics provider for shader compilation, buffer creation, pipeline building
    std::unique_ptr<D3D12GraphicsProvider> m_graphicsProvider;
};
