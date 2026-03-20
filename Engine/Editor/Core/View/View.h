#pragma once
#include "pch.h"

// ---------------------------------------------------------------------------
// View — base class for all editor panel views.
//
// Owns the offscreen DX12 render target, depth buffer, and SRV registration
// that every panel needs. Subclasses override DrawPanel() to provide their
// specific ImGui content and interaction logic.
// ---------------------------------------------------------------------------
class View
{
public:
    View()          = default;
    virtual ~View() = default;

    // Creates the offscreen texture and registers it in ImGui's SRV heap.
    virtual void Init(ID3D12Device* device,
                      uint32_t width, uint32_t height,
                      D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
                      D3D12_GPU_DESCRIPTOR_HANDLE srvGpu);

    // Recreates the offscreen texture at the new dimensions.
    // Safe to call only when the GPU is idle (after renderer->Resize()).
    void Resize(ID3D12Device* device, uint32_t width, uint32_t height);

    // Transitions SRV->RTV, clears, invokes drawFn, transitions RTV->SRV,
    // then restores mainRtv for subsequent UI rendering.
    void Render(ID3D12GraphicsCommandList* cmdList,
                D3D12_CPU_DESCRIPTOR_HANDLE mainRtv,
                std::function<void(ID3D12GraphicsCommandList*)> drawFn = nullptr);

    // Draws the ImGui panel for this view. Each subclass provides its own.
    virtual void DrawPanel() = 0;

    float    GetAspect() const { return m_aspect; }
    uint32_t GetWidth()  const { return m_width;  }
    uint32_t GetHeight() const { return m_height; }

protected:
    // Allocates / reallocates all DX12 resources for the offscreen target.
    void CreateResources(ID3D12Device* device, uint32_t width, uint32_t height);

    ComPtr<ID3D12Resource>       m_texture;     // offscreen colour render target / SRV
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;     // 1-slot non-shader-visible RTV heap
    ComPtr<ID3D12Resource>       m_depthBuffer; // depth buffer for 3-D rendering
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;     // 1-slot non-shader-visible DSV heap

    D3D12_CPU_DESCRIPTOR_HANDLE  m_srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE  m_srvGpu{};

    uint32_t m_width  = 0;
    uint32_t m_height = 0;
    float    m_aspect = 1.0f;
};