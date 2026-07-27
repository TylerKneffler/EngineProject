#include "pch.h"
#include "DX12GraphicsTexture.h"

#include <cstring>

namespace
{
D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}
}

D3D12TextureFactory::D3D12TextureFactory(
    ID3D12Device* device,
    ID3D12CommandQueue* queue)
    : m_device(device), m_queue(queue)
{
    if (!m_device)
        return;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kMaxTextures;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap))))
        m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(desc.Type);
}

std::shared_ptr<IGraphicsTexture> D3D12TextureFactory::CreateTexture2D(
    uint32_t width,
    uint32_t height,
    const uint8_t* rgbaPixels)
{
    if (!m_device || !m_queue || !m_heap || !width || !height || !rgbaPixels ||
        m_nextDescriptor >= kMaxTextures)
        return nullptr;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(m_device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
        return nullptr;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 rowBytes = 0;
    UINT64 uploadSize = 0;
    m_device->GetCopyableFootprints(
        &textureDesc, 0, 1, 0, &footprint, &rowCount, &rowBytes, &uploadSize);

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(m_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        return nullptr;

    uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return nullptr;
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(
            mapped + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
            rgbaPixels + static_cast<size_t>(row) * width * 4,
            static_cast<size_t>(width) * 4);
    upload->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&commands))))
        return nullptr;

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &barrier);
    if (FAILED(commands->Close()))
        return nullptr;
    ID3D12CommandList* lists[] = { commands.Get() };
    m_queue->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        FAILED(m_queue->Signal(fence.Get(), 1)))
        return nullptr;
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle)
        return nullptr;
    if (fence->GetCompletedValue() < 1)
    {
        if (FAILED(fence->SetEventOnCompletion(1, eventHandle)))
        {
            CloseHandle(eventHandle);
            return nullptr;
        }
        WaitForSingleObject(eventHandle, INFINITE);
    }
    CloseHandle(eventHandle);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(m_nextDescriptor) * m_descriptorSize;
    gpu.ptr += static_cast<UINT64>(m_nextDescriptor) * m_descriptorSize;
    ++m_nextDescriptor;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(texture.Get(), &srv, cpu);
    return std::make_shared<D3D12GraphicsTexture>(
        std::move(texture), m_heap, gpu);
}
