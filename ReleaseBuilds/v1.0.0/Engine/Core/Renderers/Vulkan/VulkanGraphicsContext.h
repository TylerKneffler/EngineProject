#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Graphics/IGraphicsContext.h"
#include "VulkanCommon.h"
#include "VulkanGraphicsTexture.h"
#include <memory>


namespace Engine::Renderers
{
class VulkanPipelineState;
class VulkanGraphicsBuffer;
struct VulkanOcclusionQueryState;
struct VulkanGpuTimingState;
class VulkanGraphicsContext : public Engine::Graphics::IGraphicsContext
{
public:
    VulkanGraphicsContext(
        VkCommandBuffer commandBuffer,
        std::shared_ptr<VulkanTextureSystem> textureSystem,
        std::shared_ptr<VulkanOcclusionQueryState> occlusionState,
        std::shared_ptr<VulkanGpuTimingState> gpuTimings)
        : m_commandBuffer(commandBuffer), m_textureSystem(std::move(textureSystem)),
          m_occlusionState(std::move(occlusionState)), m_gpuTimings(std::move(gpuTimings)) {}
    void SetPipeline(const Engine::Graphics::IPipelineState* pipeline) override;
    void SetStencilReference(uint32_t reference) override;
    void SetConstantBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint64_t offset = 0) override;
    void SetStructuredBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer) override;
    void SetVertexBuffer(uint32_t slot, const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t stride, uint64_t offset = 0) override;
    void SetIndexBuffer(const Engine::Graphics::IGraphicsBuffer* buffer, uint32_t indexCount, uint64_t offset = 0) override;
    void SetTexture(uint32_t slot, const Engine::Graphics::IGraphicsTexture* texture) override;
    void SetViewport(const Viewport& viewport) override;
    void SetScissorRect(const ScissorRect& rect) override;
    void Clear(float, float, float, float, float = 1.0f) override {}
    void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex = 0, uint32_t startInstance = 0) override;
    void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex = 0,
                              int32_t baseVertex = 0, uint32_t startInstance = 0) override;
    bool BeginOcclusionFrame(uint64_t viewId, uint64_t sceneSignature) override;
    bool IsOccluded(uint64_t objectId) override;
    void BeginOcclusionQuery(uint64_t objectId) override;
    void EndOcclusionQuery() override;
    void BeginGpuTiming(Engine::Graphics::GpuTimingStage stage) override;
    void EndGpuTiming(Engine::Graphics::GpuTimingStage stage) override;
    void TransitionResource(void* resource, ResourceState before, ResourceState after) override;
    void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_commandBuffer); }
private:
    VkCommandBuffer m_commandBuffer;
    const VulkanPipelineState* m_pipeline = nullptr;
    std::shared_ptr<VulkanTextureSystem> m_textureSystem;
    std::array<const VulkanGraphicsTexture*, 7> m_textures{};
    std::array<const VulkanGraphicsBuffer*, 3> m_structuredBuffers{};
    std::shared_ptr<VulkanOcclusionQueryState> m_occlusionState;
    std::shared_ptr<VulkanGpuTimingState> m_gpuTimings;
    uint64_t m_occlusionViewId = 0;
    uint64_t m_occlusionSceneSignature = 0;
    uint64_t m_activeOcclusionKey = 0;
    uint32_t m_activeOcclusionIndex = UINT32_MAX;
};

class VulkanGraphicsContextFactory : public Engine::Graphics::IGraphicsContextFactory
{
public:
    void SetCommandBuffer(void* commandBuffer) override
    { m_commandBuffer = reinterpret_cast<VkCommandBuffer>(commandBuffer); }
    void PrepareFrame(uint32_t frameSlot) override;
    void FinalizeFrame() override;
    Engine::Graphics::FrameTimingTelemetry GetFrameTimingTelemetry() const override;
    std::unique_ptr<Engine::Graphics::IGraphicsContext> CreateContext() override
    {
        return m_commandBuffer
            ? std::make_unique<VulkanGraphicsContext>(
                m_commandBuffer, m_textureSystem, m_occlusionState, m_gpuTimings)
            : nullptr;
    }
    void SetDevice(VkDevice device, float timestampPeriod);
    void SetTextureSystem(std::shared_ptr<VulkanTextureSystem> textureSystem)
    { m_textureSystem = std::move(textureSystem); }
private:
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    std::shared_ptr<VulkanTextureSystem> m_textureSystem;
    std::shared_ptr<VulkanOcclusionQueryState> m_occlusionState;
    std::shared_ptr<VulkanGpuTimingState> m_gpuTimings;
};
}
#endif
