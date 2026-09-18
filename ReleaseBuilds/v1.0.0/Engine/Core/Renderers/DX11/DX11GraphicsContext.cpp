#include "pch.h"
#include "DX11GraphicsContext.h"
#include "DX11GraphicsBuffer.h"
#include "DX11PipelineStateBuilder.h"
#include "DX11GraphicsTexture.h"
#include <algorithm>
#include <cstring>

namespace Engine::Renderers
{
D3D11FrameResourceState::D3D11FrameResourceState(ID3D11Device* device,
    ID3D11DeviceContext* context)
{
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    arenaSupported = device && context &&
        SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1))) &&
        SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
            &options, sizeof(options))) &&
        options.MapNoOverwriteOnDynamicConstantBuffer;
    if (arenaSupported)
    {
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = ArenaSize;
        descriptor.Usage = D3D11_USAGE_DYNAMIC;
        descriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&descriptor, nullptr, &constantArena)))
            arenaSupported = false;
    }

    if (device)
    {
        D3D11_SAMPLER_DESC descriptor{};
        descriptor.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        descriptor.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        descriptor.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        descriptor.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        descriptor.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&descriptor, &materialSampler);
    }
}

void D3D11FrameResourceState::PrepareFrame()
{
    arenaCursor = 0;
    firstArenaWrite = true;
    arenaWrites = 0;
    discardMaps = 0;
}

D3D11GpuTimingState::D3D11GpuTimingState(ID3D11Device* timingDevice,
    ID3D11DeviceContext* timingContext)
    : device(timingDevice), context(timingContext)
{
}

void D3D11GpuTimingState::PrepareFrame()
{
    if (!device || !context)
        return;
    for (Frame& frame : frames)
    {
        if (!frame.pending || !frame.disjoint)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if (context->GetData(frame.disjoint.Get(), &disjoint,
                sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        std::array<double, static_cast<size_t>(
            Engine::Graphics::GpuTimingStage::Count)> completed{};
        bool available = !disjoint.Disjoint && disjoint.Frequency != 0;
        for (uint32_t index = 0; available && index < frame.regionCount; ++index)
        {
            UINT64 begin = 0, end = 0;
            Region& region = frame.regions[index];
            available = context->GetData(region.begin.Get(), &begin,
                sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(region.end.Get(), &end,
                sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (available && end >= begin)
                completed[static_cast<size_t>(region.stage)] +=
                    static_cast<double>(end - begin) * 1000.0 /
                    static_cast<double>(disjoint.Frequency);
        }
        if (available)
        {
            telemetry.gpuMilliseconds = completed;
            ++telemetry.gpuSampleId;
            telemetry.gpuRegionCount = frame.regionCount;
            telemetry.gpuTimingsValid = true;
            frame.pending = false;
        }
        else if (disjoint.Disjoint)
            frame.pending = false;
    }

    frameIndex = (frameIndex + 1u) % BufferedFrames;
    Frame& frame = frames[frameIndex];
    if (frame.pending)
        return;
    if (!frame.disjoint)
    {
        D3D11_QUERY_DESC descriptor{};
        descriptor.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(device->CreateQuery(&descriptor, &frame.disjoint)))
            return;
    }
    frame.regionCount = 0;
    activeRegion = UINT32_MAX;
    recording = true;
    cpuSubmissionStart = std::chrono::steady_clock::now();
    context->Begin(frame.disjoint.Get());
}

void D3D11GpuTimingState::FinalizeFrame()
{
    if (!recording || !context)
        return;
    if (activeRegion != UINT32_MAX)
        End(frames[frameIndex].regions[activeRegion].stage);
    context->End(frames[frameIndex].disjoint.Get());
    frames[frameIndex].pending = true;
    telemetry.cpuSubmissionMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuSubmissionStart).count();
    recording = false;
}

void D3D11GpuTimingState::Begin(Engine::Graphics::GpuTimingStage stage)
{
    if (!recording || !device || !context || activeRegion != UINT32_MAX)
        return;
    Frame& frame = frames[frameIndex];
    if (frame.regionCount >= MaximumRegions)
        return;
    if (frame.regionCount == frame.regions.size())
    {
        D3D11_QUERY_DESC descriptor{};
        descriptor.Query = D3D11_QUERY_TIMESTAMP;
        Region region{};
        if (FAILED(device->CreateQuery(&descriptor, &region.begin)) ||
            FAILED(device->CreateQuery(&descriptor, &region.end)))
            return;
        frame.regions.push_back(std::move(region));
    }
    Region& region = frame.regions[frame.regionCount];
    region.stage = stage;
    activeRegion = frame.regionCount++;
    context->End(region.begin.Get());
}

void D3D11GpuTimingState::End(Engine::Graphics::GpuTimingStage stage)
{
    if (!recording || !context || activeRegion == UINT32_MAX)
        return;
    Region& region = frames[frameIndex].regions[activeRegion];
    if (region.stage != stage)
        return;
    context->End(region.end.Get());
    activeRegion = UINT32_MAX;
}

D3D11GraphicsContextFactory::D3D11GraphicsContextFactory(
    ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device), m_context(context),
      m_occlusionState(std::make_shared<D3D11OcclusionQueryState>()),
      m_frameResources(std::make_shared<D3D11FrameResourceState>(device, context)),
      m_gpuTimings(std::make_shared<D3D11GpuTimingState>(device, context))
{
}

D3D11GraphicsContext::D3D11GraphicsContext(ID3D11Device* device,
    ID3D11DeviceContext* context,
    std::shared_ptr<D3D11OcclusionQueryState> occlusionState,
    std::shared_ptr<D3D11FrameResourceState> frameResources,
    std::shared_ptr<D3D11GpuTimingState> gpuTimings)
    : m_device(device), m_context(context),
      m_occlusionState(std::move(occlusionState)),
      m_frameResources(std::move(frameResources)),
      m_gpuTimings(std::move(gpuTimings))
{
    if (m_context)
        m_context->QueryInterface(IID_PPV_ARGS(&m_context1));
}

void D3D11GraphicsContext::BeginGpuTiming(
    Engine::Graphics::GpuTimingStage stage)
{
    if (m_gpuTimings) m_gpuTimings->Begin(stage);
}

void D3D11GraphicsContext::EndGpuTiming(
    Engine::Graphics::GpuTimingStage stage)
{
    if (m_gpuTimings) m_gpuTimings->End(stage);
}

namespace
{
uint64_t OcclusionKey(uint64_t viewId, uint64_t objectId)
{
    objectId ^= viewId + 0x9e3779b97f4a7c15ull +
        (objectId << 6u) + (objectId >> 2u);
    return objectId;
}
}

bool D3D11GraphicsContext::BeginOcclusionFrame(uint64_t viewId,
    uint64_t sceneSignature)
{
    m_occlusionViewId = viewId;
    m_occlusionSceneSignature = sceneSignature;
    m_activeOcclusionQuery = nullptr;
    if (!m_context || !m_occlusionState)
        return false;

    ++m_occlusionState->frameIndex;
    const auto previous = m_occlusionState->viewSignatures.find(viewId);
    const bool invalidated = previous == m_occlusionState->viewSignatures.end() ||
        previous->second != sceneSignature;
    m_occlusionState->viewSignatures[viewId] = sceneSignature;

    for (auto iterator = m_occlusionState->entries.begin();
        iterator != m_occlusionState->entries.end();)
    {
        D3D11OcclusionQueryState::Entry& entry = iterator->second;
        if (entry.viewId == viewId && entry.pending && entry.query)
        {
            UINT64 visibleSamples = 0;
            const HRESULT status = m_context->GetData(entry.query.Get(),
                &visibleSamples, sizeof(visibleSamples),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (status == S_OK)
            {
                entry.pending = false;
                if (!invalidated && entry.issueSignature == sceneSignature)
                    entry.occluded = visibleSamples == 0;
            }
        }
        if (entry.viewId == viewId && invalidated)
            entry.occluded = false;

        const bool stale = !entry.pending &&
            m_occlusionState->frameIndex > entry.lastTouchedFrame + 600u;
        if (stale)
            iterator = m_occlusionState->entries.erase(iterator);
        else
            ++iterator;
    }
    return !invalidated;
}

bool D3D11GraphicsContext::IsOccluded(uint64_t objectId)
{
    if (!m_occlusionState || !m_occlusionViewId || !objectId)
        return false;
    const auto found = m_occlusionState->entries.find(
        OcclusionKey(m_occlusionViewId, objectId));
    if (found == m_occlusionState->entries.end() ||
        found->second.viewId != m_occlusionViewId)
        return false;
    auto& entry = found->second;
    entry.lastTouchedFrame = m_occlusionState->frameIndex;
    // Hidden objects are periodically rendered again so animated occluders or
    // topology changes cannot leave an object suppressed indefinitely.
    constexpr uint64_t reprobeInterval = 8u;
    if (entry.occluded && m_occlusionState->frameIndex <
        entry.lastProbeFrame + reprobeInterval)
        return true;
    entry.occluded = false;
    return false;
}

void D3D11GraphicsContext::BeginOcclusionQuery(uint64_t objectId)
{
    m_activeOcclusionQuery = nullptr;
    if (!m_device || !m_context || !m_occlusionState ||
        !m_occlusionViewId || !objectId)
        return;

    const uint64_t key = OcclusionKey(m_occlusionViewId, objectId);
    auto [iterator, inserted] = m_occlusionState->entries.try_emplace(key);
    auto& entry = iterator->second;
    if (!inserted && entry.viewId != m_occlusionViewId)
        entry = {};
    entry.viewId = m_occlusionViewId;
    entry.lastTouchedFrame = m_occlusionState->frameIndex;
    if (entry.pending)
        return;
    if (!entry.query)
    {
        D3D11_QUERY_DESC descriptor{};
        descriptor.Query = D3D11_QUERY_OCCLUSION;
        if (FAILED(m_device->CreateQuery(&descriptor, &entry.query)))
            return;
    }
    m_context->Begin(entry.query.Get());
    entry.lastProbeFrame = m_occlusionState->frameIndex;
    m_activeOcclusionQuery = &entry;
}

void D3D11GraphicsContext::EndOcclusionQuery()
{
    if (!m_context || !m_activeOcclusionQuery ||
        !m_activeOcclusionQuery->query)
        return;
    m_context->End(m_activeOcclusionQuery->query.Get());
    m_activeOcclusionQuery->pending = true;
    m_activeOcclusionQuery->issueSignature = m_occlusionSceneSignature;
    m_activeOcclusionQuery = nullptr;
}

void D3D11GraphicsContext::SetPipeline(const Engine::Graphics::IPipelineState* state)
{
    if (!m_context || !state) return;
    if (state == m_boundPipeline)
        return;
    const auto* pipeline = dynamic_cast<const D3D11PipelineState*>(state);
    if (!pipeline) return;

    m_context->VSSetShader(pipeline->vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(pipeline->pixelShader.Get(), nullptr, 0);
    m_context->IASetInputLayout(pipeline->inputLayout.Get());
    m_context->IASetPrimitiveTopology(pipeline->topology);
    m_context->RSSetState(pipeline->rasterizerState.Get());
    const float blendFactor[4]{};
    m_context->OMSetBlendState(pipeline->blendState.Get(), blendFactor, UINT_MAX);
    m_depthStencilState = pipeline->depthStencilState;
    m_context->OMSetDepthStencilState(m_depthStencilState.Get(), m_stencilReference);
    m_boundPipeline = state;
}

void D3D11GraphicsContext::SetStencilReference(uint32_t reference)
{
    m_stencilReference = reference;
    if (m_context)
        m_context->OMSetDepthStencilState(m_depthStencilState.Get(), reference);
}

void D3D11GraphicsContext::SetConstantBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint64_t offset)
{
    if (!m_device || !m_context || !buffer || !m_frameResources ||
        slot >= D3D11FrameResourceState::ConstantSlots) return;
    const auto* source = dynamic_cast<const D3D11GraphicsBuffer*>(buffer);
    if (!source || !source->GetShadowData() || offset >= source->GetSize()) return;

    const size_t available = static_cast<size_t>(source->GetSize() - offset);
    const size_t copySize = std::min<size_t>(
        D3D11FrameResourceState::ConstantSize, available);
    if (m_frameResources->arenaSupported &&
        m_frameResources->constantArena && m_context1)
    {
        if (m_frameResources->arenaCursor +
            D3D11FrameResourceState::ConstantSize >
            D3D11FrameResourceState::ArenaSize)
        {
            m_frameResources->arenaCursor = 0;
            m_frameResources->firstArenaWrite = true;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const D3D11_MAP mapType = m_frameResources->firstArenaWrite
            ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
        if (SUCCEEDED(m_context->Map(m_frameResources->constantArena.Get(), 0,
            mapType, 0, &mapped)))
        {
            auto* destination = static_cast<uint8_t*>(mapped.pData) +
                m_frameResources->arenaCursor;
            std::memset(destination, 0, D3D11FrameResourceState::ConstantSize);
            std::memcpy(destination, source->GetShadowData() + offset, copySize);
            m_context->Unmap(m_frameResources->constantArena.Get(), 0);

            ID3D11Buffer* native = m_frameResources->constantArena.Get();
            const UINT firstConstant = m_frameResources->arenaCursor / 16u;
            const UINT constantCount = D3D11FrameResourceState::ConstantSize / 16u;
            m_context1->VSSetConstantBuffers1(slot, 1, &native,
                &firstConstant, &constantCount);
            m_context1->PSSetConstantBuffers1(slot, 1, &native,
                &firstConstant, &constantCount);
            m_frameResources->arenaCursor +=
                D3D11FrameResourceState::ConstantSize;
            ++m_frameResources->arenaWrites;
            if (mapType == D3D11_MAP_WRITE_DISCARD)
                ++m_frameResources->discardMaps;
            m_frameResources->firstArenaWrite = false;
            return;
        }
    }

    auto& fallback = m_frameResources->fallbackConstantBuffers[slot];
    if (!fallback)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = D3D11FrameResourceState::ConstantSize;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(m_device->CreateBuffer(&desc, nullptr, &fallback));
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(m_context->Map(fallback.Get(), 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    ++m_frameResources->discardMaps;
    std::memset(mapped.pData, 0, D3D11FrameResourceState::ConstantSize);
    std::memcpy(mapped.pData, source->GetShadowData() + offset,
        copySize);
    m_context->Unmap(fallback.Get(), 0);

    ID3D11Buffer* native = fallback.Get();
    m_context->VSSetConstantBuffers(slot, 1, &native);
    m_context->PSSetConstantBuffers(slot, 1, &native);
}

void D3D11GraphicsContext::SetStructuredBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer)
{
    if (!m_context || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return;
    const auto* structured = dynamic_cast<const D3D11GraphicsBuffer*>(buffer);
    ID3D11ShaderResourceView* view = structured ? structured->GetShaderResourceView() : nullptr;
    m_context->VSSetShaderResources(slot, 1, &view);
    m_context->PSSetShaderResources(slot, 1, &view);
}

void D3D11GraphicsContext::SetVertexBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset)
{
    if (!m_context || !buffer || offset > UINT_MAX) return;
    const auto* nativeBuffer = dynamic_cast<const D3D11GraphicsBuffer*>(buffer);
    if (!nativeBuffer) return;
    ID3D11Buffer* native = nativeBuffer->GetBuffer();
    UINT nativeOffset = static_cast<UINT>(offset);
    m_context->IASetVertexBuffers(slot, 1, &native, &stride, &nativeOffset);
}

void D3D11GraphicsContext::SetIndexBuffer(const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t, uint64_t offset)
{
    if (!m_context || !buffer || offset > UINT_MAX) return;
    const auto* nativeBuffer = dynamic_cast<const D3D11GraphicsBuffer*>(buffer);
    if (nativeBuffer)
        m_context->IASetIndexBuffer(nativeBuffer->GetBuffer(), DXGI_FORMAT_R32_UINT, static_cast<UINT>(offset));
}

void D3D11GraphicsContext::SetTexture(uint32_t slot, const Engine::Graphics::IGraphicsTexture* texture)
{
    if (!m_device || !m_context || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
        return;
    // Logical texture slot 6 is the reflection panorama. Object shaders keep
    // t6-t8 available for structured scene buffers, so bind it at t9.
    const uint32_t shaderSlot = slot == 6 ? 9 : slot;
    const auto* nativeTexture = dynamic_cast<const D3D11GraphicsTexture*>(texture);
    ID3D11ShaderResourceView* view = nativeTexture ? nativeTexture->GetView() : nullptr;
    if (!m_textureSlotInitialized[shaderSlot] ||
        m_boundTextureViews[shaderSlot] != view)
    {
        m_context->PSSetShaderResources(shaderSlot, 1, &view);
        m_boundTextureViews[shaderSlot] = view;
        m_textureSlotInitialized[shaderSlot] = true;
    }

    if (!m_materialSamplerBound && m_frameResources &&
        m_frameResources->materialSampler)
    {
        ID3D11SamplerState* sampler = m_frameResources->materialSampler.Get();
        m_context->PSSetSamplers(0, 1, &sampler);
        m_materialSamplerBound = true;
    }
}

void D3D11GraphicsContext::SetViewport(const Viewport& value)
{
    if (!m_context) return;
    D3D11_VIEWPORT viewport{ value.x, value.y, value.width, value.height, value.minDepth, value.maxDepth };
    m_context->RSSetViewports(1, &viewport);
}

void D3D11GraphicsContext::SetScissorRect(const ScissorRect& value)
{
    if (!m_context) return;
    D3D11_RECT rect{ value.left, value.top, value.right, value.bottom };
    m_context->RSSetScissorRects(1, &rect);
}

void D3D11GraphicsContext::Clear(float, float, float, float, float) {}

void D3D11GraphicsContext::DrawInstanced(uint32_t vertices, uint32_t instances,
                                         uint32_t startVertex, uint32_t startInstance)
{
    if (m_context) m_context->DrawInstanced(vertices, instances, startVertex, startInstance);
}

void D3D11GraphicsContext::DrawIndexedInstanced(uint32_t indices, uint32_t instances,
                                                uint32_t startIndex, int32_t baseVertex,
                                                uint32_t startInstance)
{
    if (m_context) m_context->DrawIndexedInstanced(indices, instances, startIndex, baseVertex, startInstance);
}

void D3D11GraphicsContext::TransitionResource(void*, ResourceState, ResourceState) {}
}
