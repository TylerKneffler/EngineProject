#include "pch.h"
#include "VulkanGraphicsContext.h"
#include "VulkanGraphicsBuffer.h"
#include "VulkanPipelineStateBuilder.h"

// ---------------------------------------------------------------------------
// VulkanGraphicsContext
// ---------------------------------------------------------------------------

VulkanGraphicsContext::VulkanGraphicsContext(VkCommandBuffer cmdBuffer)
    : m_cmdBuffer(cmdBuffer)
{
    memset(m_pushData, 0, sizeof(m_pushData));
}

void VulkanGraphicsContext::SetPipeline(const IPipelineState* pipeline)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (!pipeline || m_cmdBuffer == VK_NULL_HANDLE) return;

    auto* vkPso = static_cast<const VulkanPipelineState*>(pipeline);
    vkCmdBindPipeline(m_cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      vkPso->GetVkPipeline());
    m_currentLayout = vkPso->GetVkPipelineLayout();
#else
    (void)pipeline;
#endif
}

void VulkanGraphicsContext::SetConstantBuffer(uint32_t slot,
                                              const IGraphicsBuffer* buffer,
                                              uint64_t offset)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (!buffer) return;

    // Each slot occupies 64 bytes; two slots fill the 128-byte push range
    constexpr uint32_t BYTES_PER_SLOT = 64;
    const uint32_t pushOffset = slot * BYTES_PER_SLOT;
    if (pushOffset >= sizeof(m_pushData)) return;

    const uint32_t available = static_cast<uint32_t>(sizeof(m_pushData)) - pushOffset;

    // Only Upload/Readback (host-visible) buffers can be read back to CPU
    if (buffer->GetAccessMode() == IGraphicsBuffer::AccessMode::Default)
        return;

    void* data = const_cast<IGraphicsBuffer*>(buffer)->Map();
    if (!data) return;

    const uint64_t copySize = std::min(
        static_cast<uint64_t>(available),
        buffer->GetSize() - offset);

    memcpy(m_pushData + pushOffset,
           static_cast<const uint8_t*>(data) + offset,
           static_cast<size_t>(copySize));

    const_cast<IGraphicsBuffer*>(buffer)->Unmap();
    m_pushDirty = true;
#else
    (void)slot; (void)buffer; (void)offset;
#endif
}

void VulkanGraphicsContext::SetVertexBuffer(uint32_t slot,
                                            const IGraphicsBuffer* buffer,
                                            uint32_t stride,
                                            uint64_t offset)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (!buffer || m_cmdBuffer == VK_NULL_HANDLE) return;

    auto* vkBuf = static_cast<const VulkanGraphicsBuffer*>(buffer);
    VkBuffer    vkBuffer  = vkBuf->GetVkBuffer();
    VkDeviceSize vkOffset = static_cast<VkDeviceSize>(offset);

    vkCmdBindVertexBuffers(m_cmdBuffer, slot, 1, &vkBuffer, &vkOffset);
    (void)stride; // stride is baked into the pipeline vertex input description
#else
    (void)slot; (void)buffer; (void)stride; (void)offset;
#endif
}

void VulkanGraphicsContext::SetIndexBuffer(const IGraphicsBuffer* buffer,
                                           uint32_t indexCount,
                                           uint64_t offset)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (!buffer || m_cmdBuffer == VK_NULL_HANDLE) return;

    auto* vkBuf = static_cast<const VulkanGraphicsBuffer*>(buffer);
    vkCmdBindIndexBuffer(m_cmdBuffer, vkBuf->GetVkBuffer(),
                         static_cast<VkDeviceSize>(offset),
                         VK_INDEX_TYPE_UINT32);
    m_indexCount = indexCount;
#else
    (void)buffer; (void)indexCount; (void)offset;
#endif
}

void VulkanGraphicsContext::SetViewport(const Viewport& vp)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_cmdBuffer == VK_NULL_HANDLE) return;

    VkViewport viewport{};
    viewport.x        = vp.x;
    viewport.y        = vp.y;
    viewport.width    = vp.width;
    viewport.height   = vp.height;
    viewport.minDepth = vp.minDepth;
    viewport.maxDepth = vp.maxDepth;

    vkCmdSetViewport(m_cmdBuffer, 0, 1, &viewport);
#else
    (void)vp;
#endif
}

void VulkanGraphicsContext::SetScissorRect(const ScissorRect& rect)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_cmdBuffer == VK_NULL_HANDLE) return;

    VkRect2D scissor{};
    scissor.offset.x      = rect.left;
    scissor.offset.y      = rect.top;
    scissor.extent.width  = static_cast<uint32_t>(rect.right  - rect.left);
    scissor.extent.height = static_cast<uint32_t>(rect.bottom - rect.top);

    vkCmdSetScissor(m_cmdBuffer, 0, 1, &scissor);
#else
    (void)rect;
#endif
}

void VulkanGraphicsContext::Clear(float /*r*/, float /*g*/, float /*b*/,
                                  float /*a*/, float /*depth*/)
{
    // Clearing is handled by the render pass LOAD_OP_CLEAR in VulkanView.
    // vkCmdClearAttachments is valid inside a render pass; omitted here as
    // the render-pass clear is sufficient for the current engine needs.
}

void VulkanGraphicsContext::FlushState()
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_pushDirty && m_currentLayout != VK_NULL_HANDLE)
    {
        vkCmdPushConstants(m_cmdBuffer,
                           m_currentLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0,
                           static_cast<uint32_t>(sizeof(m_pushData)),
                           m_pushData);
        m_pushDirty = false;
    }
#endif
}

void VulkanGraphicsContext::DrawInstanced(uint32_t vertexCountPerInstance,
                                          uint32_t instanceCount,
                                          uint32_t startVertexLocation,
                                          uint32_t startInstanceLocation)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_cmdBuffer == VK_NULL_HANDLE) return;
    FlushState();
    vkCmdDraw(m_cmdBuffer,
              vertexCountPerInstance,
              instanceCount,
              startVertexLocation,
              startInstanceLocation);
#else
    (void)vertexCountPerInstance; (void)instanceCount;
    (void)startVertexLocation;    (void)startInstanceLocation;
#endif
}

void VulkanGraphicsContext::DrawIndexedInstanced(uint32_t indexCountPerInstance,
                                                  uint32_t instanceCount,
                                                  uint32_t startIndexLocation,
                                                  int32_t  baseVertexLocation,
                                                  uint32_t startInstanceLocation)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_cmdBuffer == VK_NULL_HANDLE) return;
    FlushState();
    vkCmdDrawIndexed(m_cmdBuffer,
                     indexCountPerInstance,
                     instanceCount,
                     startIndexLocation,
                     baseVertexLocation,
                     startInstanceLocation);
#else
    (void)indexCountPerInstance; (void)instanceCount;
    (void)startIndexLocation;    (void)baseVertexLocation;
    (void)startInstanceLocation;
#endif
}

void VulkanGraphicsContext::TransitionResource(void* /*resource*/,
                                               ResourceState /*stateBefore*/,
                                               ResourceState /*stateAfter*/)
{
    // VulkanView manages image-layout transitions via its own render pass
    // and explicit pipeline barriers.  Direct context-level transitions are
    // not required for the current rendering model.
}

// ---------------------------------------------------------------------------
// VulkanGraphicsContextFactory
// ---------------------------------------------------------------------------

VulkanGraphicsContextFactory::VulkanGraphicsContextFactory(VkDevice      device,
                                                           VkCommandPool commandPool)
    : m_device(device)
    , m_commandPool(commandPool)
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_device == VK_NULL_HANDLE || m_commandPool == VK_NULL_HANDLE)
        return;

    // Allocate a fallback command buffer owned by the factory.
    // Renderers may override this with SetCurrentCommandBuffer().
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = m_commandPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(m_device, &allocInfo, &m_cmdBuffer) == VK_SUCCESS)
        m_ownBuffer = true;
#endif
}

VulkanGraphicsContextFactory::~VulkanGraphicsContextFactory()
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_ownBuffer && m_cmdBuffer != VK_NULL_HANDLE &&
        m_device != VK_NULL_HANDLE && m_commandPool != VK_NULL_HANDLE)
    {
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_cmdBuffer);
    }
#endif
}

void VulkanGraphicsContextFactory::SetCurrentCommandBuffer(VkCommandBuffer cmdBuffer)
{
    m_cmdBuffer = cmdBuffer;
}

std::unique_ptr<IGraphicsContext> VulkanGraphicsContextFactory::CreateContext()
{
    return std::make_unique<VulkanGraphicsContext>(m_cmdBuffer);
}
