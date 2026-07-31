#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGraphicsContext.h"
#include "VulkanGraphicsBuffer.h"
#include "VulkanPipelineStateBuilder.h"
#include <algorithm>

void VulkanGraphicsContext::SetPipeline(const IPipelineState* pipeline)
{
    m_pipeline = dynamic_cast<const VulkanPipelineState*>(pipeline);
    if (m_pipeline) vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->GetPipeline());
}

void VulkanGraphicsContext::SetConstantBuffer(uint32_t, const IGraphicsBuffer* buffer, uint64_t offset)
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

void VulkanGraphicsContext::SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t, uint64_t offset)
{
    const auto* vkBuffer = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
    if (!vkBuffer) return;
    VkBuffer native = vkBuffer->GetBuffer(); VkDeviceSize nativeOffset = offset;
    vkCmdBindVertexBuffers(m_commandBuffer, slot, 1, &native, &nativeOffset);
}

void VulkanGraphicsContext::SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t, uint64_t offset)
{
    const auto* vkBuffer = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
    if (vkBuffer) vkCmdBindIndexBuffer(m_commandBuffer, vkBuffer->GetBuffer(), offset, VK_INDEX_TYPE_UINT32);
}

void VulkanGraphicsContext::SetTexture(uint32_t slot, const IGraphicsTexture* texture)
{
    if (slot < m_textures.size())
        m_textures[slot] = dynamic_cast<const VulkanGraphicsTexture*>(texture);
}

void VulkanGraphicsContext::SetStructuredBuffer(uint32_t slot, const IGraphicsBuffer* buffer)
{
    if (slot < 5 || slot > 6) return;
    m_structuredBuffers[slot - 5] = dynamic_cast<const VulkanGraphicsBuffer*>(buffer);
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
#endif
