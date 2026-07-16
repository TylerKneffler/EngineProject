#pragma once
#include "../IView.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <functional>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// D3D12View — Direct3D 12 implementation of IView
//
// Owns offscreen render target and depth buffer resources for editor views.
// Creates DirectX 12 textures, descriptor heaps, and manages transitions.
// ---------------------------------------------------------------------------
class D3D12View : public IView
{
public:
    D3D12View() = default;
    virtual ~D3D12View() = default;

    void Init(void* device,
              uint32_t width, uint32_t height,
              void* srvCpu, void* srvGpu,
              uint32_t srvSlotIndex = 0) override;

    void Resize(void* device, uint32_t width, uint32_t height) override;

    void Render(void* cmdList, void* mainRtv,
                std::function<void(void*)> drawFn = nullptr) override;

    float    GetAspect() const override { return m_aspect; }
    uint32_t GetWidth()  const override { return m_width;  }
    uint32_t GetHeight() const override { return m_height; }
    void* GetUiTextureHandle() const override { return reinterpret_cast<void*>(m_srvGpu.ptr); }

    uint32_t GetSrvSlotIndex() const override { return m_srvSlotIndex; }

    // Access the GPU descriptor used by the selected UI package.
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpu() const { return m_srvGpu; }

private:
    void CreateResources(void* device, uint32_t width, uint32_t height);

    ComPtr<ID3D12Resource>       m_texture;     // offscreen colour render target / SRV
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;     // 1-slot non-shader-visible RTV heap
    ComPtr<ID3D12Resource>       m_depthBuffer; // depth buffer for 3-D rendering
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;     // 1-slot non-shader-visible DSV heap

    D3D12_CPU_DESCRIPTOR_HANDLE  m_srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE  m_srvGpu{};

    uint32_t m_srvSlotIndex = 0;
    uint32_t m_width  = 0;
    uint32_t m_height = 0;
    float    m_aspect = 1.0f;
};
