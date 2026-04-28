#include "DX12GameRenderer.h"
#include "DX12GraphicsContext.h"

// ---------------------------------------------------------------------------
// Destruction
//
// IMPORTANT: the GPU is an independent processor that may still be executing
// commands we submitted frames ago. If we allow C++ destructors to release
// COM objects (textures, command queues, the device …) while the GPU is still
// reading from or writing to them, the driver will crash or report validation
// errors, and the behaviour is undefined.
//
// FlushGPU() signals a fresh fence value and blocks the calling thread until
// the GPU acknowledges it, guaranteeing the GPU is completely idle before any
// ComPtr destructor runs. Only after that is it safe to close the Win32
// fence-event handle with CloseHandle().
// ---------------------------------------------------------------------------
DX12GameRenderer::~DX12GameRenderer()
{
    FlushGPU();
    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool DX12GameRenderer::Init(void* hwnd, uint32_t width, uint32_t height)
{
    try
    {
        m_width  = width;
        m_height = height;

        // ---- Initialisation order matters ----
        // 1. CreateDevice()         — must come first; everything else is created
        //                              through the ID3D12Device it produces.
        // 2. CreateCommandObjects() — command queue is created here, and
        //                              CreateSwapChain needs the queue pointer because
        //                              DXGI ties Present() signals to that queue.
        // 3. CreateSwapChain()      — creates the back-buffer textures and the
        //                              DXGI flip mechanism.
        // 4. CreateRTVHeap()        — allocates the descriptor heap that will hold
        //                              the Render Target Views (one per back-buffer).
        // 5. CreateRenderTargets()  — retrieves the back-buffer resources from the
        //                              swap chain and writes RTVs into the heap.
        CreateDevice();
        CreateCommandObjects();
        CreateSwapChain(static_cast<HWND>(hwnd), width, height);
        CreateRTVHeap();
        CreateRenderTargets();

        // ---- Graphics provider ----
        // Initialize the graphics provider for shader compilation, buffer creation, etc.
        m_graphicsProvider = std::make_unique<D3D12GraphicsProvider>(
            m_device.Get(), m_commandQueue.Get(), nullptr);

    // ---- Fence ----
    // A fence is a GPU/CPU synchronisation primitive. The GPU signals the fence
    // to a monotonically increasing integer value when it finishes a batch of
    // submitted work. The CPU can then either poll GetCompletedValue() or block
    // on a Win32 event until the desired value is reached. We keep one fence
    // shared across all frames, with a per-slot "target" value (m_fenceValues)
    // so we only stall when we actually need to reuse a back-buffer that the GPU
    // might still be writing into.
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence)));
    // m_fenceValues stays zero-initialised — no pre-wait needed on the first frame.

    // The fence event is the Win32 auto-reset event that the CPU sleeps on when
    // it needs to wait for the GPU. SetEventOnCompletion() will signal it once
    // the fence reaches the requested value, waking the WaitForSingleObject call.
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("Failed to create fence event.");

    // ---- Viewport + scissor ----
    // The viewport maps normalised device coordinates (NDC) to window pixels.
    // Fields: TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth.
    // MinDepth/MaxDepth of 0..1 is the standard full-range depth mapping.
    //
    // The scissor rect clips rasterisation to a 2D pixel rectangle. Anything
    // the rasteriser produces outside this rect is discarded. We set it to the
    // full window here (effectively “no clip”), but D3D12 requires one to be
    // bound or the validation layer will warn. Both are re-set on every Resize().
    m_viewport   = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
    m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    return true;
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA("Init failed: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
    // QueryPerformanceCounter (QPC) reads a hardware high-resolution tick counter
    // that increments at a steady rate (typically tens of MHz or more). Dividing
    // a tick delta by the frequency obtained from QueryPerformanceFrequency gives
    // elapsed time in seconds. We seed m_perfLast here so that the very first
    // DrawFPS() call computes a reasonable delta time instead of garbage.
void DX12GameRenderer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    // Now wait for all GPU work — DX12 and D3D11On12 — to finish.
    FlushGPU();

    // Release render target references before resizing the swap chain.
    // ResizeBuffers() requires that the application holds no live CPU references
    // to any back-buffer surface. ComPtr::Reset() drops our reference; the swap
    // chain itself still holds one internally, so the resource is not actually
    // destroyed until ResizeBuffers() replaces it.
    //
    // We also level all per-slot fence watermarks to m_fenceValues[m_frameIndex].
    // After FlushGPU() the GPU has completed ALL in-flight frames, so every slot
    // is safe to reuse. Setting all watermarks to the highest completed value
    // prevents BeginFrame from spuriously stalling on a slot whose old target
    // value has already been surpassed by the global fence counter.
    for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    {
        m_renderTargets[i].Reset();
        m_fenceValues[i] = m_fenceValues[m_frameIndex];
    }

    ThrowIfFailed(m_swapChain->ResizeBuffers(
        FRAME_COUNT, width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
            | (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u)));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_width  = width;
    m_height = height;

    CreateRenderTargets();

    m_viewport    = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
    m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------
void DX12GameRenderer::BeginFrame()
{
    // ---- Per-frame fence wait ----
    // m_fenceValues[m_frameIndex] is the value we signalled at the end of the
    // *last* time we rendered into this slot (FRAME_COUNT frames ago in steady
    // state). If the GPU's completed value hasn’t reached it yet, the GPU is
    // still reading/writing this slot’s resources — we must wait.
    //
    // SetEventOnCompletion() registers m_fenceEvent to be auto-signalled by the
    // GPU once the fence hits the target. WaitForSingleObject() puts the CPU
    // thread to sleep (costing zero CPU cycles) until that happens.
    //
    // On the first two frames all m_fenceValues[] are 0 and GetCompletedValue()
    // also returns 0 (fence was created with initial value 0), so the condition
    // is false and we skip the wait entirely — no stall on warm-up frames.
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(
            m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    // ---- Reset command allocator and list ----
    // Now that the GPU has finished with this slot’s allocator, Reset() reclaims
    // its backing memory for new commands (without freeing the allocation). All
    // previously recorded commands are discarded. The command list is then
    // Reset() on top of this allocator to begin recording the new frame.
    // nullptr as the pipeline state arg means “no initial PSO” — fine here
    // because we set pipeline state explicitly before each draw call.
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(
        m_commandAllocators[m_frameIndex].Get(), nullptr));

    // ---- Resource barrier: PRESENT → RENDER_TARGET ----
    // D3D12 tracks a "state" for each resource that describes how the hardware
    // is currently accessing it (display scanout, shader read, render target
    // write, etc.). Before you change how you use a resource you MUST insert a
    // transition barrier so the driver can flush caches and stall the relevant
    // pipeline stages. Skipping barriers causes corruption or GPU hangs.
    //
    // After Present() the swap chain moves the back-buffer to PRESENT state
    // (the display engine is scanning it out). We need RENDER_TARGET state so
    // the Output Merger stage can write colour data into it. The barrier
    // declares both the old state (StateBefore) and the new state (StateAfter)
    // so the driver knows exactly what cache flushes and pipeline stalls are
    // required — for example, invalidating the render-target cache.
    //
    // D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES is a wildcard that covers every
    // mip level and array slice in the texture. For a plain 2D back-buffer
    // there is only one subresource, but using the wildcard is correct practice
    // and guards against future resource type changes.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // ---- Pipeline state: viewport, scissor, render target ----
    // D3D12 pipeline state is NOT persistent across command-list resets; every
    // Reset() starts with a blank slate. We must re-bind the viewport, scissor,
    // and render target at the start of every frame.
    //
    // RSSetViewports / RSSetScissorRects bind to the Rasteriser Stage (RS).
    // OMSetRenderTargets binds to the Output Merger (OM) stage. Arguments:
    //   1            — number of RTVs to bind.
    //   &rtv         — pointer to CPU descriptor handle(s).
    //   FALSE        — handles are NOT contiguous in the heap
    //                  (we pass individual handles, not a range).
    //   nullptr      — no depth-stencil view; this renderer has no depth buffer.
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetCurrentRTV();
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
}

void DX12GameRenderer::Clear(float r, float g, float b, float a)
{
    // ClearRenderTargetView fills every pixel in the render target with the
    // given RGBA colour. The last two arguments (0, nullptr) mean “no scissor
    // rects” — clear the entire surface. This erases any content left over
    // from the previous frame. Call after BeginFrame() and before any draws.
    const float color[4] = { r, g, b, a };
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetCurrentRTV();
    m_commandList->ClearRenderTargetView(rtv, color, 0, nullptr);
}

void DX12GameRenderer::EndFrame()
{
    // NOTE: The RENDER_TARGET → PRESENT barrier is NOT recorded here.
    // RenderOverlay() calls ReleaseWrappedResources which issues that barrier
    // via the D3D11On12 device's command list, submitted with Flush().

    // ---- Submit the DX12 command list ----
    // Close() finalises the command list; no more commands can be appended
    // until it is Reset(). It MUST be closed before submission.
    // ExecuteCommandLists() enqueues the list on the GPU command queue.
    // The GPU starts executing it asynchronously; the CPU does not block here.
    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // Present — pass ALLOW_TEARING when supported so DWM does not throttle
    // the frame rate to the monitor refresh rate in windowed mode.
    const UINT presentFlags = m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    ThrowIfFailed(m_swapChain->Present(0, presentFlags));

    // ---- Record fence watermark and signal ----
    // Increment the global counter to produce a fresh target value, store it
    // as the completion watermark for this slot, then call Queue::Signal().
    // Signal() enqueues a GPU-side write: once the GPU drains everything we
    // submitted above (command list + overlay flush), it writes this value into
    // the fence. The CPU does NOT block here.
    //
    // The next time BeginFrame() runs for this same slot (one full rotation
    // later, FRAME_COUNT frames from now), it compares GetCompletedValue()
    // against m_fenceValues[m_frameIndex]. If the GPU hasn’t reached it yet,
    // it waits. This guarantees we never reset the allocator or re-record into
    // the back-buffer while the GPU is still consuming them.
    //
    // We signal BEFORE advancing m_frameIndex so the watermark is stored in
    // the slot that was just rendered, not the next one.
    m_fenceValues[m_frameIndex] = ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]));

    // ---- Advance to the next back-buffer slot ----
    // For FLIP_DISCARD swap chains, GetCurrentBackBufferIndex() cycles
    // 0→1→0→… after each Present(). We query rather than manually toggling
    // to stay in sync with the swap chain’s internal state.
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// ---------------------------------------------------------------------------
// GetGraphicsProvider
// ---------------------------------------------------------------------------
IGraphicsProvider* DX12GameRenderer::GetGraphicsProvider()
{
    return m_graphicsProvider.get();
}

std::unique_ptr<IGraphicsContext> DX12GameRenderer::CreateFrameGraphicsContext()
{
    return std::make_unique<D3D12GraphicsContext>(m_commandList.Get());
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
D3D12_CPU_DESCRIPTOR_HANDLE DX12GameRenderer::GetCurrentRTV() const
{
    // Descriptor heaps are flat arrays in GPU memory. GetCPUDescriptorHandleForHeapStart()
    // returns the CPU address of slot 0. To address slot N we stride forward by
    // N * m_rtvDescriptorSize bytes. The stride is hardware-dependent and was
    // queried via GetDescriptorHandleIncrementSize() during CreateRTVHeap() —
    // never hard-code it; it can differ between GPU vendors.
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    return handle;
}

// ---------------------------------------------------------------------------
// Private init helpers
// ---------------------------------------------------------------------------
void DX12GameRenderer::CreateDevice()
{
    // ---- Debug layer ----
    // The D3D12 debug layer intercepts every API call and validates usage:
    // wrong resource states, missing barriers, descriptor heap misuse, leaked
    // objects, etc. It prints detailed messages to the VS Output window.
    // Overhead is significant (can halve frame rate), so Debug-only.
    //
    // CRITICAL: EnableDebugLayer() must be called BEFORE any D3D12 or DXGI
    // object is created; enabling it afterwards has no effect and the runtime
    // will warn. That’s why it is literally the first thing we do here.
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif

    // ---- DXGI factory ----
    // DXGI (DirectX Graphics Infrastructure) is the layer between D3D and the
    // OS display system. IDXGIFactory lets us enumerate GPUs and create swap
    // chains. DXGI_CREATE_FACTORY_DEBUG enables the DXGI validation layer
    // alongside the D3D12 layer for richer diagnostics in Debug builds.
    ThrowIfFailed(CreateDXGIFactory2(
#if defined(_DEBUG)
        DXGI_CREATE_FACTORY_DEBUG,
#else
        0,
#endif
        IID_PPV_ARGS(&m_factory)));

    // ---- Adapter enumeration ----
    // An "adapter" is a physical GPU (or software rasteriser). We call
    // EnumAdapterByGpuPreference(HIGH_PERFORMANCE) so that on a laptop with
    // both integrated and discrete GPUs the discrete one comes first.
    //
    // DXGI_ADAPTER_FLAG_SOFTWARE marks the WARP software rasteriser (a CPU
    // fallback). We skip it — useful for testing but not production rendering.
    //
    // D3D12CreateDevice attempts to create a D3D12 device on the selected
    // adapter at the requested *minimum* feature level. D3D_FEATURE_LEVEL_11_0
    // is the baseline for all D3D12-capable hardware, so specifying it ensures
    // the broadest compatibility. The actual hardware may support much higher
    // levels (12_0, 12_1, 12_2); the feature level is a floor, not a ceiling.
    ComPtr<IDXGIAdapter1> adapter;
    for (uint32_t i = 0;
         m_factory->EnumAdapterByGpuPreference(i,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue; // skip WARP

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
                D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
        {
            std::wstring msg = std::wstring(L"[DX12] Using adapter: ") + desc.Description
                + L"  VRAM: " + std::to_wstring(desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB\n";
            OutputDebugStringW(msg.c_str());
            break;
        }
    }

    if (!m_device)
        throw std::runtime_error("No D3D12-capable hardware adapter found.");
}

void DX12GameRenderer::CreateCommandObjects()
{
    // ---- Command queue ----
    // The command queue is the GPU’s work intake. TYPE_DIRECT means it accepts
    // any combination of graphics, compute, and copy commands. Calling
    // ExecuteCommandLists() pushes closed command lists onto the queue; the
    // GPU processes them in submission order. The CPU does not stall — work
    // is consumed asynchronously by the hardware.
    //
    // Multiple queues allow overlapping execution (e.g. a dedicated COPY queue
    // for streaming assets while a DIRECT queue renders), but that is beyond
    // the scope of this renderer.
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_commandQueue)));

    // ---- Command allocators (one per back-buffer slot) ----
    // A command allocator is the raw GPU-side memory that stores the recorded
    // command stream. You MUST NOT Reset() an allocator while the GPU is still
    // executing commands recorded against it — doing so would corrupt in-flight
    // work. By keeping one allocator per swap-chain slot we can safely reset
    // slot N’s allocator once the GPU finishes frame N (verified via the fence
    // in BeginFrame), even while the GPU concurrently executes frame N+1 via
    // slot N+1’s allocator.
    for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])));
    }

    // ---- Command list ----
    // CreateCommandList() opens the list in the recording state backed by
    // allocator[0]. We immediately Close() it so it enters the closed state.
    // Each frame BeginFrame() will Reset() it onto the current slot’s allocator
    // before recording begins. We reuse a single command list across frames
    // because we always wait for the GPU to finish a slot before resetting it —
    // there is no risk of simultaneously recording into a list the GPU consumes.
    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[0].Get(), nullptr,
        IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close()); // closed until BeginFrame
}

// ---------------------------------------------------------------------------
// CreateSwapChain
//
// The swap chain manages the set of back-buffers that alternate between being
// rendered into and being displayed on screen. We use FLIP_DISCARD with two
// buffers (double-buffering): while the GPU renders into buffer N the display
// is scanning out buffer N-1. When Present() is called the buffers are
// "flipped" — the rendered buffer becomes the one on screen and the previously
// displayed one is recycled for the next frame.
//
// DXGI_SWAP_EFFECT_FLIP_DISCARD is the modern path (Windows 10+). It is
// required for features like variable refresh rate and HDR.
// ---------------------------------------------------------------------------
void DX12GameRenderer::CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height)
{
    // ---- Tearing support check ----
    // "Tearing" means presenting a frame without waiting for the vertical blank
    // (VSync). On displays with variable refresh rate (FreeSync / G-Sync) this
    // lets the GPU push frames at its natural rate with minimal latency.
    // Without it, even Present(0, 0) can be held back by the Desktop Window
    // Manager (DWM) compositor to the monitor’s fixed refresh rate.
    // IDXGIFactory5 is needed for CheckFeatureSupport; we reach it via COM QI.
    {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(m_factory.As(&factory5)))
        {
            BOOL allowed = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof(allowed))))
                m_tearingSupported = (allowed == TRUE);
        }
    }

    // ---- Swap chain descriptor ----
    // DXGI_FORMAT_R8G8B8A8_UNORM   : 32-bit RGBA, 8 bits per channel [0,1].
    //                                 The most broadly compatible SDR format.
    // SampleDesc { 1, 0 }          : No MSAA. FLIP_DISCARD swap chains do not
    //                                 support MSAA on the back-buffer; render to
    //                                 an off-screen MSAA surface and resolve.
    // DXGI_USAGE_RENDER_TARGET_OUTPUT : Declares back-buffers as render targets
    //                                   (required for CreateRenderTargetView).
    // DXGI_SWAP_EFFECT_FLIP_DISCARD  : The modern D3D12 swap model (Win10+).
    //                                   Contents are discarded after Present —
    //                                   do not rely on them persisting. Required
    //                                   for variable refresh rate and HDR.
    // ALLOW_TEARING must be declared at creation; it cannot be added later.
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width       = width;
    scDesc.Height      = height;
    scDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc  = { 1, 0 };
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = FRAME_COUNT;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
                 | (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

    // CreateSwapChainForHwnd binds the swap chain to the Win32 window and the
    // command queue. DXGI uses the queue internally to insert present signals.
    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &sc1));

    // Suppress the built-in Alt+Enter fullscreen shortcut. Fullscreen
    // transitions require careful swap-chain resizing; we suppress the default
    // behaviour and handle it manually if needed later.
    ThrowIfFailed(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

    // QI to IDXGISwapChain3 to gain GetCurrentBackBufferIndex().
    ThrowIfFailed(sc1.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// ---------------------------------------------------------------------------
// CreateRTVHeap
//
// In D3D12 you do not bind resources directly; instead you create "views" —
// lightweight descriptors that describe how the GPU should interpret a resource.
// A Render Target View (RTV) tells the Output Merger stage how to write colour
// data into a texture (format, array slice, mip level, etc.).
//
// Descriptors live in descriptor heaps — contiguous blocks of GPU-visible
// memory. We allocate one RTV heap with exactly FRAME_COUNT slots, one slot per
// back-buffer. The heap type is RTV (not shader-visible) because the OM stage
// addresses RTVs through a CPU-side handle, not through a shader root argument.
// ---------------------------------------------------------------------------
void DX12GameRenderer::CreateRTVHeap()
{
    // Allocate a descriptor heap with exactly FRAME_COUNT slots (one RTV per
    // back-buffer). In D3D12, descriptors (views) live in heaps — contiguous
    // blocks of GPU memory. You do not bind resources directly; you bind
    // descriptors that describe how to interpret those resources.
    //
    // D3D12_DESCRIPTOR_HEAP_TYPE_RTV   : This heap holds only RTVs.
    // D3D12_DESCRIPTOR_HEAP_FLAG_NONE  : CPU-visible only (not shader-visible).
    //   RTVs are addressed via CPU handles passed to OMSetRenderTargets; they
    //   do not go through the shader root signature / descriptor table system,
    //   so they do not need the SHADER_VISIBLE flag.
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = FRAME_COUNT;
    heapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap)));

    // Query and cache the descriptor stride. The size of a single RTV
    // descriptor is determined by the GPU vendor/driver and can vary. Always
    // ask the device — never hard-code a value like 32 or 64 bytes.
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

void DX12GameRenderer::CreateRenderTargets()
{
    // For each back-buffer slot, retrieve its ID3D12Resource from the swap chain
    // and write an RTV descriptor for it into the heap:
    //
    //   GetBuffer(i, …)
    //     Retrieves the D3D12 texture resource the swap chain owns for slot i.
    //     We store a ComPtr to it so we can later insert resource barriers
    //     (PRESENT ↔ RENDER_TARGET) around each frame.
    //
    //   CreateRenderTargetView(resource, nullptr, handle)
    //     Writes an RTV descriptor at the given CPU heap address. nullptr for
    //     the view desc means “use the resource’s own format and the first mip
    //     and array slice” — correct for a plain 2D swap-chain back-buffer.
    //     After this call the heap slot holds a valid descriptor that
    //     OMSetRenderTargets can use.
    //
    //   rtvHandle.ptr += m_rtvDescriptorSize
    //     Advance the CPU address to the next heap slot for the next iteration.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

void DX12GameRenderer::FlushGPU()
{
    // A “full flush” blocks the CPU until the GPU has finished ALL work currently
    // in the command queue. Useful before destructors run (dtor) or before
    // ResizeBuffers() (Resize), where we need to know no GPU work touches the
    // back-buffers or command allocators we are about to destroy or resize.
    //
    // How it works:
    //   1. Increment m_fenceValue to get a target that hasn’t been signalled.
    //   2. Queue::Signal() enqueues a GPU write of ‘value’ into the fence once
    //      the queue drains — the CPU does not block here.
    //   3. If GetCompletedValue() already equals ‘value’, the queue was empty
    //      and we skip the wait. Otherwise SetEventOnCompletion + 
    //      WaitForSingleObject sleeps the CPU thread until the GPU signals.
    //   4. Re-query the current back-buffer index. After Resize() calls
    //      ResizeBuffers() the swap chain may assign a new index; refreshing
    //      m_frameIndex here keeps it consistent.
    const uint64_t value = ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), value));
    if (m_fence->GetCompletedValue() < value)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}
