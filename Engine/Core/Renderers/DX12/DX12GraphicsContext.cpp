#include <cstdlib>
#include "DX12GraphicsContext.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IPipelineState.h"
#include "DX12GraphicsBuffer.h"
#include "DX12DredDiagnostics.h"
#include "DX12PipelineStateBuilder.h"
#include "DX12PostProcess.h"
#include "DX12GraphicsTexture.h"
#include <algorithm>
#include <chrono>
#include <d3dcompiler.h>
#include <mutex>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// D3D12GraphicsContext
// ---------------------------------------------------------------------------

namespace Engine::Renderers
{
namespace
{
bool HasEnvironmentVariable(const char* name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t valueLength = 0;
    const bool present = _dupenv_s(&value, &valueLength, name) == 0 && value;
    std::free(value);
    return present;
#else
    return std::getenv(name) != nullptr;
#endif
}
}

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

struct D3D12GpuTimingState
{
    static constexpr uint32_t QueryCapacity = 2048;
    struct Record { Engine::Graphics::GpuTimingStage stage{}; uint32_t begin = 0; uint32_t end = 0; };
    struct FrameSlot
    {
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        uint64_t* mapped = nullptr;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> statisticsHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> statisticsReadback;
        D3D12_QUERY_DATA_PIPELINE_STATISTICS* statistics = nullptr;
        std::vector<Record> records;
        uint32_t used = 0;
        bool pending = false; bool overflow = false;
    };
    D3D12GpuTimingState(ID3D12Device* value, ID3D12CommandQueue* queue)
        : device(value)
    { if (queue) queue->GetTimestampFrequency(&frequency); }
    ~D3D12GpuTimingState()
    { for (auto& slot : slots) if (slot.readback && slot.mapped) slot.readback->Unmap(0, nullptr);
      for (auto& slot : slots) if (slot.statisticsReadback && slot.statistics) slot.statisticsReadback->Unmap(0, nullptr); }
    bool EnsureSlot(uint32_t index)
    {
        if (!device) return false;
        if (slots.size() <= index) slots.resize(index + 1u);
        auto& slot = slots[index]; if (slot.heap && slot.mapped && (!collectWorkload || slot.statistics)) return true;
        D3D12_QUERY_HEAP_DESC query{}; query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; query.Count = QueryCapacity;
        if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&slot.heap)))) return false;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = uint64_t(QueryCapacity) * sizeof(uint64_t); buffer.Height = 1;
        buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.readback)))) return false;
        D3D12_RANGE range{0, static_cast<SIZE_T>(buffer.Width)};
        if (FAILED(slot.readback->Map(0, &range, reinterpret_cast<void**>(&slot.mapped)))) return false;
        if (collectWorkload)
        {
            query.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
            if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&slot.statisticsHeap)))) return false;
            buffer.Width = uint64_t(QueryCapacity) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.statisticsReadback)))) return false;
            range.End = static_cast<SIZE_T>(buffer.Width);
            if (FAILED(slot.statisticsReadback->Map(0, &range, reinterpret_cast<void**>(&slot.statistics)))) return false;
        }
        return true;
    }
    void Prepare(uint32_t index)
    {
        if (!EnsureSlot(index)) { currentSlot = UINT32_MAX; return; }
        auto& slot = slots[index];
        if (slot.pending && frequency)
        {
            telemetry.gpuMilliseconds.fill(0.0);
            telemetry.gpuWorkload = {};
            telemetry.gpuWorkloadValid = collectWorkload && slot.statistics;
            if (telemetry.gpuWorkloadValid)
                for (size_t i = 0; i < slot.records.size(); ++i)
                {
                    auto& workload = telemetry.gpuWorkload[static_cast<size_t>(slot.records[i].stage)];
                    workload.vertices += slot.statistics[i].VSInvocations;
                    workload.primitives += slot.statistics[i].IAPrimitives;
                    workload.fragments += slot.statistics[i].PSInvocations;
                }
            for (const auto& record : slot.records)
                if (slot.mapped[record.end] >= slot.mapped[record.begin])
                    telemetry.gpuMilliseconds[static_cast<size_t>(record.stage)] +=
                        double(slot.mapped[record.end] - slot.mapped[record.begin]) * 1000.0 / double(frequency);
            telemetry.gpuTimingsValid = !slot.overflow;
                if (slot.overflow) telemetry.gpuWorkloadValid = false; telemetry.gpuRegionCount = static_cast<uint32_t>(slot.records.size());
            ++telemetry.gpuSampleId;
        }
        slot.used = 0; slot.records.clear(); slot.pending = false; slot.overflow = false;
        currentSlot = index; active = UINT32_MAX; cpuStart = std::chrono::steady_clock::now();
    }
    void Begin(ID3D12GraphicsCommandList* list, Engine::Graphics::GpuTimingStage stage)
    {
        if (!list || currentSlot >= slots.size() || active != UINT32_MAX) return;
        auto& slot = slots[currentSlot]; if (slot.used + 2 > QueryCapacity) { slot.overflow = true; return; }
        Record record{stage, slot.used++, slot.used++}; slot.records.push_back(record);
        active = static_cast<uint32_t>(slot.records.size() - 1u);
        list->EndQuery(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, record.begin);
        if (collectWorkload) list->BeginQuery(slot.statisticsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, active);
    }
    void End(ID3D12GraphicsCommandList* list, Engine::Graphics::GpuTimingStage stage)
    {
        if (!list || currentSlot >= slots.size() || active == UINT32_MAX) return;
        auto& slot = slots[currentSlot]; auto& record = slot.records[active]; if (record.stage != stage) return;
        if (collectWorkload) list->EndQuery(slot.statisticsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, active);
        list->EndQuery(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, record.end); active = UINT32_MAX;
    }
    void Finalize(ID3D12GraphicsCommandList* list)
    {
        if (!list || currentSlot >= slots.size()) return;
        auto& slot = slots[currentSlot];
        if (active != UINT32_MAX) End(list, slot.records[active].stage);
        if (slot.used) list->ResolveQueryData(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            0, slot.used, slot.readback.Get(), 0);
        if (collectWorkload && !slot.records.empty())
            list->ResolveQueryData(slot.statisticsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                0, static_cast<UINT>(slot.records.size()), slot.statisticsReadback.Get(), 0);
        slot.pending = slot.used != 0;
        telemetry.cpuSubmissionMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuStart).count();
    }
    const bool collectWorkload = HasEnvironmentVariable("ENGINE_GPU_WORKLOAD_COUNTERS");
    ID3D12Device* device = nullptr; uint64_t frequency = 0; std::vector<FrameSlot> slots;
    uint32_t currentSlot = UINT32_MAX, active = UINT32_MAX;
    Engine::Graphics::FrameTimingTelemetry telemetry{};
    std::chrono::steady_clock::time_point cpuStart{};
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

struct D3D12IndirectSignatures
{
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> signatures[2];
    explicit D3D12IndirectSignatures(ID3D12Device* device)
    {
        for(unsigned indexed=0;indexed<2;++indexed)
        {
            D3D12_INDIRECT_ARGUMENT_DESC argument{};
            argument.Type=indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            D3D12_COMMAND_SIGNATURE_DESC descriptor{};
            descriptor.ByteStride=indexed?20u:16u; descriptor.NumArgumentDescs=1; descriptor.pArgumentDescs=&argument;
            if(FAILED(device->CreateCommandSignature(&descriptor,nullptr,IID_PPV_ARGS(&signatures[indexed]))))
                throw std::runtime_error("Failed to create indirect draw signature");
        }
    }
};

D3D12GraphicsContext::D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList)
    : m_cmdList(cmdList)
{
}

D3D12GraphicsContext::D3D12GraphicsContext(ID3D12GraphicsCommandList* cmdList,
    ID3D12RootSignature* rootSig,
    std::shared_ptr<D3D12OcclusionQueryState> occlusionState,
    std::shared_ptr<D3D12GpuTimingState> gpuTimings,
    std::shared_ptr<D3D12IndirectSignatures> indirectSignatures,
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> nullTexture,
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> nullArrayTexture)
    : m_cmdList(cmdList), m_rootSignature(rootSig),
      m_occlusionState(std::move(occlusionState)), m_gpuTimings(std::move(gpuTimings)), m_indirectSignatures(std::move(indirectSignatures)), m_nullTexture(std::move(nullTexture)),
      m_nullArrayTexture(std::move(nullArrayTexture))
{
    // Set the root signature immediately if provided
    if (m_cmdList && m_rootSignature)
    {
        m_cmdList->SetGraphicsRootSignature(m_rootSignature);
    }
}

void D3D12GraphicsContext::BeginGpuTiming(Engine::Graphics::GpuTimingStage stage)
{ if (m_gpuTimings) m_gpuTimings->Begin(m_cmdList, stage); }
void D3D12GraphicsContext::EndGpuTiming(Engine::Graphics::GpuTimingStage stage)
{ if (m_gpuTimings) m_gpuTimings->End(m_cmdList, stage); }

void D3D12GraphicsContext::SetDebugMarker(uint64_t tag)
{
    if (!m_cmdList || !Engine::Renderers::IsDredEnabled()) return;
    // A wide string marker (metadata 0): DRED keeps it as the breadcrumb's
    // context, which the device-removed report prints.
    wchar_t text[32];
    const int length = swprintf_s(text, L"tag %llu", static_cast<unsigned long long>(tag));
    if (length > 0)
        m_cmdList->SetMarker(0, text, static_cast<UINT>((length + 1) * sizeof(wchar_t)));
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
    // Match the bound target's sample count: the MSAA scene target outside
    // depth-only passes (shadow maps are always single-sample).
    if (auto* state = dynamic_cast<const D3D12PipelineState*>(pipeline))
        nativeHandle = state->ForSamples(m_activeDepthTarget ? 1u
            : DX12PostProcess::BoundSceneSamples(m_cmdList));
    if (nativeHandle)
    {
        m_cmdList->SetPipelineState(nativeHandle);
    }
}

void D3D12GraphicsContext::SetConstantBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint64_t offset)
{
    if (!buffer || !m_cmdList) return;

    auto* d3dBuffer = static_cast<D3D12GraphicsBuffer*>(const_cast<Engine::Graphics::IGraphicsBuffer*>(buffer));
    RetainBuffer(d3dBuffer->GetResource());
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = d3dBuffer->GetGPUVirtualAddress() + offset;
    m_cmdList->SetGraphicsRootConstantBufferView(slot, gpuAddr);
}

void D3D12GraphicsContext::SetVertexBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset)
{
    if (!buffer || !m_cmdList) return;

    auto* d3dBuffer = static_cast<D3D12GraphicsBuffer*>(const_cast<Engine::Graphics::IGraphicsBuffer*>(buffer));
    RetainBuffer(d3dBuffer->GetResource());
    
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
    RetainBuffer(d3dBuffer->GetResource());
    
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = d3dBuffer->GetGPUVirtualAddress() + offset;
    ibv.SizeInBytes = static_cast<UINT>(d3dBuffer->GetSize() - offset);
    ibv.Format = DXGI_FORMAT_R32_UINT;

    m_cmdList->IASetIndexBuffer(&ibv);
    m_indexCount = indexCount;
}

void D3D12GraphicsContext::SetStructuredBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer)
{
    if (!buffer || !m_cmdList ||
        !((slot >= 6 && slot <= 8) || (slot >= 10 && slot <= 11) ||
          (slot >= 14 && slot <= 15) || (slot >= 45 && slot <= 47))) return;
    const auto* structured = dynamic_cast<const D3D12GraphicsBuffer*>(buffer);
    if (!structured) return;
    RetainBuffer(structured->GetResource());
    // Root parameters 7-9 and 11-12 are root SRVs matching t6-t8/t10-t11.
    const uint32_t rootParameter = slot + 1;
    m_cmdList->SetGraphicsRootShaderResourceView(
        rootParameter, structured->GetGPUVirtualAddress());
}

namespace
{
void TransitionComputeBuffer(ID3D12GraphicsCommandList* commands,
    D3D12GraphicsBuffer* buffer, D3D12_RESOURCE_STATES destination)
{
    if (!commands || !buffer || buffer->GetResourceState() == destination) return;
    // A buffer readable by every shader stage (CopyBufferRegion leaves the
    // placement pool so) already satisfies a narrower read state.
    if (buffer->GetResourceState() == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE &&
        (destination == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ||
         destination == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer->GetResource();
    barrier.Transition.StateBefore = buffer->GetResourceState();
    barrier.Transition.StateAfter = destination;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &barrier);
    buffer->SetResourceState(destination);
}
}

bool D3D12GraphicsContext::SetComputePipeline(
    const Engine::Graphics::IPipelineState* pipeline)
{
    const auto* compute = dynamic_cast<const D3D12ComputePipeline*>(pipeline);
    if (!m_cmdList || !compute) return false;
    m_cmdList->SetComputeRootSignature(compute->GetRootSignature());
    m_cmdList->SetPipelineState(static_cast<ID3D12PipelineState*>(
        compute->GetNativeHandle()));
    return true;
}

void D3D12GraphicsContext::SetComputeConstants(const void* data, uint32_t size)
{
    if (m_cmdList && data && size && size <= 128 && size % 4 == 0)
        m_cmdList->SetComputeRoot32BitConstants(0, size / 4, data, 0);
}

void D3D12GraphicsContext::SetComputeReadBuffer(
    const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t slot)
{
    auto* native = dynamic_cast<D3D12GraphicsBuffer*>(
        const_cast<Engine::Graphics::IGraphicsBuffer*>(buffer));
    if (!m_cmdList || !native) return;
    RetainBuffer(native->GetResource());
    TransitionComputeBuffer(m_cmdList, native,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_cmdList->SetComputeRootShaderResourceView(slot == 2 ? 4 : slot == 1 ? 3 : 1,
        native->GetGPUVirtualAddress());
}

void D3D12GraphicsContext::SetComputeWriteBuffer(
    const Engine::Graphics::IGraphicsBuffer* buffer)
{
    auto* native = dynamic_cast<D3D12GraphicsBuffer*>(
        const_cast<Engine::Graphics::IGraphicsBuffer*>(buffer));
    if (!m_cmdList || !native || native->GetUsage() !=
        Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer) return;
    RetainBuffer(native->GetResource());
    TransitionComputeBuffer(m_cmdList, native,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cmdList->SetComputeRootUnorderedAccessView(2,
        native->GetGPUVirtualAddress());
}

void D3D12GraphicsContext::DispatchCompute(uint32_t x, uint32_t y, uint32_t z)
{
    if (m_cmdList && x && y && z) m_cmdList->Dispatch(x, y, z);
}

namespace
{
// Copies sample 0 of a multisampled scene depth into a float buffer. MSAA
// depth cannot be copied with CopyTextureRegion, and D3D12 does not resolve
// depth formats. Its own root signature: (b0) extent, (t0) depth table,
// (u0) output buffer; one shader-visible descriptor for the depth view.
struct MultisampledDepthCopy
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    ID3D12Resource* describedDepth = nullptr;
    bool failed = false;
};

MultisampledDepthCopy* AcquireMultisampledDepthCopy(ID3D12Device* device)
{
    static std::mutex mutex;
    static MultisampledDepthCopy copy;
    std::lock_guard lock(mutex);
    if (copy.device.Get() != device)
        copy = MultisampledDepthCopy{ device };
    if (copy.pipeline || copy.failed) return copy.pipeline ? &copy : nullptr;
    copy.failed = true;
    static const char source[] =
        "Texture2DMS<float> sceneDepth : register(t0);\n"
        "RWStructuredBuffer<float> depthOutput : register(u0);\n"
        "cbuffer CopyConstants : register(b0) { uint4 extent; };\n"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    if (id.x >= extent.x || id.y >= extent.y) return;\n"
        "    depthOutput[id.y * extent.z + id.x] = sceneDepth.Load(int2(id.xy), 0);\n"
        "}\n";
    Microsoft::WRL::ComPtr<ID3DBlob> shader, errors;
    if (FAILED(D3DCompile(source, sizeof(source) - 1, "MultisampledDepthCopy", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors)))
        return nullptr;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 4;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC signatureDesc{};
    signatureDesc.NumParameters = 3;
    signatureDesc.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    if (FAILED(D3D12SerializeRootSignature(&signatureDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            &signature, &errors)) ||
        FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(),
            signature->GetBufferSize(), IID_PPV_ARGS(&copy.rootSignature))))
        return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = copy.rootSignature.Get();
    pipelineDesc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&copy.pipeline))) ||
        FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&copy.heap))))
    {
        copy.pipeline.Reset();
        return nullptr;
    }
    copy.failed = false;
    return &copy;
}
}

void D3D12GraphicsContext::CopySceneDepth(Engine::Graphics::IGraphicsBuffer* destination,
    uint32_t width, uint32_t height)
{
    auto* native = dynamic_cast<D3D12GraphicsBuffer*>(destination);
    if (!m_cmdList || !m_sceneDepth || !native || !width || !height) return;
    const D3D12_RESOURCE_DESC depthDesc = m_sceneDepth->GetDesc();
    width = std::min<uint32_t>(width, static_cast<uint32_t>(depthDesc.Width));
    height = std::min<uint32_t>(height, depthDesc.Height);
    const uint32_t pitch = Engine::Graphics::SceneDepthRowPitch(width);
    if (destination->GetSize() < uint64_t(pitch) * height * 4u) return;
    RetainBuffer(native->GetResource());
    if (depthDesc.SampleDesc.Count > 1u)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(m_cmdList->GetDevice(IID_PPV_ARGS(&device)))) return;
        auto* copy = AcquireMultisampledDepthCopy(device.Get());
        if (!copy || native->GetUsage() !=
            Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer) return;
        if (copy->describedDepth != m_sceneDepth)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            device->CreateShaderResourceView(m_sceneDepth, &view,
                copy->heap->GetCPUDescriptorHandleForHeapStart());
            copy->describedDepth = m_sceneDepth;
        }
        TransitionComputeBuffer(m_cmdList, native, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_RESOURCE_BARRIER toRead{};
        toRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRead.Transition.pResource = m_sceneDepth;
        toRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        toRead.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        m_cmdList->ResourceBarrier(1, &toRead);
        m_cmdList->SetComputeRootSignature(copy->rootSignature.Get());
        m_cmdList->SetPipelineState(copy->pipeline.Get());
        ID3D12DescriptorHeap* heaps[] = { copy->heap.Get() };
        m_cmdList->SetDescriptorHeaps(1, heaps);
        const uint32_t extent[4] = { width, height, pitch, 0u };
        m_cmdList->SetComputeRoot32BitConstants(0, 4, extent, 0);
        m_cmdList->SetComputeRootDescriptorTable(1, copy->heap->GetGPUDescriptorHandleForHeapStart());
        m_cmdList->SetComputeRootUnorderedAccessView(2, native->GetGPUVirtualAddress());
        m_cmdList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);
        D3D12_RESOURCE_BARRIER after[2]{};
        after[0] = toRead;
        std::swap(after[0].Transition.StateBefore, after[0].Transition.StateAfter);
        after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        after[1].UAV.pResource = native->GetResource();
        m_cmdList->ResourceBarrier(2, after);
        // Engine textures all live in one shared heap; put it back for the
        // descriptor tables bound after this copy.
        if (const auto* shared = dynamic_cast<const D3D12GraphicsTexture*>(m_nullTexture.get()))
        {
            ID3D12DescriptorHeap* restore[] = { shared->GetHeap() };
            m_cmdList->SetDescriptorHeaps(1, restore);
        }
        return;
    }
    TransitionComputeBuffer(m_cmdList, native, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sceneDepth;
    // Plane 0 of D32_FLOAT_S8X24 holds the depth.
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_cmdList->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_sceneDepth;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = native->GetResource();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Offset = 0;
    target.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_TYPELESS;
    target.PlacedFootprint.Footprint.Width = width;
    target.PlacedFootprint.Footprint.Height = height;
    target.PlacedFootprint.Footprint.Depth = 1;
    target.PlacedFootprint.Footprint.RowPitch = pitch * 4u;
    const D3D12_BOX box{ 0, 0, 0, width, height, 1 };
    m_cmdList->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_cmdList->ResourceBarrier(1, &barrier);
}

void D3D12GraphicsContext::StorageBarrier(
    const Engine::Graphics::IGraphicsBuffer* buffer)
{
    auto* native = dynamic_cast<D3D12GraphicsBuffer*>(
        const_cast<Engine::Graphics::IGraphicsBuffer*>(buffer));
    if (!m_cmdList || !native) return;
    RetainBuffer(native->GetResource());
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = native->GetResource();
    m_cmdList->ResourceBarrier(1, &barrier);
    TransitionComputeBuffer(m_cmdList, native,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}

bool D3D12GraphicsContext::DrawInstancesIndirect(const Engine::Graphics::IGraphicsBuffer* buffer,
    uint64_t offset, bool indexed)
{
    const auto* native = dynamic_cast<const D3D12GraphicsBuffer*>(buffer);
    if (!m_cmdList || !native || offset + (indexed ? 20u : 16u) > buffer->GetSize()) return false;
    if(!m_indirectSignatures) return false;
    RetainBuffer(native->GetResource());
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->ExecuteIndirect(m_indirectSignatures->signatures[indexed?1:0].Get(),1,native->GetResource(),offset,nullptr,0);
    return true;
}

bool D3D12GraphicsContext::CopyBuffer(
    const Engine::Graphics::IGraphicsBuffer* source,
    Engine::Graphics::IGraphicsBuffer* destination, uint64_t bytes)
{
    auto* src = dynamic_cast<D3D12GraphicsBuffer*>(
        const_cast<Engine::Graphics::IGraphicsBuffer*>(source));
    auto* dst = dynamic_cast<D3D12GraphicsBuffer*>(destination);
    if (!m_cmdList || !src || !dst || destination->GetAccessMode() !=
        Engine::Graphics::IGraphicsBuffer::AccessMode::Readback)
        return false;
    const uint64_t size = bytes ? bytes : source->GetSize();
    if (!size || size > source->GetSize() || size > destination->GetSize())
        return false;
    RetainBuffer(src->GetResource());
    RetainBuffer(dst->GetResource());
    const auto previous = src->GetResourceState();
    TransitionComputeBuffer(m_cmdList, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_cmdList->CopyBufferRegion(dst->GetResource(), 0,
        src->GetResource(), 0, size);
    TransitionComputeBuffer(m_cmdList, src, previous);
    return true;
}

bool D3D12GraphicsContext::CopyBufferRegion(
    const Engine::Graphics::IGraphicsBuffer* source, uint64_t sourceOffset,
    Engine::Graphics::IGraphicsBuffer* destination, uint64_t destinationOffset, uint64_t bytes)
{
    auto* src = dynamic_cast<D3D12GraphicsBuffer*>(
        const_cast<Engine::Graphics::IGraphicsBuffer*>(source));
    auto* dst = dynamic_cast<D3D12GraphicsBuffer*>(destination);
    using Access = Engine::Graphics::IGraphicsBuffer::AccessMode;
    if (!m_cmdList || !src || !dst || !bytes ||
        source->GetAccessMode() != Access::Upload || destination->GetAccessMode() != Access::Default ||
        sourceOffset + bytes > source->GetSize() || destinationOffset + bytes > destination->GetSize())
        return false;
    RetainBuffer(src->GetResource());
    RetainBuffer(dst->GetResource());
    TransitionComputeBuffer(m_cmdList, dst, D3D12_RESOURCE_STATE_COPY_DEST);
    m_cmdList->CopyBufferRegion(dst->GetResource(), destinationOffset,
        src->GetResource(), sourceOffset, bytes);
    TransitionComputeBuffer(m_cmdList, dst, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return true;
}

void D3D12GraphicsContext::SetTexture(uint32_t slot, const Engine::Graphics::IGraphicsTexture* texture)
{
    // Logical slots 12-15 are the terrain layer arrays and paint map (t27-t30).
    const bool terrainArray = slot >= 12 && slot <= 15;
    // Logical slots 24-26: the terrain virtual texture arrays (t31-t33).
    const bool virtualTextureArray = slot >= 24 && slot <= 26;
    if (!m_cmdList || (slot >= 12 && !terrainArray && !virtualTextureArray && (slot < 16 || slot >= 24)))
        return;
    const auto* nullTexture = terrainArray || virtualTextureArray
        ? m_nullArrayTexture.get() : m_nullTexture.get();
    const auto* nativeTexture = dynamic_cast<const D3D12GraphicsTexture*>(texture ? texture : nullTexture);
    if (!nativeTexture)
        return;
    RetainTexture(nativeTexture);
    ID3D12DescriptorHeap* heaps[] = { nativeTexture->GetHeap() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    const uint32_t rootParameter = terrainArray ? slot + 16 : virtualTextureArray ? slot + 8 :
        slot == 10 ? 26 : slot == 11 ? 27 :
        slot >= 16 ? slot + 2 : slot == 6 ? 10 :
        (slot == 7 ? 13 : (slot == 8 ? 14 : (slot == 9 ? 17 : slot + 1)));
    m_cmdList->SetGraphicsRootDescriptorTable(
        rootParameter, nativeTexture->GetGpuHandle());
}

bool D3D12GraphicsContext::BeginDepthOnlyPass(
    const Engine::Graphics::IGraphicsTexture* texture, float clearDepth)
{
    if (!m_cmdList || m_activeDepthTarget) return false;
    auto* depth = dynamic_cast<D3D12GraphicsTexture*>(
        const_cast<Engine::Graphics::IGraphicsTexture*>(texture));
    if (!depth || !depth->IsDepthTarget()) return false;
    RetainTexture(depth);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = static_cast<ID3D12Resource*>(depth->GetNativeHandle());
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);
    const auto dsv = depth->GetDsvHandle();
    m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    if (clearDepth >= 0.f)
        m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH,
            clearDepth, 0, 0, nullptr);
    m_activeDepthTarget = depth;
    return true;
}

void D3D12GraphicsContext::ClearDepthRectangle(const ScissorRect& rect,float depth)
{
    if(!m_cmdList || !m_activeDepthTarget || rect.left>=rect.right || rect.top>=rect.bottom) return;
    const D3D12_RECT area{rect.left,rect.top,rect.right,rect.bottom};
    m_cmdList->ClearDepthStencilView(m_activeDepthTarget->GetDsvHandle(),
        D3D12_CLEAR_FLAG_DEPTH,depth,0,1,&area);
}

void D3D12GraphicsContext::EndDepthOnlyPass()
{
    if (!m_cmdList || !m_activeDepthTarget) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = static_cast<ID3D12Resource*>(
        m_activeDepthTarget->GetNativeHandle());
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);
    m_activeDepthTarget = nullptr;
    if(m_restoreDepthOnlyPass) m_restoreDepthOnlyPass();
}

bool D3D12GraphicsContext::BeginColorPass(const Engine::Graphics::IGraphicsTexture* const* targets,
    uint32_t count, uint32_t slice, const Engine::Graphics::IGraphicsTexture* depthTexture)
{
    if (!m_cmdList || m_activeDepthTarget || m_activeColorCount || !targets || !count || count > 4)
        return false;
    std::array<D3D12GraphicsTexture*, 4> colors{};
    for (uint32_t i = 0; i < count; ++i)
    {
        colors[i] = dynamic_cast<D3D12GraphicsTexture*>(const_cast<Engine::Graphics::IGraphicsTexture*>(targets[i]));
        if (!colors[i] || !colors[i]->IsColorTarget() || slice >= colors[i]->GetRenderTargetSlices())
            return false;
    }
    auto* depth = depthTexture ? dynamic_cast<D3D12GraphicsTexture*>(
        const_cast<Engine::Graphics::IGraphicsTexture*>(depthTexture)) : nullptr;
    if (depthTexture && (!depth || !depth->IsDepthTarget())) return false;
    std::array<D3D12_RESOURCE_BARRIER, 5> barriers{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtvs{};
    uint32_t barrierCount = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        RetainTexture(colors[i]);
        auto& barrier = barriers[barrierCount++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = static_cast<ID3D12Resource*>(colors[i]->GetNativeHandle());
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        rtvs[i] = colors[i]->GetRtvHandle(slice);
    }
    if (depth)
    {
        RetainTexture(depth);
        auto& barrier = barriers[barrierCount++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = static_cast<ID3D12Resource*>(depth->GetNativeHandle());
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    m_cmdList->ResourceBarrier(barrierCount, barriers.data());
    const auto dsv = depth ? depth->GetDsvHandle() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    m_cmdList->OMSetRenderTargets(count, rtvs.data(), FALSE, depth ? &dsv : nullptr);
    m_activeColorTargets = colors;
    m_activeColorCount = count;
    m_activeColorSlice = slice;
    m_activeColorDepth = depth;
    return true;
}

void D3D12GraphicsContext::ClearColorPassRectangle(const ScissorRect& rect, const float* color, float depth)
{
    if (!m_cmdList || !m_activeColorCount || rect.left >= rect.right || rect.top >= rect.bottom) return;
    const D3D12_RECT area{rect.left, rect.top, rect.right, rect.bottom};
    if (color)
        for (uint32_t i = 0; i < m_activeColorCount; ++i)
            m_cmdList->ClearRenderTargetView(m_activeColorTargets[i]->GetRtvHandle(m_activeColorSlice),
                color, 1, &area);
    if (m_activeColorDepth)
        m_cmdList->ClearDepthStencilView(m_activeColorDepth->GetDsvHandle(), D3D12_CLEAR_FLAG_DEPTH,
            depth, 0, 1, &area);
}

void D3D12GraphicsContext::EndColorPass()
{
    if (!m_cmdList || !m_activeColorCount) return;
    std::array<D3D12_RESOURCE_BARRIER, 5> barriers{};
    uint32_t barrierCount = 0;
    for (uint32_t i = 0; i < m_activeColorCount; ++i)
    {
        auto& barrier = barriers[barrierCount++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = static_cast<ID3D12Resource*>(m_activeColorTargets[i]->GetNativeHandle());
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    if (m_activeColorDepth)
    {
        auto& barrier = barriers[barrierCount++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = static_cast<ID3D12Resource*>(m_activeColorDepth->GetNativeHandle());
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    m_cmdList->ResourceBarrier(barrierCount, barriers.data());
    m_activeColorTargets = {};
    m_activeColorCount = 0;
    m_activeColorDepth = nullptr;
    if (m_restoreDepthOnlyPass) m_restoreDepthOnlyPass();
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
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
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
    ID3D12RootSignature* rootSignature,
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> nullTexture,
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> nullArrayTexture)
    : m_device(device), m_commandQueue(commandQueue), m_rootSignature(rootSignature),
      m_occlusionState(std::make_shared<D3D12OcclusionQueryState>(device)),
      m_gpuTimings(std::make_shared<D3D12GpuTimingState>(device, commandQueue)),
      m_indirectSignatures(std::make_shared<D3D12IndirectSignatures>(device)),
      m_nullTexture(std::move(nullTexture)),
      m_nullArrayTexture(std::move(nullArrayTexture))
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
    auto result = std::make_unique<D3D12GraphicsContext>(
        list, m_rootSignature, m_occlusionState, m_gpuTimings, m_indirectSignatures, m_nullTexture, m_nullArrayTexture);
    result->SetTextureOwners(m_textureOwners);
    result->SetBufferOwners(m_bufferOwners);
    result->SetSceneDepth(m_sceneDepth);
    return result;
}

std::unique_ptr<Engine::Graphics::IGraphicsContext> D3D12GraphicsContextFactory::CreateContextForCommandBuffer(void* commandBuffer)
{
    if (!commandBuffer) return nullptr;
    auto result = std::make_unique<D3D12GraphicsContext>(
        static_cast<ID3D12GraphicsCommandList*>(commandBuffer), m_rootSignature,
        m_occlusionState, m_gpuTimings, m_indirectSignatures, m_nullTexture, m_nullArrayTexture);
    result->SetTextureOwners(m_textureOwners);
    result->SetBufferOwners(m_bufferOwners);
    result->SetSceneDepth(m_sceneDepth);
    return result;
}

void D3D12GraphicsContextFactory::SetCommandBuffer(void* cmd)
{
    m_externalCmdList = static_cast<ID3D12GraphicsCommandList*>(cmd);
}

void D3D12GraphicsContextFactory::PrepareFrame(uint32_t frameSlot)
{
    m_preparedFrameSlot = frameSlot;
    m_textureOwners->Prepare(frameSlot);
    m_bufferOwners->Prepare(frameSlot);
    ++m_preparedFrameSerial;
    if (m_occlusionState) m_occlusionState->Prepare(frameSlot);
    if (m_gpuTimings) m_gpuTimings->Prepare(frameSlot);
}

void D3D12GraphicsContextFactory::FinalizeFrame()
{ if (m_gpuTimings) m_gpuTimings->Finalize(m_externalCmdList ? m_externalCmdList : m_cmdList.Get()); }
Engine::Graphics::FrameTimingTelemetry D3D12GraphicsContextFactory::GetFrameTimingTelemetry() const
{ return m_gpuTimings ? m_gpuTimings->telemetry : Engine::Graphics::FrameTimingTelemetry{}; }
}
