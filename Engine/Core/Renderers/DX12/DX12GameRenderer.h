#pragma once
#include "pch.h"
#include "../IGameRenderer.h"
#include "DX12GraphicsProvider.h"
#include <memory>

// ---------------------------------------------------------------------------
// DX12Renderer — Tutorial Overview
//
// DirectX 12 is an *explicit* GPU API. Unlike D3D11, the driver does almost
// nothing automatically — you are responsible for:
//   1. Allocating and managing GPU memory and objects.
//   2. Recording GPU work into command lists.
//   3. Submitting those lists to a command queue for the GPU to execute.
//   4. Synchronising the CPU and GPU (via fences) so you never read or write
//      memory that the other side is still using.
//
// ---- Core objects ----
//   ID3D12Device
//     The logical GPU handle. Almost every D3D12 object (heaps, resources,
//     fences, command queues …) is created by calling a method on the device.
//     One device per physical adapter is typical.
//
//   ID3D12CommandQueue
//     A FIFO queue that the GPU drains in order. The CPU calls
//     ExecuteCommandLists() to push closed command lists onto the queue; the
//     GPU processes them asynchronously. Think of it as the pipeline feeding
//     work to the hardware. You can have multiple queues (graphics, compute,
//     copy) for concurrent execution, but this renderer uses one.
//
//   ID3D12CommandAllocator
//     Raw GPU-side backing memory for the command stream recorded into a list.
//     Cannot be reset while the GPU is still executing commands from it.
//     We keep one per back-buffer frame so we can safely reset slot N once the
//     GPU finishes frame N, even while it is still running frame N+1.
//
//   ID3D12GraphicsCommandList
//     The "pen" you record commands into (draw calls, clears, barriers …).
//     After recording: Close() the list, then pass it to ExecuteCommandLists().
//     Afterwards Reset() re-attaches it to a fresh allocator for the next frame.
//
// ---- Swap chain & double-buffering ----
//   The swap chain owns two back-buffer textures (FRAME_COUNT = 2). While the
//   GPU renders into buffer N, the display scans out buffer N-1. Present()
//   "flips" them. Having one allocator per buffer means we can build frame N+1
//   on the CPU while the GPU is still consuming frame N.
//
// ---- Descriptor heaps & render target views ----
//   In D3D12 you never bind a raw resource to the pipeline; you bind a small
//   "descriptor" (view) stored in a descriptor heap. A Render Target View (RTV)
//   describes the format and subresource layout the Output Merger stage uses
//   when writing colour data into a texture. Descriptor heaps are flat arrays;
//   addressing a specific slot requires manual pointer arithmetic using the
//   hardware-specific descriptor stride.
//
// ---- Fence synchronisation ----
//   A fence is a GPU/CPU synchronisation primitive holding a monotonically
//   increasing 64-bit integer. After submitting work you call
//   Queue::Signal(fence, N) — the GPU writes N into the fence once all prior
//   work is done. The CPU polls GetCompletedValue() or calls
//   SetEventOnCompletion() + WaitForSingleObject() to sleep until the GPU
//   reaches that value. We store one completion target per swap-chain slot
//   (m_fenceValues[]) so we only stall when we actually need to reuse a
//   back-buffer the GPU might still be writing.
//
// ---------------------------------------------------------------------------
// DX12GameRenderer — Tutorial Overview
//
// Concrete implementation of IGameRenderer using DirectX 12.
// ---------------------------------------------------------------------------

class DX12GameRenderer : public IGameRenderer
{
public:
    DX12GameRenderer() = default;
    ~DX12GameRenderer();

    // IRenderer interface
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    IGraphicsProvider* GetGraphicsProvider() override;

    // IGameRenderer interface
    void BeginFrame() override;   // reset allocator + list, transition backbuffer to RENDER_TARGET
    void EndFrame() override;     // execute list, present, signal fence

    // ---------- D3D12-specific accessors (for internal use) ----------
    ID3D12Device*              GetDevice()      const { return m_device.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const;

    // Number of back-buffer slots in the swap chain (double-buffering).
    // Must be declared before any member arrays that use it as a size.
    static constexpr uint32_t FRAME_COUNT = 2;

private:
    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height);
    void CreateRTVHeap();
    void CreateRenderTargets();
    void FlushGPU(); // wait for all GPU work to finish (used in Resize / dtor)

    // -- Core device objects --
    // m_device    : The logical D3D12 device. Every resource and object is
    //               created through it. There is typically one instance per GPU.
    // m_factory   : DXGI factory used to enumerate adapters and create the swap
    //               chain. IDXGIFactory6 exposes EnumAdapterByGpuPreference so
    //               we can prefer the discrete GPU on hybrid (iGPU+dGPU) systems.
    // m_swapChain : Manages the back-buffer textures. IDXGISwapChain3 adds
    //               GetCurrentBackBufferIndex() which tells us which slot is
    //               ready to render into after each Present() call.
    ComPtr<ID3D12Device>              m_device;
    ComPtr<IDXGIFactory6>             m_factory;
    ComPtr<IDXGISwapChain3>           m_swapChain;

    // -- Command objects --
    // m_commandQueue      : The GPU's work intake. ExecuteCommandLists() pushes
    //                       closed command lists onto it. TYPE_DIRECT accepts
    //                       any graphics, compute, or copy commands.
    // m_commandAllocators : Raw GPU memory backing the command stream. One per
    //                       swap-chain slot — never reset an allocator while the
    //                       GPU may still be reading from it (checked via fence).
    // m_commandList       : Reusable recording object. Reset() onto the current
    //                       frame's allocator at BeginFrame(), Close() and submit
    //                       at EndFrame(). One list is sufficient because we
    //                       always wait for the GPU to finish a slot before
    //                       reusing it.
    ComPtr<ID3D12CommandQueue>        m_commandQueue;
    ComPtr<ID3D12CommandAllocator>    m_commandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // -- Render targets --
    // m_rtvHeap          : Descriptor heap holding one RTV slot per back-buffer.
    //                      Descriptors are small GPU-readable structs that describe
    //                      how a resource should be interpreted. RTVs are CPU-only
    //                      (D3D12_DESCRIPTOR_HEAP_FLAG_NONE) because OMSetRenderTargets
    //                      takes a CPU handle, not a shader-visible root argument.
    // m_renderTargets    : The D3D12 texture resources owned by the swap chain.
    //                      Retrieved via GetBuffer(); we hold refs so we can
    //                      transition their state (PRESENT ↔ RENDER_TARGET) each frame.
    // m_rtvDescriptorSize: Descriptor size is hardware-dependent. Always query it
    //                      via GetDescriptorHandleIncrementSize — never hard-code it.
    //                      Used to stride through the heap: heap_base + i * stride.
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    ComPtr<ID3D12Resource>            m_renderTargets[FRAME_COUNT];
    uint32_t                          m_rtvDescriptorSize = 0;

    // -- Sync --
    // m_fence       : GPU/CPU synchronisation primitive. The GPU writes an ever-
    //                 increasing integer into it when it finishes batches of work.
    // m_fenceEvent  : Win32 auto-reset event. The CPU sleeps on it (via
    //                 WaitForSingleObject) while waiting for the GPU to catch up.
    //                 Auto-reset means it clears itself after a single wait.
    // m_fenceValue  : Global monotonic counter incremented every Signal() call.
    //                 Always increasing ensures we never accidentally reuse a value
    //                 that has already been signalled (which would look "done").
    // m_fenceValues : Per-frame "you must reach at least this value before I reuse
    //                 this slot" watermarks. BeginFrame checks the fence against
    //                 m_fenceValues[m_frameIndex] before resetting the allocator.
    ComPtr<ID3D12Fence>               m_fence;
    HANDLE                            m_fenceEvent = nullptr;
    uint64_t                          m_fenceValue  = 0;           // global monotonic counter
    uint64_t                          m_fenceValues[FRAME_COUNT]{}; // per-slot completion values

    // -- Viewport / scissor --
    // The viewport maps clip-space coordinates to window pixels. TopLeftX/Y,
    // Width, Height define the 2D region; MinDepth/MaxDepth (0..1) map NDC
    // depth values into the depth buffer range. The scissor rect clips
    // rasterisation to a pixel rectangle — anything outside is discarded. We
    // set both to cover the full window and update them on every Resize().
    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT     m_scissorRect{};

    uint32_t m_frameIndex = 0;
    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    // True when the DXGI adapter supports tearing (queried once during Init).
    // Required for Present(0, DXGI_PRESENT_ALLOW_TEARING) to actually uncap
    // the frame rate in windowed mode — Present(0,0) alone isn't sufficient
    // because DWM can still throttle to the monitor refresh rate.
    bool m_tearingSupported = false;

    // Graphics provider for shader compilation, buffer creation, pipeline building
    std::unique_ptr<D3D12GraphicsProvider> m_graphicsProvider;

};
