#include "DX12EditorRenderer.h"
#include "D3D12View.h"
#include "DX12DredDiagnostics.h"
#include "Core/Renderers/GraphicsDeviceName.h"
#include <d3d12sdklayers.h>
#include <fstream>
#include <sstream>


namespace Engine::Renderers
{
// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------
DX12EditorRenderer::~DX12EditorRenderer()
{
    FlushGPU();
    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool DX12EditorRenderer::Init(void* hwnd, uint32_t width, uint32_t height)
{
    try
    {
        m_width  = width;
        m_height = height;

        CreateDevice();
        CreateCommandObjects();
        CreateSwapChain(static_cast<HWND>(hwnd), width, height);
        CreateRTVHeap();
        CreateRenderTargets();

        // Object materials use b0 plus five glTF texture slots (t0-t4).
        m_rootSignature = CreateD3D12MaterialRootSignature(m_device.Get());

        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_fence)));

        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent)
            throw std::runtime_error("Failed to create fence event.");

        // SRV heap layout:
        //   slot 0   : ImGui font/default UI descriptor
        //   slots 1+ : editor view slots (Scene/Game/etc.)
        //   tail     : shared editor texture previews (ImGui image widgets)
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.NumDescriptors = IEditorRenderer::MAX_SRV_SLOTS + UI_TEXTURE_SRV_SLOTS;
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)));

        // ---- Graphics provider ----
        // Use the shared editor SRV heap tail for texture previews so ImGui can
        // safely sample them through the same bound descriptor heap.
        m_graphicsProvider = std::make_unique<D3D12GraphicsProvider>(
            m_device.Get(), m_commandQueue.Get(), m_rootSignature.Get(), m_srvHeap,
            IEditorRenderer::MAX_SRV_SLOTS, UI_TEXTURE_SRV_SLOTS);

        // Initialize free view slots; slot 0 remains reserved for the UI package.
        for (uint32_t i = IEditorRenderer::MAX_SRV_SLOTS - 1; i > 0; --i)
            m_freeSrvSlots.push_back(i);

        m_viewport = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
        m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

        return true;
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA("Init failed: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
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

    // Preserve the tearing flag selected when the swap chain was created.
    ThrowIfFailed(m_swapChain->ResizeBuffers(
        FRAME_COUNT, width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
            (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u)));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_width  = width;
    m_height = height;

    CreateRenderTargets();

    m_viewport    = { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
    m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
}

// ---------------------------------------------------------------------------
// GetGraphicsProvider
// ---------------------------------------------------------------------------
Engine::Graphics::IGraphicsProvider* DX12EditorRenderer::GetGraphicsProvider()
{
    return m_graphicsProvider.get();
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
    m_lastPresentMilliseconds = 0.0;
    m_lastQueueSubmissionMilliseconds = 0.0;
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

    if (m_graphicsProvider)
        m_graphicsProvider->GetContextFactory()->PrepareFrame(m_frameIndex);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(
        m_commandAllocators[m_frameIndex].Get(), nullptr));
    if (m_graphicsProvider)
        m_graphicsProvider->GetContextFactory()->SetCommandBuffer(m_commandList.Get());

    // Set the root signature immediately after reset (required for D3D12)
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Transition back-buffer: PRESENT -> RENDER_TARGET
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

    m_currentRTVHandle = GetCurrentRTV();
    if (m_uiHooks.beginFrame) m_uiHooks.beginFrame();
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
// Present without VSync so the editor frame limit controls pacing.
// ---------------------------------------------------------------------------
void DX12EditorRenderer::EndFrame()
{
    // Transition back-buffer: RENDER_TARGET → PRESENT
    // Unlike DX12GameRenderer (which used D3D11On12 to issue this barrier),
    // the editor renderer handles it directly here.
    if (m_uiHooks.render) m_uiHooks.render(m_commandList.Get());
    if (m_uiHooks.endFrame) m_uiHooks.endFrame();

    // Transition back-buffer: RENDER_TARGET → PRESENT
    // Must happen after the UI package has finished drawing.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // Resolve this slot's timestamps on the same command list that recorded
    // them. PrepareFrame reads the results after the slot fence completes.
    if (m_graphicsProvider)
        m_graphicsProvider->GetContextFactory()->FinalizeFrame();
    const auto checkSubmission = [this](HRESULT hr, const char* operation)
    {
        if (SUCCEEDED(hr)) return;
        std::ostringstream message;
        message << operation << " failed: HRESULT=0x" << std::hex
            << static_cast<unsigned long>(hr) << ", deviceRemovedReason=0x"
            << static_cast<unsigned long>(m_device->GetDeviceRemovedReason());
        ComPtr<ID3D12InfoQueue> queue;
        if (SUCCEEDED(m_device.As(&queue)))
        {
            const UINT64 count = queue->GetNumStoredMessages();
            const UINT64 first = count > 64 ? count - 64 : 0;
            for (UINT64 index = first; index < count; ++index)
            {
                SIZE_T bytes = 0;
                if (FAILED(queue->GetMessage(index, nullptr, &bytes)) || !bytes) continue;
                std::vector<unsigned char> storage(bytes);
                auto* entry = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(queue->GetMessage(index, entry, &bytes)))
                    message << '\n' << entry->pDescription;
            }
        }
        AppendDredDiagnostics(m_device.Get(), message);
        std::ofstream log("dx12-frame-error.log", std::ios::app);
        if (log) log << message.str() << std::endl;
        OutputDebugStringA((message.str() + "\n").c_str());
        throw std::runtime_error(message.str());
    };
    checkSubmission(m_commandList->Close(), "Command list Close");

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    const auto submissionStart = std::chrono::steady_clock::now();
    m_commandQueue->ExecuteCommandLists(1, lists);
    m_lastQueueSubmissionMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - submissionStart).count();
    ++m_submittedFrameCount;

    // Present without a vblank wait; allow tearing when the swap chain supports it.
    const auto presentStart = std::chrono::steady_clock::now();
    checkSubmission(m_swapChain->Present(0,
        m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0u), "Present");
    m_lastPresentMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - presentStart).count();

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
// GetCurrentRenderTargetHandle
// ---------------------------------------------------------------------------
void* DX12EditorRenderer::GetCurrentRenderTargetHandle() const
{
    // m_currentRTVHandle is updated in BeginFrame; return stable pointer to it
    return reinterpret_cast<void*>(const_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(&m_currentRTVHandle));
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
// AllocateSrvSlot
// ---------------------------------------------------------------------------
std::pair<std::pair<void*, void*>, uint32_t> DX12EditorRenderer::AllocateSrvSlot()
{
    if (m_freeSrvSlots.empty())
        throw std::runtime_error("No available SRV slots in editor renderer.");

    uint32_t slot = m_freeSrvSlots.back();
    m_freeSrvSlots.pop_back();

    auto [cpu, gpu] = GetSrvSlot(slot);
    return { { reinterpret_cast<void*>(cpu.ptr), reinterpret_cast<void*>(gpu.ptr) }, slot };
}

// ---------------------------------------------------------------------------
// FreeSrvSlot
// ---------------------------------------------------------------------------
void DX12EditorRenderer::FreeSrvSlot(uint32_t slotIndex)
{
    if (slotIndex == 0)
        return;  // Slot 0 is reserved for the selected UI package.

    // Add back to free list
    m_freeSrvSlots.push_back(slotIndex);
}

// ---------------------------------------------------------------------------
// CanAllocateSrvSlot
// ---------------------------------------------------------------------------
bool DX12EditorRenderer::CanAllocateSrvSlot() const
{
    return !m_freeSrvSlots.empty();
}

std::unique_ptr<IView> DX12EditorRenderer::CreateViewBackend()
{
    return std::make_unique<D3D12View>();
}

// ---------------------------------------------------------------------------
// GetAvailableSrvSlots
// ---------------------------------------------------------------------------
uint32_t DX12EditorRenderer::GetAvailableSrvSlots() const
{
    return static_cast<uint32_t>(m_freeSrvSlots.size());
}

// ---------------------------------------------------------------------------
// CreateDevice
// ---------------------------------------------------------------------------
void DX12EditorRenderer::CreateDevice()
{
    EnableDredDiagnostics();
    bool debugLayerActive = false;
    if (IsGraphicsDebugLayerEnabled())
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            debugLayerActive = true;
        }
    }

    ThrowIfFailed(CreateDXGIFactory2(
        debugLayerActive ? DXGI_CREATE_FACTORY_DEBUG : 0u,
        IID_PPV_ARGS(&m_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    bool selectedDeviceFound = m_graphicsDevice.empty() || m_graphicsDevice == "Auto";
    for (uint32_t i = 0;
         m_factory->EnumAdapterByGpuPreference(i,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (!selectedDeviceFound)
        {
            const int selectedLength = MultiByteToWideChar(CP_UTF8, 0, m_graphicsDevice.c_str(), -1,
                nullptr, 0);
            std::wstring selectedName(static_cast<size_t>(selectedLength > 0 ? selectedLength : 1), L'\0');
            if (selectedLength > 0)
                MultiByteToWideChar(CP_UTF8, 0, m_graphicsDevice.c_str(), -1,
                    selectedName.data(), selectedLength);
            if (!selectedName.empty() && selectedName.back() == L'\0') selectedName.pop_back();
            if (desc.Description != selectedName) continue;
            selectedDeviceFound = true;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
                D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
        {
            m_actualGraphicsDeviceName = Details::GraphicsDeviceNameFromWide(desc.Description);
            std::wstring msg = std::wstring(L"[DX12Editor] Using adapter: ") + desc.Description
                + L"  VRAM: " + std::to_wstring(desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB\n";
            OutputDebugStringW(msg.c_str());
            break;
        }
    }

    if (!m_device)
        throw std::runtime_error(selectedDeviceFound
            ? "No D3D12-capable hardware adapter found."
            : "Selected graphics device is no longer available: " + m_graphicsDevice);
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
// Allow tearing when supported so the optional frame limit can exceed refresh.
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
    ComPtr<IDXGIFactory5> factory5;
    BOOL allowed = FALSE;
    m_tearingSupported = SUCCEEDED(m_factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed,
            sizeof(allowed))) && allowed;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
        (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

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

void DX12EditorRenderer::WaitForFrame(uint64_t token)
{
    if (IsFrameComplete(token)) return;
    ThrowIfFailed(m_fence->SetEventOnCompletion(token, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
}
}
