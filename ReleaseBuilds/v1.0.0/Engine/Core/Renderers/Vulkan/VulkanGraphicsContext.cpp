#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGraphicsContext.h"
#include "VulkanGraphicsBuffer.h"
#include "VulkanPipelineStateBuilder.h"
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>


namespace Engine::Renderers
{
struct VulkanOcclusionQueryState
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
    struct Record { uint64_t key = 0; uint64_t signature = 0; };
    struct FrameSlot
    {
        VkQueryPool pool = VK_NULL_HANDLE;
        std::vector<uint64_t> results;
        std::vector<Record> records;
        uint32_t used = 0;
    };

    explicit VulkanOcclusionQueryState(VkDevice value) : device(value) {}
    ~VulkanOcclusionQueryState()
    {
        if (device)
            for (auto& slot : slots)
                if (slot.pool) vkDestroyQueryPool(device, slot.pool, nullptr);
    }

    bool EnsureSlot(uint32_t frameSlot)
    {
        if (!device) return false;
        if (slots.size() <= frameSlot) slots.resize(frameSlot + 1u);
        auto& slot = slots[frameSlot];
        if (slot.pool) return true;
        VkQueryPoolCreateInfo createInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        createInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        createInfo.queryCount = QueryCapacity;
        if (vkCreateQueryPool(device, &createInfo, nullptr, &slot.pool) != VK_SUCCESS)
            return false;
        slot.results.resize(QueryCapacity);
        return true;
    }

    void Prepare(uint32_t frameSlot, VkCommandBuffer commandBuffer)
    {
        if (!commandBuffer || !EnsureSlot(frameSlot))
        {
            currentSlot = UINT32_MAX;
            return;
        }
        auto& slot = slots[frameSlot];
        VkResult result = VK_SUCCESS;
        if (slot.used)
            result = vkGetQueryPoolResults(device, slot.pool, 0, slot.used,
                VkDeviceSize(slot.used) * sizeof(uint64_t), slot.results.data(),
                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        for (uint32_t index = 0; index < slot.used; ++index)
        {
            const auto& record = slot.records[index];
            const auto found = entries.find(record.key);
            if (found == entries.end()) continue;
            auto& entry = found->second;
            if (entry.pending && entry.issueSignature == record.signature)
            {
                entry.pending = false;
                entry.occluded = result == VK_SUCCESS && slot.results[index] == 0;
            }
        }
        slot.used = 0;
        slot.records.clear();
        vkCmdResetQueryPool(commandBuffer, slot.pool, 0, QueryCapacity);
        currentSlot = frameSlot;
    }

    VkDevice device = VK_NULL_HANDLE;
    std::vector<FrameSlot> slots;
    std::unordered_map<uint64_t, Entry> entries;
    std::unordered_map<uint64_t, uint64_t> viewSignatures;
    uint64_t frameIndex = 0;
    uint32_t currentSlot = UINT32_MAX;
};

struct VulkanGpuTimingState
{
    static constexpr uint32_t QueryCapacity = 128;
    struct Record { Engine::Graphics::GpuTimingStage stage{}; uint32_t begin = 0; uint32_t end = 0; };
    struct FrameSlot { VkQueryPool pool = VK_NULL_HANDLE; std::vector<Record> records; std::vector<uint64_t> results; uint32_t used = 0; bool pending = false; };
    VulkanGpuTimingState(VkDevice value, float period) : device(value), timestampPeriod(period) {}
    ~VulkanGpuTimingState()
    { if (device) for (auto& slot : slots) if (slot.pool) vkDestroyQueryPool(device, slot.pool, nullptr); }
    bool EnsureSlot(uint32_t index)
    {
        if (!device) return false; if (slots.size() <= index) slots.resize(index + 1u);
        auto& slot = slots[index]; if (slot.pool) return true;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = QueryCapacity;
        if (vkCreateQueryPool(device, &info, nullptr, &slot.pool) != VK_SUCCESS) return false;
        slot.results.resize(QueryCapacity); return true;
    }
    void Prepare(uint32_t index, VkCommandBuffer command)
    {
        if (!command || !EnsureSlot(index)) { currentSlot = UINT32_MAX; return; }
        auto& slot = slots[index];
        if (slot.pending && slot.used && timestampPeriod > 0.f)
        {
            const VkResult result = vkGetQueryPoolResults(device, slot.pool, 0, slot.used,
                VkDeviceSize(slot.used) * sizeof(uint64_t), slot.results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result == VK_SUCCESS)
            {
                telemetry.gpuMilliseconds.fill(0.0);
                for (const auto& record : slot.records)
                    if (slot.results[record.end] >= slot.results[record.begin])
                        telemetry.gpuMilliseconds[static_cast<size_t>(record.stage)] +=
                            double(slot.results[record.end] - slot.results[record.begin]) *
                            double(timestampPeriod) / 1000000.0;
                telemetry.gpuTimingsValid = true; telemetry.gpuRegionCount = static_cast<uint32_t>(slot.records.size());
                ++telemetry.gpuSampleId;
            }
        }
        slot.used = 0; slot.records.clear(); slot.pending = false;
        vkCmdResetQueryPool(command, slot.pool, 0, QueryCapacity);
        currentSlot = index; active = UINT32_MAX; cpuStart = std::chrono::steady_clock::now();
    }
    void Begin(VkCommandBuffer command, Engine::Graphics::GpuTimingStage stage)
    {
        if (!command || currentSlot >= slots.size() || active != UINT32_MAX) return;
        auto& slot = slots[currentSlot]; if (slot.used + 2 > QueryCapacity) return;
        Record record{stage, slot.used++, slot.used++}; slot.records.push_back(record);
        active = static_cast<uint32_t>(slot.records.size() - 1u);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.pool, record.begin);
    }
    void End(VkCommandBuffer command, Engine::Graphics::GpuTimingStage stage)
    {
        if (!command || currentSlot >= slots.size() || active == UINT32_MAX) return;
        auto& slot = slots[currentSlot]; auto& record = slot.records[active]; if (record.stage != stage) return;
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.pool, record.end); active = UINT32_MAX;
    }
    void Finalize(VkCommandBuffer command)
    {
        if (!command || currentSlot >= slots.size()) return; auto& slot = slots[currentSlot];
        if (active != UINT32_MAX) End(command, slot.records[active].stage);
        slot.pending = slot.used != 0;
        telemetry.cpuSubmissionMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuStart).count();
    }
    VkDevice device = VK_NULL_HANDLE; float timestampPeriod = 0.f; std::vector<FrameSlot> slots;
    uint32_t currentSlot = UINT32_MAX, active = UINT32_MAX;
    Engine::Graphics::FrameTimingTelemetry telemetry{}; std::chrono::steady_clock::time_point cpuStart{};
};

namespace
{
uint64_t VulkanOcclusionKey(uint64_t viewId, uint64_t objectId)
{
    objectId ^= viewId + 0x9e3779b97f4a7c15ull +
        (objectId << 6u) + (objectId >> 2u);
    return objectId;
}
}

void VulkanGraphicsContextFactory::SetDevice(VkDevice device, float timestampPeriod)
{
    m_occlusionState = std::make_shared<VulkanOcclusionQueryState>(device);
    m_gpuTimings = std::make_shared<VulkanGpuTimingState>(device, timestampPeriod);
}

void VulkanGraphicsContextFactory::PrepareFrame(uint32_t frameSlot)
{
    if (m_occlusionState)
        m_occlusionState->Prepare(frameSlot, m_commandBuffer);
    if (m_gpuTimings)
        m_gpuTimings->Prepare(frameSlot, m_commandBuffer);
}

void VulkanGraphicsContextFactory::FinalizeFrame()
{ if (m_gpuTimings) m_gpuTimings->Finalize(m_commandBuffer); }
Engine::Graphics::FrameTimingTelemetry VulkanGraphicsContextFactory::GetFrameTimingTelemetry() const
{ return m_gpuTimings ? m_gpuTimings->telemetry : Engine::Graphics::FrameTimingTelemetry{}; }

void VulkanGraphicsContext::BeginGpuTiming(Engine::Graphics::GpuTimingStage stage)
{ if (m_gpuTimings) m_gpuTimings->Begin(m_commandBuffer, stage); }
void VulkanGraphicsContext::EndGpuTiming(Engine::Graphics::GpuTimingStage stage)
{ if (m_gpuTimings) m_gpuTimings->End(m_commandBuffer, stage); }

bool VulkanGraphicsContext::BeginOcclusionFrame(uint64_t viewId,
    uint64_t sceneSignature)
{
    m_occlusionViewId = viewId;
    m_occlusionSceneSignature = sceneSignature;
    m_activeOcclusionKey = 0;
    m_activeOcclusionIndex = UINT32_MAX;
    if (!m_commandBuffer || !m_occlusionState ||
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
        if (entry.viewId == viewId && invalidated) entry.occluded = false;
        const bool stale = !entry.pending && m_occlusionState->frameIndex >
            entry.lastTouchedFrame + 600u;
        if (stale) iterator = m_occlusionState->entries.erase(iterator);
        else ++iterator;
    }
    return !invalidated;
}

bool VulkanGraphicsContext::IsOccluded(uint64_t objectId)
{
    if (!m_occlusionState || !m_occlusionViewId || !objectId) return false;
    const auto found = m_occlusionState->entries.find(
        VulkanOcclusionKey(m_occlusionViewId, objectId));
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

void VulkanGraphicsContext::BeginOcclusionQuery(uint64_t objectId)
{
    m_activeOcclusionKey = 0;
    m_activeOcclusionIndex = UINT32_MAX;
    if (!m_commandBuffer || !m_occlusionState || !m_occlusionViewId || !objectId ||
        m_occlusionState->currentSlot >= m_occlusionState->slots.size())
        return;
    auto& slot = m_occlusionState->slots[m_occlusionState->currentSlot];
    if (!slot.pool || slot.used >= VulkanOcclusionQueryState::QueryCapacity)
        return;
    const uint64_t key = VulkanOcclusionKey(m_occlusionViewId, objectId);
    auto [iterator, inserted] = m_occlusionState->entries.try_emplace(key);
    auto& entry = iterator->second;
    if (!inserted && entry.viewId != m_occlusionViewId) entry = {};
    entry.viewId = m_occlusionViewId;
    entry.lastTouchedFrame = m_occlusionState->frameIndex;
    if (entry.pending) return;
    m_activeOcclusionIndex = slot.used++;
    m_activeOcclusionKey = key;
    vkCmdBeginQuery(m_commandBuffer, slot.pool, m_activeOcclusionIndex, 0);
    entry.lastProbeFrame = m_occlusionState->frameIndex;
}

void VulkanGraphicsContext::EndOcclusionQuery()
{
    if (!m_commandBuffer || !m_occlusionState ||
        m_activeOcclusionIndex == UINT32_MAX ||
        m_occlusionState->currentSlot >= m_occlusionState->slots.size())
        return;
    auto& slot = m_occlusionState->slots[m_occlusionState->currentSlot];
    vkCmdEndQuery(m_commandBuffer, slot.pool, m_activeOcclusionIndex);
    auto& entry = m_occlusionState->entries[m_activeOcclusionKey];
    entry.pending = true;
    entry.issueSignature = m_occlusionSceneSignature;
    slot.records.push_back({ m_activeOcclusionKey, m_occlusionSceneSignature });
    m_activeOcclusionKey = 0;
    m_activeOcclusionIndex = UINT32_MAX;
}

void VulkanGraphicsContext::SetPipeline(const Engine::Graphics::IPipelineState* pipeline)
{
    m_pipeline = dynamic_cast<const VulkanPipelineState*>(pipeline);
    if (m_pipeline) vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->GetPipeline());
}

void VulkanGraphicsContext::SetStencilReference(uint32_t reference)
{
    vkCmdSetStencilReference(m_commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK,
        reference);
}

void VulkanGraphicsContext::SetConstantBuffer(uint32_t, const Engine::Graphics::IGraphicsBuffer* buffer, uint64_t offset)
{
    if (!m_pipeline || !buffer || offset >= buffer->GetSize()) return;
    const auto* vkBuffer = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
    if (!vkBuffer || !vkBuffer->GetMappedData()) return;
    const uint64_t requested = buffer->GetElementStride()
        ? buffer->GetElementStride()
        : buffer->GetSize() - offset;
    uint32_t size = static_cast<uint32_t>(std::min<uint64_t>(128, requested));
    size &= ~3u;
    vkCmdPushConstants(m_commandBuffer, m_pipeline->GetLayout(),
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size,
        vkBuffer->GetMappedData() + offset);
}

void VulkanGraphicsContext::SetVertexBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t, uint64_t offset)
{
    const auto* vkBuffer = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
    if (!vkBuffer) return;
    VkBuffer native = vkBuffer->GetBuffer(); VkDeviceSize nativeOffset = offset;
    vkCmdBindVertexBuffers(m_commandBuffer, slot, 1, &native, &nativeOffset);
}

void VulkanGraphicsContext::SetIndexBuffer(const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t, uint64_t offset)
{
    const auto* vkBuffer = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
    if (vkBuffer) vkCmdBindIndexBuffer(m_commandBuffer, vkBuffer->GetBuffer(), offset, VK_INDEX_TYPE_UINT32);
}

void VulkanGraphicsContext::SetTexture(uint32_t slot, const Engine::Graphics::IGraphicsTexture* texture)
{
    if (slot < m_textures.size())
        m_textures[slot] = dynamic_cast<const VulkanGraphicsTexture*>(texture);
}

void VulkanGraphicsContext::SetStructuredBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer)
{
    if (slot < 6 || slot > 8) return;
    m_structuredBuffers[slot - 6] = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
}

void VulkanGraphicsContext::SetViewport(const Viewport& value)
{
    VkViewport viewport{ value.x, value.y + value.height, value.width, -value.height, value.minDepth, value.maxDepth };
    vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);
}

void VulkanGraphicsContext::SetScissorRect(const ScissorRect& value)
{
    VkRect2D rect{ { value.left, value.top }, { static_cast<uint32_t>(value.right - value.left), static_cast<uint32_t>(value.bottom - value.top) } };
    vkCmdSetScissor(m_commandBuffer, 0, 1, &rect);
}

void VulkanGraphicsContext::DrawInstanced(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
{
    try
    {
        if (m_textureSystem && m_pipeline)
            m_textureSystem->Bind(m_commandBuffer, m_pipeline->GetLayout(), m_textures,
                m_structuredBuffers);
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[Vulkan] Material binding failed: ") +
            error.what() + "\n").c_str());
        return;
    }
    vkCmdDraw(m_commandBuffer, vertices, instances, firstVertex, firstInstance);
}
void VulkanGraphicsContext::DrawIndexedInstanced(uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    try
    {
        if (m_textureSystem && m_pipeline)
            m_textureSystem->Bind(m_commandBuffer, m_pipeline->GetLayout(), m_textures,
                m_structuredBuffers);
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[Vulkan] Material binding failed: ") +
            error.what() + "\n").c_str());
        return;
    }
    vkCmdDrawIndexed(m_commandBuffer, indices, instances, firstIndex, vertexOffset, firstInstance);
}
void VulkanGraphicsContext::TransitionResource(void*, ResourceState, ResourceState) {}
}
#endif
