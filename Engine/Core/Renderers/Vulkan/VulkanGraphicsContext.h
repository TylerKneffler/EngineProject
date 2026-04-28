#pragma once
#include "Core/Graphics/IGraphicsContext.h"

#if defined(ENGINE_VULKAN_ENABLED)
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR 1
    #endif
    #include <vulkan/vulkan.h>
#else
    typedef void* VkCommandBuffer;
    typedef void* VkDevice;
    typedef void* VkCommandPool;
    typedef void* VkPipelineLayout;
    #define VK_NULL_HANDLE nullptr
#endif

#include <memory>
#include <cstdint>

// ---------------------------------------------------------------------------
// VulkanGraphicsContext — VkCommandBuffer wrapper implementing IGraphicsContext
//
// Constant buffer slots are delivered via Vulkan push constants (128 bytes):
//   slot 0 → bytes   0–63  (e.g. transform / MVP matrix)
//   slot 1 → bytes  64–127 (e.g. material data)
//
// The pipeline layout for each bound VulkanPipelineState must declare a
// push constant range covering all 128 bytes in VK_SHADER_STAGE_ALL_GRAPHICS.
// ---------------------------------------------------------------------------
class VulkanGraphicsContext : public IGraphicsContext
{
public:
    explicit VulkanGraphicsContext(VkCommandBuffer cmdBuffer);
    ~VulkanGraphicsContext() override = default;

    void SetPipeline(const IPipelineState* pipeline) override;
    void SetConstantBuffer(uint32_t slot, const IGraphicsBuffer* buffer,
                           uint64_t offset = 0) override;
    void SetVertexBuffer(uint32_t slot, const IGraphicsBuffer* buffer,
                         uint32_t stride, uint64_t offset = 0) override;
    void SetIndexBuffer(const IGraphicsBuffer* buffer, uint32_t indexCount,
                        uint64_t offset = 0) override;

    void SetViewport(const Viewport& vp)       override;
    void SetScissorRect(const ScissorRect& rect) override;

    void Clear(float r, float g, float b, float a, float depth = 1.0f) override;

    void DrawInstanced(uint32_t vertexCountPerInstance,
                       uint32_t instanceCount,
                       uint32_t startVertexLocation = 0,
                       uint32_t startInstanceLocation = 0) override;

    void DrawIndexedInstanced(uint32_t indexCountPerInstance,
                               uint32_t instanceCount,
                               uint32_t startIndexLocation    = 0,
                               int32_t  baseVertexLocation    = 0,
                               uint32_t startInstanceLocation = 0) override;

    // Image-layout transitions are handled by VulkanView's render pass;
    // this is a no-op stub kept for API compatibility.
    void TransitionResource(void* resource,
                            ResourceState stateBefore,
                            ResourceState stateAfter) override;

    void* GetNativeHandle() const override { return static_cast<void*>(m_cmdBuffer); }

private:
    void FlushState();

    VkCommandBuffer  m_cmdBuffer        = VK_NULL_HANDLE;
    VkPipelineLayout m_currentLayout    = VK_NULL_HANDLE;

    // Push-constant shadow: 128 bytes split across slots (64 bytes each)
    uint8_t          m_pushData[128]    = {};
    bool             m_pushDirty        = false;

    uint32_t         m_indexCount       = 0;
};

// ---------------------------------------------------------------------------
// VulkanGraphicsContextFactory — allocates and resets VkCommandBuffers
//
// Call SetCurrentCommandBuffer() from the renderer's BeginFrame / RenderIfNeeded
// before calling CreateContext() so the returned context uses the correct
// per-frame command buffer.
// ---------------------------------------------------------------------------
class VulkanGraphicsContextFactory : public IGraphicsContextFactory
{
public:
    VulkanGraphicsContextFactory(VkDevice device, VkCommandPool commandPool);
    ~VulkanGraphicsContextFactory() override;

    // Point the factory at the command buffer that BeginFrame already began.
    void SetCurrentCommandBuffer(VkCommandBuffer cmdBuffer);

    std::unique_ptr<IGraphicsContext> CreateContext() override;

private:
    VkDevice        m_device      = VK_NULL_HANDLE;
    VkCommandPool   m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmdBuffer   = VK_NULL_HANDLE;
    bool            m_ownBuffer   = false; // true if we allocated m_cmdBuffer
};
