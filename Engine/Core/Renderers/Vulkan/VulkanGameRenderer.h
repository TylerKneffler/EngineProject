#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "../IGameRenderer.h"
#include "VulkanRenderCore.h"
#include "VulkanGraphicsProvider.h"
#include "VulkanPostProcess.h"
#include <vector>

namespace Engine::Renderers
{
class VulkanGameRenderer : public IGameRenderer
{
public:
    explicit VulkanGameRenderer(std::string graphicsDevice = "Auto")
        : m_graphicsDevice(std::move(graphicsDevice)) {}
    ~VulkanGameRenderer() override;
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    const char* GetBackendName() const override { return "vulkan"; }
    std::string GetGraphicsDeviceName() const override { return m_core.GetPhysicalDeviceName(); }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    Engine::Graphics::IGraphicsProvider* GetGraphicsProvider() override { return m_provider.get(); }
    void WaitIdle() override { m_core.WaitIdle(); }
    void RequestSceneCapture() { m_captureRequested = true; }
    bool ReadSceneCaptureRGBA(std::vector<uint8_t>& pixels);
    void BeginFrame() override;
    void EndFrame() override;
    uint64_t GetSubmittedFrameToken() const override
    { return m_core.GetSubmittedFrameToken(); }
    bool IsFrameComplete(uint64_t token) override
    { return m_core.IsFrameComplete(token); }
    std::unique_ptr<Engine::Graphics::IGraphicsContext> CreateFrameGraphicsContext() override;
    bool RecordWorkerCommands(const Engine::Graphics::GraphicsCommandPacket&) override;
    bool SupportsWorkerCommands() const override { return true; }
    void SetWorkerRecordingEnabled(bool enabled) override
    { m_workerRecordingEnabled = enabled; }
    void SubmitWorkerCommands() override;
    Engine::Graphics::FrameTimingTelemetry GetFrameTimingTelemetry() const override
    { return m_frameTelemetry; }
    double GetLastQueueSubmissionMilliseconds() const override
    { return m_core.GetLastQueueSubmissionMilliseconds(); }
    double GetLastPresentationMilliseconds() const override
    { return m_core.GetLastPresentationMilliseconds(); }
private:
    std::string m_graphicsDevice = "Auto";
    void RecordSceneCapture();
    void DestroySceneCapture();
    void BeginMainRenderPass(const float color[4]);
    VulkanRenderCore m_core;
    std::unique_ptr<VulkanGraphicsProvider> m_provider;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VkCommandPool m_workerCommandPools[2]{};
    VkCommandBuffer m_workerCommandBuffers[2]{};
    bool m_workerCommandsReady = false;
    bool m_workerRecordingEnabled = false;
    uint32_t m_width = 0, m_height = 0;
    bool m_renderPassActive = false;
    VulkanPostProcess m_postProcess;
    VkBuffer m_captureBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_captureMemory = VK_NULL_HANDLE;
    bool m_captureRequested = false;
    float m_clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    Engine::Graphics::FrameTimingTelemetry m_frameTelemetry{};
};
}
#endif
