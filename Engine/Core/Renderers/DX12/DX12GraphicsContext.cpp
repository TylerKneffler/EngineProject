#include "DX12GraphicsContext.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IPipelineState.h"
#include "DX12GraphicsBuffer.h"
#include "DX12GraphicsTexture.h"
#include <algorithm>
#include <stdexcept>

// ---------------------------------------------------------------------------
// D3D12GraphicsContext
// ---------------------------------------------------------------------------

namespace Engine::Renderers
{
struct D3D12OcclusionQueryState
{
    static constexpr uint32_t QueryCapacity = 16384;
    struct Entry
    {
        uint64_t viewId = 0;
        uint64_t issueSignature = 0;
        uint64_t lastProbeFrame = 0;
        uint64_t lastTouchedFrame = 0;
        bool pending = false;
        bool occluded = false;
    };
    struct Record
    {
        uint64_t key = 0;
        uint64_t signature = 0;
    };
    struct FrameSlot
    {
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        uint64_t* mappedResults = nullptr;
        std::vector<Record> records;
        uint32_t used = 0;
    };

    explicit D3D12OcclusionQueryState(ID3D12Device* value) : device(value) {}
    ~D3D12OcclusionQueryState()
    {
        for (auto& slot : slots)
            if (slot.readback && slot.mappedResults)
                slot.readback->Unmap(0, nullptr);
    }

    bool EnsureSlot(uint32_t frameSlot)
    {
        if (!device) return false;
        if (slots.size() <= frameSlot) slots.resize(frameSlot + 1u);
        auto& slot = slots[frameSlot];
        if (slot.heap) return true;

        D3D12_QUERY_HEAP_DESC queryDesc{};
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        queryDesc.Count = QueryCapacity;
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&slot.heap))))
            return false;

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = uint64_t(QueryCapacity) * sizeof(uint64_t);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heapProperties,
            D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&slot.readback))))
            return false;
        D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(bufferDesc.Width) };
        return SUCCEEDED(slot.readback->Map(0, &readRange,
            reinterpret_cast<void**>(&slot.mappedResults)));
    }

    void Prepare(uint32_t frameSlot)
    {
        if (!EnsureSlot(frameSlot)) { currentSlot = UINT32_MAX; return; }
        auto& slot = slots[frameSlot];
        // The renderer calls this only after its existing fence wait proves
        // this slot's previous command list has completed.
        for (uint32_t index = 0; index < slot.used; ++index)
        {
            const auto& record = slot.records[index];
            const auto found = entries.find(record.key);
            if (found == entries.end()) continue;
            auto& entry = found->second;
            if (entry.pending && entry.issueSignature == record.signature)
            {
                entry.pending = false;
                entry.occluded = slot.mappedResults[index] == 0;
            }
        }
        slot.used = 0;
        slot.records.clear();
        currentSlot = frameSlot;
    }

    ID3D12Device* device = nullptr;
    std::vector<FrameSlot> slots;
    std::unordered_map<uint64_t, Entry> entries;
    std::unordered_map<uint64_t, uint64_t> viewSignatures;
    uint64_t frameIndex = 0;
    uint32_t currentSlot = UINT32_MAX;
};

namespace
{
uint64_t D3D12OcclusionKey(uint64_t viewId, uint64_t objectId)
{
    objectId ^= viewId + 0x9e3779b97f4a7c15ull +
        (objectId << 6u) + (objectId >> 2u);
    return objectId;
}
}

D3D12GraphicsContext::D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList)
    : m_cmdList(cmdList)
{
}

D3D12GraphicsContext::D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList,
    ID3D12RootSignature* rootSig,
    std::shared_ptr<D3D12OcclusionQueryState> occlusionState)
    : m_cmdList(cmdList), m_rootSignature(rootSig),
      m_occlusionState(std::move(occlusionState))
{
    // Set the root signature immediately if provided
    if (m_cmdList && m_rootSignature)
    {
        m_cmdList->SetGraphicsRootSignature(m_rootSignature);
    }
}

bool D3D12GraphicsContext::BeginOcclusionFrame(uint64_t viewId,
    uint64_t sceneSignature)
{
    m_occlusionViewId = viewId;
    m_occlusionSceneSignature = sceneSignature;
    m_activeOcclusionKey = 0;
    m_activeOcclusionIndex = UINT32_MAX;
    if (!m_cmdList || !m_occlusionState ||
        m_occlusionState->currentSlot == UINT32_MAX)
        return false;

    ++m_occlusionState->frameIndex;
    const auto previous = m_occlusionState->viewSignatures.find(viewId);
    const bool invalidated = previous == m_occlusionState->viewSignatures.end() ||
        previous->second != sceneSignature;
    m_occlusionState->viewSignatures[viewId] = sceneSignature;
    for (auto iterator = m_occlusionState->entries.begin();
        iterator != m_occlusionState->entries.end();)
    {
        auto& entry = iterator->second;
        if (entry.viewId == viewId && invalidated)
            entry.occluded = false;
        const bool stale = !entry.pending && m_occlusionState->frameIndex >
            entry.lastTouchedFrame + 600u;
        if (stale) iterator = m_occlusionState->entries.erase(iterator);
        else ++iterator;
    }
    return !invalidated;
}

bool D3D12GraphicsContext::IsOccluded(uint64_t objectId)
{
    if (!m_occlusionState || !m_occlusionViewId || !objectId) return false;
    const auto found = m_occlusionState->entries.find(
        D3D12OcclusionKey(m_occlusionViewId, objectId));
    if (found == m_occlusionState->entries.end() ||
        found->second.viewId != m_occlusionViewId)
        return false;
    auto& entry = found->second;
    entry.lastTouchedFrame = m_occlusionState->frameIndex;
    constexpr uint64_t reprobeInterval = 8u;
    if (entry.occluded && m_occlusionState->frameIndex <
        entry.lastProbeFrame + reprobeInterval)
        return true;
    entry.occluded = false;
    return false;
}

void D3D12GraphicsContext::BeginOcclusionQuery(uint64_t objectId)
{
    m_activeOcclusionKey = 0;
    m_activeOcclusionIndex = UINT32_MAX;
    if (!m_cmdList || !m_occlusionState || !m_occlusionViewId || !objectId ||
        m_occlusionState->currentSlot >= m_occlusionState->slots.size())
        return;
    auto& slot = m_occlusionState->slots[m_occlusionState->currentSlot];
    if (!slot.heap || slot.used >= D3D12OcclusionQueryState::QueryCapacity)
        return;
    const uint64_t key = D3D12OcclusionKey(m_occlusionViewId, objectId);
    auto [iterator, inserted] = m_occlusionState->entries.try_emplace(key);
    auto& entry = iterator->second;
    if (!inserted && entry.viewId != m_occlusionViewId) entry = {};
    entry.viewId = m_occlusionViewId;
    entry.lastTouchedFrame = m_occlusionState->frameIndex;
    if (entry.pending) return;

    m_activeOcclusionIndex = slot.used++;
    m_activeOcclusionKey = key;
    m_cmdList->BeginQuery(slot.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
        m_activeOcclusionIndex);
    entry.lastProbeFrame = m_occlusionState->frameIndex;
}

void D3D12GraphicsContext::EndOcclusionQuery()
{
    if (!m_cmdList || !m_occlusionState ||
        m_activeOcclusionIndex == UINT32_MAX ||
        m_occlusionState->currentSlot >= m_occlusionState->slots.size())
        return;
    auto& slot = m_occlusionState->slots[m_occlusionState->currentSlot];
    m_cmdList->EndQuery(slot.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
        m_activeOcclusionIndex);
    m_cmdList->ResolveQueryData(slot.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
        m_activeOcclusionIndex, 1, slot.readback.Get(),
        uint64_t(m_activeOcclusionIndex) * sizeof(uint64_t));
    auto& entry = m_occlusionState->entries[m_activeOcclusionKey];
    entry.pending = true;
    entry.issueSignature = m_occlusionSceneSignature;
    slot.records.push_back({ m_activeOcclusionKey, m_occlusionSceneSignature });
    m_activeOcclusionKey = 0;
    m_activeOcclusionIndex = UINT32_MAX;
}

void D3D12GraphicsContext::SetPipeline(const Engine::Graphics::IPipelineState* pipeline)
{
    if (!pipeline || !m_cmdList) return;

    auto* nativeHandle = static_cast<ID3D12PipelineState*>(const_cast<void*>(pipeline->GetNativeHandle()));
    if (nativeHandle)
    {
        m_cmdList->SetPipelineState(nativeHandle);
    }
}

void D3D12GraphicsContext::SetConstantBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint64_t offset)
{
    if (!buffer || !m_cmdList) return;

    auto* d3dBuffer = static_cast<D3D12GraphicsBuffer*>(const_cast<Engine::Graphics::IGraphicsBuffer*>(buffer));
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = d3dBuffer->GetGPUVirtualAddress() + offset;
    m_cmdList->SetGraphicsRootConstantBufferView(slot, gpuAddr);
}

void D3D12GraphicsContext::SetVertexBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset)
{
    if (!buffer || !m_cmdList) return;

    auto* d3dBuffer = static_cast<D3D12GraphicsBuffer*>(const_cast<Engine::Graphics::IGraphicsBuffer*>(buffer));
    
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = d3dBuffer->GetGPUVirtualAddress() + offset;
    vbv.SizeInBytes = static_cast<UINT>(d3dBuffer->GetSize() - offset);
    vbv.StrideInBytes = stride;

    m_cmdList->IASetVertexBuffers(slot, 1, &vbv);
}

void D3D12GraphicsContext::SetIndexBuffer(const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset)
{
    if (!buffer || !m_cmdList) return;

    auto* d3dBuffer = static_cast<D3D12GraphicsBuffer*>(const_cast<Engine::Graphics::IGraphicsBuffer*>(buffer));
    
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = d3dBuffer->GetGPUVirtualAddress() + offset;
    ibv.SizeInBytes = static_cast<UINT>(d3dBuffer->GetSize() - offset);
    ibv.Format = DXGI_FORMAT_R32_UINT;

    m_cmdList->IASetIndexBuffer(&ibv);
    m_indexCount = indexCount;
}

void D3D12GraphicsContext::SetStructuredBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer)
{
    if (!buffer || !m_cmdList || slot < 6 || slot > 8) return;
    const auto* structured = dynamic_cast<const D3D12GraphicsBuffer*>(buffer);
    if (!structured) return;
    // Root parameters 7-9 are root SRVs for shader registers t6-t8.
    m_cmdList->SetGraphicsRootShaderResourceView(
        slot + 1, structured->GetGPUVirtualAddress());
}

void D3D12GraphicsContext::SetTexture(uint32_t slot, const Engine::Graphics::IGraphicsTexture* texture)
{
    if (!m_cmdList || slot >= 7)
        return;
    const auto* nativeTexture = dynamic_cast<const D3D12GraphicsTexture*>(texture);
    if (!nativeTexture)
        return;
    ID3D12DescriptorHeap* heaps[] = { nativeTexture->GetHeap() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    m_cmdList->SetGraphicsRootDescriptorTable(
        slot == 6 ? 10 : slot + 1, nativeTexture->GetGpuHandle());
}

void D3D12GraphicsContext::SetStencilReference(uint32_t reference)
{
    if (m_cmdList)
        m_cmdList->OMSetStencilRef(reference);
}

void D3D12GraphicsContext::SetViewport(const Viewport& vp)
{
    if (!m_cmdList) return;

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = vp.x;
    viewport.TopLeftY = vp.y;
    viewport.Width = vp.width;
    viewport.Height = vp.height;
    viewport.MinDepth = vp.minDepth;
    viewport.MaxDepth = vp.maxDepth;

    m_cmdList->RSSetViewports(1, &viewport);
}

void D3D12GraphicsContext::SetScissorRect(const ScissorRect& rect)
{
    if (!m_cmdList) return;

    D3D12_RECT scissorRect{};
    scissorRect.left = rect.left;
    scissorRect.top = rect.top;
    scissorRect.right = rect.right;
    scissorRect.bottom = rect.bottom;

    m_cmdList->RSSetScissorRects(1, &scissorRect);
}

void D3D12GraphicsContext::Clear(float r, float g, float b, float a, float depth)
{
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    (void)depth;
    // This would typically clear the current render target
    // Implementation depends on having RTV/DSV descriptors
    // For now, this is a placeholder
}

void D3D12GraphicsContext::DrawInstanced(
    uint32_t vertexCountPerInstance,
    uint32_t instanceCount,
    uint32_t startVertexLocation,
    uint32_t startInstanceLocation)
{
    if (!m_cmdList) return;
    // D3D12 requires primitive topology to be set before drawing
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->DrawInstanced(vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

void D3D12GraphicsContext::DrawIndexedInstanced(
    uint32_t indexCountPerInstance,
    uint32_t instanceCount,
    uint32_t startIndexLocation,
    int32_t baseVertexLocation,
    uint32_t startInstanceLocation)
{
    if (!m_cmdList) return;
    m_cmdList->DrawIndexedInstanced(indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void D3D12GraphicsContext::TransitionResource(void* resource, ResourceState stateBefore, ResourceState stateAfter)
{
    if (!resource || !m_cmdList) return;

    auto* d3dResource = static_cast<ID3D12Resource*>(resource);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = d3dResource;
    barrier.Transition.StateBefore = ConvertResourceState(stateBefore);
    barrier.Transition.StateAfter = ConvertResourceState(stateAfter);
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_cmdList->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_STATES D3D12GraphicsContext::ConvertResourceState(ResourceState state) const
{
    switch (state)
    {
        case ResourceState::Common: return D3D12_RESOURCE_STATE_COMMON;
        case ResourceState::VertexAndConstantBuffer: return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        case ResourceState::IndexBuffer: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
        case ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case ResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ResourceState::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
        case ResourceState::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case ResourceState::StreamOut: return D3D12_RESOURCE_STATE_STREAM_OUT;
        case ResourceState::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case ResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::CopySrc: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
        default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

// ---------------------------------------------------------------------------
// D3D12GraphicsContextFactory
// ---------------------------------------------------------------------------

D3D12GraphicsContextFactory::D3D12GraphicsContextFactory(
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    ID3D12RootSignature* rootSignature)
    : m_device(device), m_commandQueue(commandQueue), m_rootSignature(rootSignature),
      m_occlusionState(std::make_shared<D3D12OcclusionQueryState>(device))
{
    // Create command allocator
    HRESULT hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_cmdAllocator));
    
    if (FAILED(hr))
        throw std::runtime_error("Failed to create D3D12 command allocator");

    // Create command list
    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_cmdList));
    
    if (FAILED(hr))
        throw std::runtime_error("Failed to create D3D12 command list");
}

std::unique_ptr<Engine::Graphics::IGraphicsContext> D3D12GraphicsContextFactory::CreateContext()
{
    // Use the externally-supplied command list if one was set via SetCommandBuffer,
    // otherwise fall back to the factory's own command list.
    auto* list = m_externalCmdList ? m_externalCmdList : m_cmdList.Get();
    return std::make_unique<D3D12GraphicsContext>(
        list, m_rootSignature, m_occlusionState);
}

void D3D12GraphicsContextFactory::SetCommandBuffer(void* cmd)
{
    m_externalCmdList = static_cast<ID3D12GraphicsCommandList*>(cmd);
}

void D3D12GraphicsContextFactory::PrepareFrame(uint32_t frameSlot)
{
    if (m_occlusionState) m_occlusionState->Prepare(frameSlot);
}
}
