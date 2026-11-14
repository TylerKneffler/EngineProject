#pragma once
#if defined(ENGINE_VULKAN_ENABLED)

#include "Core/Graphics/IGraphicsTexture.h"
#include "VulkanCommon.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <atomic>


namespace Engine::Renderers
{
class VulkanGraphicsTexture;
class VulkanGraphicsBuffer;

class VulkanTextureSystem : public std::enable_shared_from_this<VulkanTextureSystem>
{
public:
    VulkanTextureSystem(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkQueue queue,
        uint32_t queueFamily);
    ~VulkanTextureSystem();
    void Shutdown();
    void RegisterTexture(VulkanGraphicsTexture* texture);
    void UnregisterTexture(VulkanGraphicsTexture* texture);

    std::shared_ptr<VulkanGraphicsTexture> CreateTexture(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels,
        uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format,
        bool srgb = true, uint32_t layers = 1, bool arrayView = false);
    std::shared_ptr<VulkanGraphicsTexture> CreateDepthTexture(
        uint32_t width, uint32_t height);
    void Bind(
        VkCommandBuffer commands,
        VkPipelineLayout pipelineLayout,
        const std::array<const VulkanGraphicsTexture*, 30>& textures,
        const std::array<const VulkanGraphicsBuffer*, 9>& buffers);
    VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_layout; }
    VkDevice GetDevice() const { return m_device; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
    void PrepareFrame(uint32_t frameSlot);
    void PollUploads();
    uint64_t PendingUploadBytes() const;
    bool UploadComplete(VkImage image) const;
    void ReleaseUploadedImage(VulkanImageResource& image);
    uint64_t EstimateAllocationBytes(uint32_t, uint32_t, uint32_t,
        Engine::Graphics::GraphicsTextureFormat, bool) const;
    bool SupportsBlockCompression() const;

private:
    struct TextureKey
    {
        std::array<VkImageView, 30> views{};
        std::array<uint64_t,30> identities{};
        std::array<VkBuffer, 9> buffers{};
        std::array<uint64_t, 9> bufferIdentities{};
        bool operator==(const TextureKey& other) const
        {
            return views == other.views && identities == other.identities && buffers == other.buffers && bufferIdentities == other.bufferIdentities;
        }
    };
    struct TextureKeyHash
    {
        size_t operator()(const TextureKey& key) const;
    };

    VulkanImageResource Upload(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels,
        uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format,
        bool srgb = true, uint32_t layers = 1, bool arrayView = false);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkBuffer m_dummyBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_dummyBufferMemory = VK_NULL_HANDLE;
    VulkanImageResource m_white;
    // Default for the terrain layer array bindings (logical slots 12-14),
    // whose shader declarations are 2D arrays.
    VulkanImageResource m_whiteArray;
    struct DescriptorFrame
    {
        std::vector<VkDescriptorPool> pools;
        size_t activePool=0;
        std::unordered_map<TextureKey,VkDescriptorSet,TextureKeyHash> sets;
    };
    std::vector<DescriptorFrame> m_descriptorFrames;
    size_t m_descriptorFrame=0;
    VkDescriptorPool CreateDescriptorPool();
    std::unordered_set<VulkanGraphicsTexture*> m_liveTextures;
    struct PendingUpload
    {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VulkanImageResource retiredImage{};
        uint64_t bytes = 0;
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> owner;
    };
    std::vector<PendingUpload> m_uploads;
};

class VulkanGraphicsTexture final : public Engine::Graphics::IGraphicsTexture
{
public:
    VulkanGraphicsTexture(
        std::shared_ptr<VulkanTextureSystem> system,
        VulkanImageResource image);
    ~VulkanGraphicsTexture() override;
    void* GetNativeHandle() const override
    {
        return reinterpret_cast<void*>(m_image.image);
    }
    VkImageView GetView() const { return m_image.view; }
    uint64_t GetIdentity() const { return m_identity; }
    // Dedicated image allocation size (excludes staging and descriptors).
    bool IsUploadComplete() const override { return !m_system || m_system->UploadComplete(m_image.image); }
    uint64_t GetAllocationSizeBytes() const override
    {
        if (!m_system || !m_system->GetDevice() || !m_image.image) return 0;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_system->GetDevice(), m_image.image, &requirements);
        return requirements.size;
    }
    bool IsDepthTarget() const { return m_framebuffer != VK_NULL_HANDLE; }
    VkRenderPass GetRenderPass() const { return m_renderPass; }
    VkRenderPass GetLoadRenderPass() const { return m_loadRenderPass; }
    VkFramebuffer GetFramebuffer() const { return m_framebuffer; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    void SetDepthTargetResources(VulkanImageResource color,
        VkRenderPass renderPass, VkRenderPass loadRenderPass, VkFramebuffer framebuffer,
        uint32_t width, uint32_t height);

private:
    friend class VulkanTextureSystem;
    inline static std::atomic<uint64_t> s_nextIdentity{1};
    uint64_t m_identity=s_nextIdentity.fetch_add(1,std::memory_order_relaxed);
    void ReleaseImage(VkDevice device);
    std::shared_ptr<VulkanTextureSystem> m_system;
    VulkanImageResource m_image;
    VulkanImageResource m_depthColor;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkRenderPass m_loadRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

class VulkanTextureFactory final : public Engine::Graphics::IGraphicsTextureFactory
{
public:
    explicit VulkanTextureFactory(std::shared_ptr<VulkanTextureSystem> system)
        : m_system(std::move(system)) {}
    void PollUploads() override { m_system->PollUploads(); }
    uint64_t GetPendingUploadBytes() const override { return m_system->PendingUploadBytes(); }
    uint64_t EstimateAllocationBytes(uint32_t w, uint32_t h, uint32_t levels,
        Engine::Graphics::GraphicsTextureFormat format, bool srgb = true) const override
    { return m_system->EstimateAllocationBytes(w,h,levels,format,srgb); }
    bool SupportsBlockCompression() const override
    { return m_system && m_system->SupportsBlockCompression(); }
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateTexture2D(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels,
        uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format,
        bool srgb = true) override;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateTexture2DArray(
        uint32_t width, uint32_t height, uint32_t layers, const uint8_t* pixels,
        uint32_t mipLevels, Engine::Graphics::GraphicsTextureFormat format,
        bool srgb = true) override;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateDepthTexture2D(
        uint32_t width, uint32_t height) override;

private:
    std::shared_ptr<VulkanTextureSystem> m_system;
};
}
#endif
