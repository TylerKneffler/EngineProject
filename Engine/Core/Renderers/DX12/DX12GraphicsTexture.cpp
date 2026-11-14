#include "pch.h"
#include "DX12GraphicsTexture.h"
#include "DX12QueueMutex.h"



namespace Engine::Renderers
{
namespace
{
// Uncompressed 8-bit resources stay typeless so one resource can be viewed in
// either color space; block formats are created in their final color space.
DXGI_FORMAT D3D12TextureFormat(Engine::Graphics::GraphicsTextureFormat format, bool srgb, bool view)
{
    using Engine::Graphics::GraphicsTextureFormat;
    switch (format)
    {
    case GraphicsTextureFormat::Rgba32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case GraphicsTextureFormat::Bc1: return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case GraphicsTextureFormat::Bc4: return DXGI_FORMAT_BC4_UNORM;
    case GraphicsTextureFormat::Bc5: return DXGI_FORMAT_BC5_UNORM;
    case GraphicsTextureFormat::Bc7: return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    default:
        return !view ? DXGI_FORMAT_R8G8B8A8_TYPELESS
            : (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
    }
}
}

uint32_t D3D12TextureFactory::AllocateDescriptor()
{
    std::lock_guard lock(m_slots->mutex);
    if (!m_slots->free.empty())
    {
        auto index = m_slots->free.back(); m_slots->free.pop_back(); return index;
    }
    return m_nextDescriptor < m_descriptorCount ? m_nextDescriptor++ : UINT32_MAX;
}
void D3D12TextureFactory::OwnDescriptor(const std::shared_ptr<D3D12GraphicsTexture>& texture, uint32_t index)
{
    texture->SetDescriptorRelease([slots=m_slots,index] {
        std::lock_guard lock(slots->mutex); slots->free.push_back(index);
    });
}
void D3D12TextureFactory::PollUploads()
{
    m_uploads.erase(std::remove_if(m_uploads.begin(), m_uploads.end(),
        [](const auto& upload) { return upload.fence->GetCompletedValue() >= 1; }), m_uploads.end());
}
uint64_t D3D12TextureFactory::GetPendingUploadBytes() const
{
    uint64_t bytes = 0; for (const auto& upload : m_uploads) bytes += upload.bytes; return bytes;
}
D3D12TextureFactory::~D3D12TextureFactory()
{
    // Shutdown only. Runtime polling and texture replacement never wait.
    for (const auto& upload : m_uploads) if (upload.fence->GetCompletedValue() < 1)
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event)
        {
            if (SUCCEEDED(upload.fence->SetEventOnCompletion(1, event))) WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }
    }
}
uint64_t D3D12TextureFactory::EstimateAllocationBytes(uint32_t width, uint32_t height,
    uint32_t levels, Engine::Graphics::GraphicsTextureFormat format, bool srgb) const
{
    if (!m_device || !Engine::Graphics::GraphicsTextureIsValidBase(format, width, height)) return 0;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(levels); desc.SampleDesc.Count = 1;
    desc.Format = D3D12TextureFormat(format, srgb, false);
    return m_device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
}
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

namespace Engine::Renderers
{

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
    {
        m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(desc.Type);
        m_descriptorCount = kMaxTextures;
    }
}

D3D12TextureFactory::D3D12TextureFactory(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sharedHeap,
    uint32_t firstDescriptor,
    uint32_t descriptorCount)
    : m_device(device), m_queue(queue), m_heap(std::move(sharedHeap)),
      m_firstDescriptor(firstDescriptor), m_descriptorCount(descriptorCount)
{
    if (!m_device || !m_heap || m_descriptorCount == 0)
        return;
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D12TextureFactory::GetNullTexture()
{
    if(m_nullTexture || !m_device || !m_heap || m_nextDescriptor>=m_descriptorCount)
        return m_nullTexture;
    auto cpu=m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu=m_heap->GetGPUDescriptorHandleForHeapStart();
    const auto index=m_firstDescriptor+m_nextDescriptor++;
    cpu.ptr+=SIZE_T(index)*m_descriptorSize;
    gpu.ptr+=UINT64(index)*m_descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels=1;
    m_device->CreateShaderResourceView(nullptr,&view,cpu);
    m_nullTexture=std::make_shared<D3D12GraphicsTexture>(
        Microsoft::WRL::ComPtr<ID3D12Resource>{},m_heap,gpu);
    return m_nullTexture;
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D12TextureFactory::GetNullArrayTexture()
{
    if(m_nullArrayTexture || !m_device || !m_heap) return m_nullArrayTexture;
    const uint32_t slot=AllocateDescriptor();
    if(slot==UINT32_MAX) return nullptr;
    auto cpu=m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu=m_heap->GetGPUDescriptorHandleForHeapStart();
    const auto index=m_firstDescriptor+slot;
    cpu.ptr+=SIZE_T(index)*m_descriptorSize;
    gpu.ptr+=UINT64(index)*m_descriptorSize;
    // Null descriptors must carry the dimension the shader declares.
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels=1;
    view.Texture2DArray.ArraySize=1;
    m_device->CreateShaderResourceView(nullptr,&view,cpu);
    m_nullArrayTexture=std::make_shared<D3D12GraphicsTexture>(
        Microsoft::WRL::ComPtr<ID3D12Resource>{},m_heap,gpu);
    return m_nullArrayTexture;
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D12TextureFactory::CreateTexture2D(
    uint32_t width,
    uint32_t height,
    const uint8_t* rgbaPixels,
    uint32_t mipLevels,
    Engine::Graphics::GraphicsTextureFormat format,
    bool srgb)
{
    return Create(width, height, 1, false, rgbaPixels, mipLevels, format, srgb);
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D12TextureFactory::CreateTexture2DArray(
    uint32_t width, uint32_t height, uint32_t layers, const uint8_t* pixels,
    uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format, bool srgb)
{
    return Create(width, height, layers, true, pixels, mipLevels, format, srgb);
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> D3D12TextureFactory::Create(
    uint32_t width, uint32_t height, uint32_t layers, bool asArray,
    const uint8_t* rgbaPixels, uint32_t mipLevels,
    Engine::Graphics::GraphicsTextureFormat format, bool srgb)
{
    if (!m_device || !m_queue || !m_heap || !width || !height || !layers || !rgbaPixels ||
        !mipLevels || !Engine::Graphics::GraphicsTextureIsValidBase(format, width, height))
        return nullptr;
    PollUploads();
    const uint32_t slot = AllocateDescriptor();
    if(slot==UINT32_MAX) return nullptr;
    struct DescriptorLease
    {
        std::shared_ptr<DescriptorSlots> slots;uint32_t slot;bool owned=false;
        ~DescriptorLease(){if(!owned){std::lock_guard lock(slots->mutex);slots->free.push_back(slot);}}
    } lease{m_slots,slot};

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = static_cast<UINT16>(layers);
    textureDesc.MipLevels = static_cast<UINT16>(mipLevels);
    textureDesc.Format = D3D12TextureFormat(format, srgb, false);
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(m_device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
        return nullptr;

    // Subresource index is mip + layer * mipLevels, the source order.
    const uint32_t subresources = mipLevels * layers;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    std::vector<UINT> rowCounts(subresources);
    std::vector<UINT64> rowBytes(subresources);
    UINT64 uploadSize = 0;
    m_device->GetCopyableFootprints(
        &textureDesc, 0, subresources, 0, footprints.data(), rowCounts.data(),
        rowBytes.data(), &uploadSize);
    if (GetPendingUploadBytes() + uploadSize > 256ull * 1024 * 1024) return nullptr;

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
    size_t sourceOffset = 0;
    for (uint32_t subresource = 0; subresource < subresources; ++subresource)
    {
        const uint32_t mip = subresource % mipLevels;
        const uint32_t mipWidth = std::max(1u, width >> mip);
        const uint32_t mipHeight = std::max(1u, height >> mip);
        // Rows of pixels, or rows of 4x4 blocks for compressed formats.
        const size_t rowSize = GraphicsTextureRowBytes(format, mipWidth);
        const uint32_t rows = GraphicsTextureRowCount(format, mipHeight);
        const auto& footprint = footprints[subresource];
        for (uint32_t row = 0; row < rows; ++row)
            std::memcpy(
                mapped + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                rgbaPixels + sourceOffset + static_cast<size_t>(row) * rowSize,
                rowSize);
        sourceOffset += rowSize * rows;
    }
    upload->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&commands))))
        return nullptr;

    for (uint32_t subresource = 0; subresource < subresources; ++subresource)
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[subresource];
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }

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
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return nullptr;
    m_uploads.reserve(m_uploads.size()+1);
    {
        std::lock_guard queueLock(DX12QueueMutex());
        m_queue->ExecuteCommandLists(1, lists);
        if (FAILED(m_queue->Signal(fence.Get(), 1)))
            return nullptr;
    }
    // Same graphics queue: subsequent draws are ordered after this copy.
    // Keep command/staging/texture storage alive until the upload fence passes.
    m_uploads.push_back({upload, texture, allocator, commands, fence, uploadSize});

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    const uint32_t descriptorIndex = m_firstDescriptor + slot;
    cpu.ptr += static_cast<SIZE_T>(descriptorIndex) * m_descriptorSize;
    gpu.ptr += static_cast<UINT64>(descriptorIndex) * m_descriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = D3D12TextureFormat(format, srgb, true);
    srv.ViewDimension = asArray ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (asArray)
    {
        srv.Texture2DArray.MipLevels = mipLevels;
        srv.Texture2DArray.ArraySize = layers;
    }
    else
        srv.Texture2D.MipLevels = mipLevels;
    m_device->CreateShaderResourceView(texture.Get(), &srv, cpu);
    auto result = std::make_shared<D3D12GraphicsTexture>(texture, m_heap, gpu);
    result->SetUpload(fence, m_device->GetResourceAllocationInfo(0, 1, &textureDesc).SizeInBytes);
    OwnDescriptor(result, slot);
    lease.owned=true;
    m_uploads.back().owner = result;
    return result;
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture>
D3D12TextureFactory::CreateDepthTexture2D(uint32_t width, uint32_t height)
{
    if (!m_device || !m_heap || !width || !height)
        return nullptr;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    clear.DepthStencil.Depth = 1.f;
    clear.DepthStencil.Stencil = 0;
    const auto heapProperties = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    if (FAILED(m_device->CreateCommittedResource(&heapProperties,
        D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clear, IID_PPV_ARGS(&texture))))
        return nullptr;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    const uint32_t slot = AllocateDescriptor();
    if(slot==UINT32_MAX) return nullptr;
    const uint32_t descriptorIndex = m_firstDescriptor + slot;
    cpu.ptr += static_cast<SIZE_T>(descriptorIndex) * m_descriptorSize;
    gpu.ptr += static_cast<UINT64>(descriptorIndex) * m_descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(texture.Get(), &srv, cpu);

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
    if (FAILED(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap))))
    {
        std::lock_guard lock(m_slots->mutex); m_slots->free.push_back(slot);
        return nullptr;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(texture.Get(), &dsv,
        dsvHeap->GetCPUDescriptorHandleForHeapStart());
    auto result = std::make_shared<D3D12GraphicsTexture>(
        std::move(texture), m_heap, gpu, std::move(dsvHeap));
    OwnDescriptor(result,slot);
    return result;
}
}
