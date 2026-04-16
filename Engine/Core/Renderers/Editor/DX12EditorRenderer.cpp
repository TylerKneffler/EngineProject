#include "DX12EditorRenderer.h"

// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------
DX12EditorRenderer::~DX12EditorRenderer()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    FlushGPU();
    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void DX12EditorRenderer::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    CreateDevice();
    CreateCommandObjects();
    CreateSwapChain(hwnd, width, height);
    CreateRTVHeap();
    CreateRenderTargets();

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence)));

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("Failed to create fence event.");
    
    // SRV heap: slot 0 = ImGui font atlas; slots 1-31 available for view textures.
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.NumDescriptors = 32; // enough headroom for many simultaneous view panels
    srvDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo dxInitInfo{};
    dxInitInfo.Device            = m_device.Get();
    dxInitInfo.CommandQueue      = m_commandQueue.Get();  // required for font texture upload
    dxInitInfo.NumFramesInFlight = static_cast<int>(FRAME_COUNT);
    dxInitInfo.RTVFormat         = DXGI_FORMAT_R8G8B8A8_UNORM;
    dxInitInfo.DSVFormat         = DXGI_FORMAT_UNKNOWN;
    dxInitInfo.SrvDescriptorHeap = m_srvHeap.Get();
    dxInitInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu)
    {
        *out_cpu = info->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        *out_gpu = info->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    };
    dxInitInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
        D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
    ImGui_ImplDX12_Init(&dxInitInfo);

    m_viewport    = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
    m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void DX12EditorRenderer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    FlushGPU();

    for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    {
        m_renderTargets[i].Reset();
        m_fenceValues[i] = m_fenceValues[m_frameIndex];
    }

    // The editor swap chain does not carry the ALLOW_TEARING flag — it was
    // never set at creation — so it must not be passed to ResizeBuffers either.
    ThrowIfFailed(m_swapChain->ResizeBuffers(
        FRAME_COUNT, width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_width  = width;
    m_height = height;

    CreateRenderTargets();

    m_viewport    = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
    m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
}

// ---------------------------------------------------------------------------
// RenderIfNeeded — the primary public render entry point
//
// Only submits GPU work when the dirty flag is set. drawFn (if provided) is
// called between BeginFrame and EndFrame so the caller can issue draw commands.
// If drawFn is nullptr the frame is cleared to a neutral editor grey.
// ---------------------------------------------------------------------------
void DX12EditorRenderer::RenderIfNeeded(std::function<void()> drawFn)
{
    if (!m_dirty)
        return;
    m_dirty = false;

    BeginFrame();

    if (drawFn)
        drawFn();
    else
        Clear(0.18f, 0.18f, 0.18f); // default: neutral dark-grey editor background

    EndFrame();
}

// ---------------------------------------------------------------------------
// BeginFrame
// ---------------------------------------------------------------------------
void DX12EditorRenderer::BeginFrame()
{
    // Wait for the GPU to finish with this slot's allocator.
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(
            m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(
        m_commandAllocators[m_frameIndex].Get(), nullptr));

    // Transition back-buffer: PRESENT → RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetCurrentRTV();
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------
void DX12EditorRenderer::Clear(float r, float g, float b, float a)
{
    const float color[4] = { r, g, b, a };
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetCurrentRTV();
    m_commandList->ClearRenderTargetView(rtv, color, 0, nullptr);
}

// ---------------------------------------------------------------------------
// EndFrame
//
// Transitions the back-buffer to PRESENT state before submitting, then calls
// Present(1, 0) — vsync, no tearing. The editor does not need to exceed the
// monitor refresh rate and vsync avoids tearing at no meaningful cost.
// ---------------------------------------------------------------------------
void DX12EditorRenderer::EndFrame()
{
    // Transition back-buffer: RENDER_TARGET → PRESENT
    // Unlike DX12GameRenderer (which used D3D11On12 to issue this barrier),
    // the editor renderer handles it directly here.
    ImGui::Render();
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());

    // Update and render additional platform windows (viewports outside the main window).
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    // Transition back-buffer: RENDER_TARGET → PRESENT
    // Must happen after ImGui has finished drawing to the render target.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // SyncInterval = 1: wait for one vblank before presenting.
    // PresentFlags = 0: no tearing; the editor does not need uncapped frames.
    ThrowIfFailed(m_swapChain->Present(1, 0));

    m_fenceValues[m_frameIndex] = ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// ---------------------------------------------------------------------------
// GetCurrentRTV
// ---------------------------------------------------------------------------
D3D12_CPU_DESCRIPTOR_HANDLE DX12EditorRenderer::GetCurrentRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    return handle;
}

// ---------------------------------------------------------------------------
// GetSrvSlot
// ---------------------------------------------------------------------------
std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE>
DX12EditorRenderer::GetSrvSlot(uint32_t slot) const
{
    const uint32_t stride = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * stride;

    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot) * stride;

    return { cpu, gpu };
}

// ---------------------------------------------------------------------------
// CreateDevice
// ---------------------------------------------------------------------------
void DX12EditorRenderer::CreateDevice()
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif

    ThrowIfFailed(CreateDXGIFactory2(
#if defined(_DEBUG)
        DXGI_CREATE_FACTORY_DEBUG,
#else
        0,
#endif
        IID_PPV_ARGS(&m_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (uint32_t i = 0;
         m_factory->EnumAdapterByGpuPreference(i,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
                D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
        {
            std::wstring msg = std::wstring(L"[DX12Editor] Using adapter: ") + desc.Description
                + L"  VRAM: " + std::to_wstring(desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB\n";
            OutputDebugStringW(msg.c_str());
            break;
        }
    }

    if (!m_device)
        throw std::runtime_error("No D3D12-capable hardware adapter found.");
}

// ---------------------------------------------------------------------------
// CreateCommandObjects
// ---------------------------------------------------------------------------
void DX12EditorRenderer::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_commandQueue)));

    for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])));
    }

    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[0].Get(), nullptr,
        IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close());
}

// ---------------------------------------------------------------------------
// CreateSwapChain
//
// The editor swap chain omits DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING entirely.
// Tearing is desirable for uncapped game rendering but unnecessary here —
// Present(1, 0) waits for vblank and the editor never renders faster than
// the screen can display.
// ---------------------------------------------------------------------------
void DX12EditorRenderer::CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height)
{
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width       = width;
    scDesc.Height      = height;
    scDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc  = { 1, 0 };
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = FRAME_COUNT;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &sc1));

    ThrowIfFailed(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(sc1.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// ---------------------------------------------------------------------------
// CreateRTVHeap
// ---------------------------------------------------------------------------
void DX12EditorRenderer::CreateRTVHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = FRAME_COUNT;
    heapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap)));

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

// ---------------------------------------------------------------------------
// CreateRenderTargets
// ---------------------------------------------------------------------------
void DX12EditorRenderer::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

// ---------------------------------------------------------------------------
// FlushGPU
// ---------------------------------------------------------------------------
void DX12EditorRenderer::FlushGPU()
{
    const uint64_t value = ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), value));
    if (m_fence->GetCompletedValue() < value)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}
