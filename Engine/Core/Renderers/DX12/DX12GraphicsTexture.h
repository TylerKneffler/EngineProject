#pragma once

#include "Core/Graphics/IGraphicsTexture.h"

#include <d3d12.h>
#include <wrl/client.h>

namespace Engine::Renderers
{
class D3D12GraphicsTexture final : public Engine::Graphics::IGraphicsTexture
{
public:
    D3D12GraphicsTexture(
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap,
        D3D12_GPU_DESCRIPTOR_HANDLE handle,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap = {})
        : m_resource(std::move(resource)), m_heap(std::move(heap)),
          m_handle(handle), m_dsvHeap(std::move(dsvHeap)) {}

    void* GetNativeHandle() const override { return m_resource.Get(); }
    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() const { return m_handle; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHandle() const
    { return m_dsvHeap ? m_dsvHeap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{}; }
    bool IsDepthTarget() const { return m_dsvHeap != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_GPU_DESCRIPTOR_HANDLE m_handle{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
};

class D3D12TextureFactory final : public Engine::Graphics::IGraphicsTextureFactory
{
public:
    D3D12TextureFactory(ID3D12Device* device, ID3D12CommandQueue* queue);
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateTexture2D(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels,
        uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format,
        bool srgb = true) override;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateDepthTexture2D(
        uint32_t width, uint32_t height) override;

private:
    static constexpr uint32_t kMaxTextures = 1024;
    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    uint32_t m_descriptorSize = 0;
    uint32_t m_nextDescriptor = 0;
};
}
