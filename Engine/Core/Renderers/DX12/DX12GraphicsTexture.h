#pragma once

#include "Core/Graphics/IGraphicsTexture.h"

#include <d3d12.h>
#include <cstdint>
#include <wrl/client.h>
#include <functional>
#include <mutex>
#include <vector>

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
    ~D3D12GraphicsTexture() override { if (m_releaseDescriptor) m_releaseDescriptor(); }
    bool IsUploadComplete() const override { return !m_uploadFence || m_uploadFence->GetCompletedValue() >= 1; }
    uint64_t GetAllocationSizeBytes() const override { return m_allocationBytes; }
    void SetUpload(Microsoft::WRL::ComPtr<ID3D12Fence> fence, uint64_t bytes)
    { m_uploadFence = std::move(fence); m_allocationBytes = bytes; }
    void SetDescriptorRelease(std::function<void()> release) { m_releaseDescriptor = std::move(release); }

    void* GetNativeHandle() const override { return m_resource.Get(); }
    void* GetUiHandle() const override
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(m_handle.ptr));
    }
    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() const { return m_handle; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHandle() const
    { return m_dsvHeap ? m_dsvHeap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{}; }
    bool IsDepthTarget() const { return m_dsvHeap != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> m_uploadFence;
    uint64_t m_allocationBytes = 0;
    std::function<void()> m_releaseDescriptor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_GPU_DESCRIPTOR_HANDLE m_handle{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
};

class D3D12TextureFactory final : public Engine::Graphics::IGraphicsTextureFactory
{
public:
    ~D3D12TextureFactory() override;
    void PollUploads() override;
    uint64_t GetPendingUploadBytes() const override;
    uint64_t EstimateAllocationBytes(uint32_t, uint32_t, uint32_t,
        Engine::Graphics::GraphicsTextureFormat, bool = true) const override;
    D3D12TextureFactory(ID3D12Device* device, ID3D12CommandQueue* queue);
    D3D12TextureFactory(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sharedHeap,
        uint32_t firstDescriptor,
        uint32_t descriptorCount);
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateTexture2D(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels,
        uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format,
        bool srgb = true) override;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateTexture2DArray(
        uint32_t width, uint32_t height, uint32_t layers, const uint8_t* pixels,
        uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format,
        bool srgb = true) override;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateDepthTexture2D(
        uint32_t width, uint32_t height) override;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> GetNullTexture();
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> GetNullArrayTexture();

private:
    struct DescriptorSlots { std::mutex mutex; std::vector<uint32_t> free; };
    std::shared_ptr<DescriptorSlots> m_slots = std::make_shared<DescriptorSlots>();
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> Create(
        uint32_t width, uint32_t height, uint32_t layers, bool asArray,
        const uint8_t* pixels, uint32_t mipLevels,
        Engine::Graphics::GraphicsTextureFormat format, bool srgb);
    uint32_t AllocateDescriptor();
    void OwnDescriptor(const std::shared_ptr<D3D12GraphicsTexture>&, uint32_t);
    struct PendingUpload
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> staging, texture;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t bytes = 0;
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> owner;
    };
    std::vector<PendingUpload> m_uploads;
    static constexpr uint32_t kMaxTextures = 1024;
    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    uint32_t m_descriptorSize = 0;
    uint32_t m_firstDescriptor = 0;
    uint32_t m_descriptorCount = 0;
    uint32_t m_nextDescriptor = 0;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_nullTexture;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_nullArrayTexture;
};
}
