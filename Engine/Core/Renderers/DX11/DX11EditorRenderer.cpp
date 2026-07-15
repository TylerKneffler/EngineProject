#include "pch.h"
#include "DX11EditorRenderer.h"
#include "D3D11View.h"
#include "imgui_impl_dx11.h"
#include <algorithm>

DX11EditorRenderer::~DX11EditorRenderer()
{
    if (m_imguiInitialized)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_context) m_context->ClearState();
}

bool DX11EditorRenderer::Init(void* hwndHandle, uint32_t width, uint32_t height)
{
    try
    {
        HWND hwnd = static_cast<HWND>(hwndHandle);
        DXGI_SWAP_CHAIN_DESC swap{};
        swap.BufferDesc.Width = width;
        swap.BufferDesc.Height = height;
        swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap.SampleDesc.Count = 1;
        swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap.BufferCount = 2;
        swap.OutputWindow = hwnd;
        swap.Windowed = TRUE;
        swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

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
        ThrowIfFailed(hr);
        m_width = width;
        m_height = height;
        CreateRenderTarget();
        m_graphicsProvider = std::make_unique<D3D11GraphicsProvider>(m_device.Get(), m_context.Get());

        for (uint32_t slot = MAX_SRV_SLOTS - 1; slot > 0; --slot)
            m_freeSrvSlots.push_back(slot);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        if (!ImGui_ImplWin32_Init(hwnd))
            throw std::runtime_error("Failed to initialize the ImGui Win32 backend");
        if (!ImGui_ImplDX11_Init(m_device.Get(), m_context.Get()))
            throw std::runtime_error("Failed to initialize ImGui for Direct3D 11");
        m_imguiInitialized = true;
        return true;
    }
    catch (const std::exception& error)
    {
        if (ImGui::GetCurrentContext())
        {
            if (ImGui::GetIO().BackendRendererUserData)
                ImGui_ImplDX11_Shutdown();
            if (ImGui::GetIO().BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        OutputDebugStringA((std::string("DX11 editor initialization failed: ") + error.what() + "\n").c_str());
        return false;
    }
}

void DX11EditorRenderer::CreateRenderTarget()
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    ThrowIfFailed(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv));
}

void DX11EditorRenderer::Resize(uint32_t width, uint32_t height)
{
    if (!m_swapChain || width == 0 || height == 0 || (width == m_width && height == m_height)) return;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.Reset();
    ThrowIfFailed(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
    m_width = width;
    m_height = height;
    CreateRenderTarget();
    m_dirty = true;
}

void DX11EditorRenderer::Clear(float r, float g, float b, float a)
{
    const float color[4] = { r, g, b, a };
    m_context->ClearRenderTargetView(m_rtv.Get(), color);
}

void DX11EditorRenderer::RenderIfNeeded(std::function<void()> drawFn)
{
    if (!m_dirty || !m_rtv) return;
    m_dirty = false;
    ID3D11RenderTargetView* target = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_context->RSSetViewports(1, &viewport);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (drawFn) drawFn(); else Clear(0.18f, 0.18f, 0.18f);

    ImGui::Render();
    target = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    ThrowIfFailed(m_swapChain->Present(1, 0));
}

std::pair<std::pair<void*, void*>, uint32_t> DX11EditorRenderer::AllocateSrvSlot()
{
    if (m_freeSrvSlots.empty())
        throw std::runtime_error("No available editor view slots");
    uint32_t slot = m_freeSrvSlots.back();
    m_freeSrvSlots.pop_back();
    return { { nullptr, nullptr }, slot };
}

void DX11EditorRenderer::FreeSrvSlot(uint32_t slot)
{
    if (slot == 0 || slot >= MAX_SRV_SLOTS) return;
    if (std::find(m_freeSrvSlots.begin(), m_freeSrvSlots.end(), slot) == m_freeSrvSlots.end())
        m_freeSrvSlots.push_back(slot);
}

std::unique_ptr<IView> DX11EditorRenderer::CreateViewBackend()
{
    return std::make_unique<D3D11View>();
}
