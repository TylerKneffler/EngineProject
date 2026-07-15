#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Graphics/IGraphicsContext.h"
#include "VulkanCommon.h"

class VulkanPipelineState;
class VulkanGraphicsContext : public IGraphicsContext
{
public:
    explicit VulkanGraphicsContext(VkCommandBuffer commandBuffer) : m_commandBuffer(commandBuffer) {}
    void SetPipeline(const IPipelineState* pipeline) override;
    void SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint64_t offset = 0) override;
    void SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset = 0) override;
    void SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset = 0) override;
    void SetViewport(const Viewport& viewport) override;
    void SetScissorRect(const ScissorRect& rect) override;
    void Clear(float, float, float, float, float = 1.0f) override {}
    void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex = 0, uint32_t startInstance = 0) override;
    void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex = 0,
                              int32_t baseVertex = 0, uint32_t startInstance = 0) override;
    void TransitionResource(void* resource, ResourceState before, ResourceState after) override;
    void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_commandBuffer); }
private:
    VkCommandBuffer m_commandBuffer;
    const VulkanPipelineState* m_pipeline = nullptr;
};

class VulkanGraphicsContextFactory : public IGraphicsContextFactory
{
public:
    void SetCommandBuffer(void* commandBuffer) override
    { m_commandBuffer = reinterpret_cast<VkCommandBuffer>(commandBuffer); }
    std::unique_ptr<IGraphicsContext> CreateContext() override
    { return m_commandBuffer ? std::make_unique<VulkanGraphicsContext>(m_commandBuffer) : nullptr; }
private:
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
};
#endif
