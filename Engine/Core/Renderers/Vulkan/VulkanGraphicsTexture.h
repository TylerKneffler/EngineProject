#pragma once
#if defined(ENGINE_VULKAN_ENABLED)

#include "Core/Graphics/IGraphicsTexture.h"
#include "VulkanCommon.h"

#include <array>
#include <memory>
#include <unordered_map>

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

    std::shared_ptr<VulkanGraphicsTexture> CreateTexture(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels,
        uint32_t mipLevels, GraphicsTextureFormat format,
        bool srgb = true);
    void Bind(
        VkCommandBuffer commands,
        VkPipelineLayout pipelineLayout,
        const std::array<const VulkanGraphicsTexture*, 6>& textures,
        const std::array<const VulkanGraphicsBuffer*, 3>& buffers);
    VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_layout; }
    VkDevice GetDevice() const { return m_device; }

private:
    struct TextureKey
    {
        std::array<VkImageView, 6> views{};
        std::array<VkBuffer, 3> buffers{};
        bool operator==(const TextureKey& other) const
        {
            return views == other.views && buffers == other.buffers;
        }
    };
    struct TextureKeyHash
    {
        size_t operator()(const TextureKey& key) const;
    };

    VulkanImageResource Upload(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels,
        uint32_t mipLevels, GraphicsTextureFormat format,
        bool srgb = true);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkBuffer m_dummyBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_dummyBufferMemory = VK_NULL_HANDLE;
    VulkanImageResource m_white;
    std::unordered_map<TextureKey, VkDescriptorSet, TextureKeyHash> m_sets;
};

class VulkanGraphicsTexture final : public IGraphicsTexture
{
public:
    VulkanGraphicsTexture(
        std::shared_ptr<VulkanTextureSystem> system,
        VulkanImageResource image)
        : m_system(std::move(system)), m_image(image) {}
    ~VulkanGraphicsTexture() override;
    void* GetNativeHandle() const override
    {
        return reinterpret_cast<void*>(m_image.image);
    }
    VkImageView GetView() const { return m_image.view; }

private:
    std::shared_ptr<VulkanTextureSystem> m_system;
    VulkanImageResource m_image;
};

class VulkanTextureFactory final : public IGraphicsTextureFactory
{
public:
    explicit VulkanTextureFactory(std::shared_ptr<VulkanTextureSystem> system)
        : m_system(std::move(system)) {}
    std::shared_ptr<IGraphicsTexture> CreateTexture2D(
        uint32_t width, uint32_t height, const uint8_t* rgbaPixels,
        uint32_t mipLevels, GraphicsTextureFormat format,
        bool srgb = true) override;

private:
    std::shared_ptr<VulkanTextureSystem> m_system;
};
#endif
