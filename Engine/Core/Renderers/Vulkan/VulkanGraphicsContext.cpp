#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGraphicsContext.h"
#include "VulkanRenderPassScope.h"
#include "VulkanGraphicsBuffer.h"
#include "VulkanPipelineStateBuilder.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <vector>


namespace Engine::Renderers
{
struct VulkanComputeDescriptorArena
{
    explicit VulkanComputeDescriptorArena(VkDevice value) : device(value) {}
    ~VulkanComputeDescriptorArena()
    {
        for (const auto& entry : pools)
            vkDestroyDescriptorPool(device, entry.second, nullptr);
    }
    void Prepare(uint32_t slot)
    {
        currentSlot = slot;
        auto& pool = pools[slot];
        if (!pool)
        {
            VkDescriptorPoolSize size{};
            size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            size.descriptorCount = 3 * 8192;
            VkDescriptorPoolCreateInfo info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            info.maxSets = 8192;
            info.poolSizeCount = 1;
            info.pPoolSizes = &size;
            VkCheck(vkCreateDescriptorPool(device, &info, nullptr, &pool),
                "vkCreateDescriptorPool(compute)");
        }
        else
            VkCheck(vkResetDescriptorPool(device, pool, 0),
                "vkResetDescriptorPool(compute)");
    }
    VkDescriptorSet Allocate(VkDescriptorSetLayout layout)
    {
        const auto entry = pools.find(currentSlot);
        if (entry == pools.end()) return VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        info.descriptorPool = entry->second;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkCheck(vkAllocateDescriptorSets(device, &info, &set),
            "vkAllocateDescriptorSets(compute)");
        return set;
    }
    VkDevice device = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkDescriptorPool> pools;
    uint32_t currentSlot = 0;
};
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
    static constexpr uint32_t QueryCapacity = 2048;
    struct Record { Engine::Graphics::GpuTimingStage stage{}; uint32_t begin = 0; uint32_t end = 0; };
    static constexpr uint32_t StatisticsCapacity = 16384;
    struct StatisticRecord { Engine::Graphics::GpuTimingStage stage; };
    struct FrameSlot { VkQueryPool statisticsPool = VK_NULL_HANDLE; std::vector<StatisticRecord> statisticsRecords;
        std::vector<uint64_t> statisticsResults; bool statisticsOverflow = false;
        VkQueryPool pool = VK_NULL_HANDLE; std::vector<Record> records; std::vector<uint64_t> results; uint32_t used = 0; bool pending = false; bool overflow = false; };
    VulkanGpuTimingState(VkDevice value, float period, bool supported) : device(value), timestampPeriod(period),
        collectWorkload(supported && std::getenv("ENGINE_GPU_WORKLOAD_COUNTERS") != nullptr) {}
    ~VulkanGpuTimingState()
    { if (device) for (auto& slot : slots) {
        if (slot.pool) vkDestroyQueryPool(device, slot.pool, nullptr);
        if (slot.statisticsPool) vkDestroyQueryPool(device, slot.statisticsPool, nullptr);
    } }
    bool EnsureSlot(uint32_t index)
    {
        if (!device) return false; if (slots.size() <= index) slots.resize(index + 1u);
        auto& slot = slots[index]; if (slot.pool) return true;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = QueryCapacity;
        if (vkCreateQueryPool(device, &info, nullptr, &slot.pool) != VK_SUCCESS) return false;
        slot.results.resize(QueryCapacity);
        if (collectWorkload) {
            info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            info.queryCount = StatisticsCapacity;
            info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
            if (vkCreateQueryPool(device, &info, nullptr, &slot.statisticsPool) != VK_SUCCESS) {
                vkDestroyQueryPool(device, slot.pool, nullptr); slot.pool = VK_NULL_HANDLE;
                return false;
            }
            slot.statisticsResults.resize(size_t(StatisticsCapacity) * 3);
        }
        return true;
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
                telemetry.gpuWorkload = {};
                telemetry.gpuWorkloadValid = false;
                if (collectWorkload && !slot.statisticsOverflow && !slot.statisticsRecords.empty()) {
                    const auto count = static_cast<uint32_t>(slot.statisticsRecords.size());
                    const auto status = vkGetQueryPoolResults(device, slot.statisticsPool, 0, count,
                        size_t(count) * 3 * sizeof(uint64_t), slot.statisticsResults.data(),
                        3 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
                    telemetry.gpuWorkloadValid = status == VK_SUCCESS;
                    if (telemetry.gpuWorkloadValid) for (size_t i=0; i<count; ++i) {
                        auto& workload = telemetry.gpuWorkload[static_cast<size_t>(slot.statisticsRecords[i].stage)];
                        // Vulkan returns counters in ascending statistic-bit order.
                        workload.primitives += slot.statisticsResults[i*3];
                        workload.vertices += slot.statisticsResults[i*3+1];
                        workload.fragments += slot.statisticsResults[i*3+2];
                    }
                }
                telemetry.gpuMilliseconds.fill(0.0);
                for (const auto& record : slot.records)
                    if (slot.results[record.end] >= slot.results[record.begin])
                        telemetry.gpuMilliseconds[static_cast<size_t>(record.stage)] +=
                            double(slot.results[record.end] - slot.results[record.begin]) *
                            double(timestampPeriod) / 1000000.0;
                telemetry.gpuTimingsValid = !slot.overflow;
                if (slot.overflow) telemetry.gpuWorkloadValid = false; telemetry.gpuRegionCount = static_cast<uint32_t>(slot.records.size());
                ++telemetry.gpuSampleId;
            }
        }
        slot.used = 0; slot.records.clear(); slot.pending = false; slot.overflow = false;
        slot.statisticsRecords.clear(); slot.statisticsOverflow = false;
        vkCmdResetQueryPool(command, slot.pool, 0, QueryCapacity);
        if (collectWorkload) vkCmdResetQueryPool(command, slot.statisticsPool, 0, StatisticsCapacity);
        currentSlot = index; active = UINT32_MAX; cpuStart = std::chrono::steady_clock::now();
    }
    void Begin(VkCommandBuffer command, Engine::Graphics::GpuTimingStage stage)
    {
        if (!command || currentSlot >= slots.size() || active != UINT32_MAX) return;
        auto& slot = slots[currentSlot]; if (slot.used + 2 > QueryCapacity) { slot.overflow = true; return; }
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
    // Scope statistics to individual physical draws: instancing compute can pause
    // and resume render passes inside one timestamp region. Queries must not span
    // those render-pass boundaries.
    bool BeginDraw(VkCommandBuffer command)
    {
        if (!collectWorkload || currentSlot >= slots.size() || active == UINT32_MAX) return false;
        auto& slot = slots[currentSlot];
        if (slot.statisticsRecords.size() >= StatisticsCapacity) { slot.statisticsOverflow=true; return false; }
        const auto index=static_cast<uint32_t>(slot.statisticsRecords.size());
        slot.statisticsRecords.push_back({slot.records[active].stage});
        vkCmdBeginQuery(command, slot.statisticsPool, index, 0);
        return true;
    }
    void EndDraw(VkCommandBuffer command)
    {
        auto& slot=slots[currentSlot];
        vkCmdEndQuery(command, slot.statisticsPool, static_cast<uint32_t>(slot.statisticsRecords.size()-1));
    }
    void Finalize(VkCommandBuffer command)
    {
        if (!command || currentSlot >= slots.size()) return; auto& slot = slots[currentSlot];
        if (active != UINT32_MAX) End(command, slot.records[active].stage);
        slot.pending = slot.used != 0;
        telemetry.cpuSubmissionMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuStart).count();
    }
    const bool collectWorkload;
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

void VulkanGraphicsContextFactory::SetDevice(VkDevice device, float timestampPeriod, bool pipelineStatisticsEnabled)
{
    m_occlusionState = std::make_shared<VulkanOcclusionQueryState>(device);
    m_gpuTimings = std::make_shared<VulkanGpuTimingState>(device, timestampPeriod, pipelineStatisticsEnabled);
    m_computeDescriptors = std::make_shared<VulkanComputeDescriptorArena>(device);
}

void VulkanGraphicsContextFactory::PrepareFrame(uint32_t frameSlot)
{ PrepareFrameForCommandBuffer(frameSlot, m_commandBuffer); }

void VulkanGraphicsContextFactory::PrepareFrameForCommandBuffer(
    uint32_t frameSlot, VkCommandBuffer commandBuffer)
{
    m_preparedFrameSlot = frameSlot;
    m_textureOwners->Prepare(frameSlot);
    ++m_preparedFrameSerial;
    if(m_textureSystem) m_textureSystem->PrepareFrame(frameSlot);
    if (m_occlusionState)
        m_occlusionState->Prepare(frameSlot, commandBuffer);
    if (m_gpuTimings)
        m_gpuTimings->Prepare(frameSlot, commandBuffer);
    if (m_computeDescriptors)
        m_computeDescriptors->Prepare(frameSlot);
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
    RetainTexture(texture);
    if (slot < m_textures.size())
        m_textures[slot] = dynamic_cast<const VulkanGraphicsTexture*>(texture);
}

bool VulkanGraphicsContext::BeginDepthOnlyPass(
    const Engine::Graphics::IGraphicsTexture* texture, float clearDepth)
{
    if (!m_commandBuffer || m_activeDepthTarget) return false;
    auto* depth = dynamic_cast<const VulkanGraphicsTexture*>(texture);
    if (!depth || !depth->IsDepthTarget()) return false;
    RetainTexture(depth);
    VkClearValue clears[2]{};
    clears[1].depthStencil = { clearDepth < 0.f ? 1.f : clearDepth, 0 };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    // A negative clear depth requests preservation of an existing atlas.
    // Partial directional-cascade refreshes use the same contract as DX11/12.
    begin.renderPass = clearDepth < 0.f
        ? depth->GetLoadRenderPass() : depth->GetRenderPass();
    begin.framebuffer = depth->GetFramebuffer();
    begin.renderArea.extent = { depth->GetWidth(), depth->GetHeight() };
    begin.clearValueCount = 2;
    begin.pClearValues = clears;
    vkCmdBeginRenderPass(m_commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    m_activeDepthTarget = depth;
    return true;
}

void VulkanGraphicsContext::ClearDepthRectangle(const ScissorRect& rect,float depth)
{
    if(!m_commandBuffer || !m_activeDepthTarget || rect.left>=rect.right || rect.top>=rect.bottom) return;
    VkClearAttachment attachment{};
    attachment.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    attachment.clearValue.depthStencil={depth,0};
    VkClearRect area{};
    area.rect.offset={rect.left,rect.top};
    area.rect.extent={uint32_t(rect.right-rect.left),uint32_t(rect.bottom-rect.top)};
    area.layerCount=1;
    vkCmdClearAttachments(m_commandBuffer,1,&attachment,1,&area);
}

void VulkanGraphicsContext::EndDepthOnlyPass()
{
    if (!m_commandBuffer || !m_activeDepthTarget) return;
    vkCmdEndRenderPass(m_commandBuffer);
    m_activeDepthTarget = nullptr;
}

void VulkanGraphicsContext::SetStructuredBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer)
{
    size_t index = 0;
    if (slot >= 6 && slot <= 8)
        index = slot - 6;
    else if (slot >= 10 && slot <= 11)
        index = slot - 7;
    else if (slot >= 14 && slot <= 15)
        index = slot - 9;
    else if (slot == 45 || slot == 46)
        index = slot-38;
    else
        return;
    m_structuredBuffers[index] = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
}

bool VulkanGraphicsContext::BeginComputePass()
{
    if (!m_commandBuffer || m_pausedDepthTarget || m_activeOcclusionIndex != UINT32_MAX) return false;
    if(m_activeDepthTarget)
    {
        if(!m_activeDepthTarget->GetLoadRenderPass()) return false;
        vkCmdEndRenderPass(m_commandBuffer);
        m_pausedDepthTarget=m_activeDepthTarget; m_activeDepthTarget=nullptr;
    }
    else PauseVulkanScenePass(m_commandBuffer);
    return true;
}
void VulkanGraphicsContext::EndComputePass()
{
    if(m_pausedDepthTarget)
    {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass=m_pausedDepthTarget->GetLoadRenderPass();
        begin.framebuffer=m_pausedDepthTarget->GetFramebuffer();
        begin.renderArea.extent={m_pausedDepthTarget->GetWidth(),m_pausedDepthTarget->GetHeight()};
        vkCmdBeginRenderPass(m_commandBuffer,&begin,VK_SUBPASS_CONTENTS_INLINE);
        m_activeDepthTarget=m_pausedDepthTarget; m_pausedDepthTarget=nullptr;
    }
    else ResumeVulkanScenePass(m_commandBuffer);
}
bool VulkanGraphicsContext::DrawInstancesIndirect(const Engine::Graphics::IGraphicsBuffer* buffer,
    uint64_t offset, bool indexed)
{
    const auto* native=dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
    if (!m_commandBuffer || !native || offset+(indexed?20u:16u)>buffer->GetSize()) return false;
    if (m_textureSystem && m_pipeline)
        m_textureSystem->Bind(m_commandBuffer,m_pipeline->GetLayout(),m_textures,m_structuredBuffers);
    const bool statistics=m_gpuTimings && m_gpuTimings->BeginDraw(m_commandBuffer);
    if(indexed) vkCmdDrawIndexedIndirect(m_commandBuffer,native->GetBuffer(),offset,1,20);
    else vkCmdDrawIndirect(m_commandBuffer,native->GetBuffer(),offset,1,16);
    if(statistics) m_gpuTimings->EndDraw(m_commandBuffer);
    return true;
}

bool VulkanGraphicsContext::SetComputePipeline(
    const Engine::Graphics::IPipelineState* pipeline)
{
    m_computePipeline = dynamic_cast<const VulkanComputePipeline*>(pipeline);
    if (!m_commandBuffer || !m_computePipeline || !m_computeDescriptors)
        return false;
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        m_computePipeline->GetPipeline());
    return true;
}

void VulkanGraphicsContext::SetComputeConstants(const void* data, uint32_t size)
{
    if (m_commandBuffer && m_computePipeline && data && size &&
        size <= 128 && size % 4 == 0)
        vkCmdPushConstants(m_commandBuffer, m_computePipeline->GetLayout(),
            VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
}

void VulkanGraphicsContext::SetComputeReadBuffer(
    const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t slot)
{
    if(slot==1) m_computePool=dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
    else m_computeRead = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
}

void VulkanGraphicsContext::SetComputeWriteBuffer(
    const Engine::Graphics::IGraphicsBuffer* buffer)
{
    m_computeWrite = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
}

void VulkanGraphicsContext::DispatchCompute(uint32_t x, uint32_t y, uint32_t z)
{
    if (!m_commandBuffer || !m_computePipeline || !m_computeDescriptors ||
        !m_computeWrite || !x || !y || !z) return;
    const VulkanGraphicsBuffer* read = m_computeRead ? m_computeRead : m_computeWrite;
    VkDescriptorSet set = m_computeDescriptors->Allocate(
        m_computePipeline->GetSetLayout());
    if (!set) return;
    VkDescriptorBufferInfo buffers[3]{};
    buffers[0] = { read->GetBuffer(), 0, VK_WHOLE_SIZE };
    buffers[1] = { m_computeWrite->GetBuffer(), 0, VK_WHOLE_SIZE };
    buffers[2] = { (m_computePool ? m_computePool:read)->GetBuffer(),0,VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t index = 0; index < 3; ++index)
    {
        writes[index] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[index].dstSet = set;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &buffers[index];
    }
    vkUpdateDescriptorSets(m_computeDescriptors->device, 3, writes, 0, nullptr);
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        m_computePipeline->GetLayout(), 0, 1, &set, 0, nullptr);
    vkCmdDispatch(m_commandBuffer, x, y, z);
}

void VulkanGraphicsContext::StorageBarrier(
    const Engine::Graphics::IGraphicsBuffer* buffer)
{
    const auto* native = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
    if (!m_commandBuffer || !native) return;
    VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = native->GetBuffer();
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(m_commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
}

bool VulkanGraphicsContext::CopyBuffer(
    const Engine::Graphics::IGraphicsBuffer* source,
    Engine::Graphics::IGraphicsBuffer* destination, uint64_t bytes)
{
    const auto* src = dynamic_cast<const VulkanGraphicsBuffer*>(source);
    auto* dst = dynamic_cast<VulkanGraphicsBuffer*>(destination);
    if (!m_commandBuffer || !src || !dst || destination->GetAccessMode() !=
        Engine::Graphics::IGraphicsBuffer::AccessMode::Readback)
        return false;
    const uint64_t size = bytes ? bytes : source->GetSize();
    if (!size || size > source->GetSize() || size > destination->GetSize())
        return false;
    VkBufferMemoryBarrier before{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.buffer = src->GetBuffer();
    before.size = size;
    vkCmdPipelineBarrier(m_commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &before, 0, nullptr);
    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(m_commandBuffer, src->GetBuffer(), dst->GetBuffer(),
        1, &region);
    VkBufferMemoryBarrier after = before;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    after.buffer = dst->GetBuffer();
    vkCmdPipelineBarrier(m_commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &after, 0, nullptr);
    return true;
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
    const bool statistics=m_gpuTimings && m_gpuTimings->BeginDraw(m_commandBuffer);
    vkCmdDraw(m_commandBuffer, vertices, instances, firstVertex, firstInstance);
    if(statistics) m_gpuTimings->EndDraw(m_commandBuffer);
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
    const bool statistics=m_gpuTimings && m_gpuTimings->BeginDraw(m_commandBuffer);
    vkCmdDrawIndexed(m_commandBuffer, indices, instances, firstIndex, vertexOffset, firstInstance);
    if(statistics) m_gpuTimings->EndDraw(m_commandBuffer);
}
void VulkanGraphicsContext::TransitionResource(void*, ResourceState, ResourceState) {}
}
#endif
