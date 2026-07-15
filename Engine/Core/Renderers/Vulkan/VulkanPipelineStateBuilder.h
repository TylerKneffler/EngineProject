#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Graphics/IPipelineState.h"
#include "VulkanCommon.h"
#include <vector>

class VulkanPipelineState : public IPipelineState
{
public:
    VulkanPipelineState(VkDevice device, VkPipeline pipeline, VkPipelineLayout layout)
        : m_device(device), m_pipeline(pipeline), m_layout(layout) {}
    ~VulkanPipelineState() override;
    void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_pipeline); }
    VkPipeline GetPipeline() const { return m_pipeline; }
    VkPipelineLayout GetLayout() const { return m_layout; }
private:
    VkDevice m_device;
    VkPipeline m_pipeline;
    VkPipelineLayout m_layout;
};

class VulkanPipelineStateBuilder : public IPipelineStateBuilder
{
public:
    VulkanPipelineStateBuilder(VkDevice device, VkRenderPass renderPass)
        : m_device(device), m_renderPass(renderPass) {}
    IPipelineStateBuilder& SetVertexShader(const IShader* shader) override;
    IPipelineStateBuilder& SetPixelShader(const IShader* shader) override;
    IPipelineStateBuilder& SetFillMode(bool wireframe) override;
    IPipelineStateBuilder& SetCullMode(bool cullBackFaces) override;
    IPipelineStateBuilder& SetFrontCounterClockwise(bool ccw) override;
    IPipelineStateBuilder& SetDepthClipEnable(bool enable) override;
    IPipelineStateBuilder& SetBlendEnable(bool enable) override;
    IPipelineStateBuilder& SetSrcBlend(int mode) override;
    IPipelineStateBuilder& SetDestBlend(int mode) override;
    IPipelineStateBuilder& SetBlendOp(int op) override;
    IPipelineStateBuilder& SetSrcBlendAlpha(int mode) override;
    IPipelineStateBuilder& SetDestBlendAlpha(int mode) override;
    IPipelineStateBuilder& SetBlendOpAlpha(int op) override;
    IPipelineStateBuilder& SetDepthEnable(bool enable) override;
    IPipelineStateBuilder& SetDepthWriteEnable(bool enable) override;
    IPipelineStateBuilder& SetDepthFunc(int func) override;
    IPipelineStateBuilder& SetInputLayout(const VertexElement* elements, uint32_t count) override;
    IPipelineStateBuilder& SetPrimitiveTopology(PrimitiveTopology topology) override;
    IPipelineStateBuilder& SetRenderTargetFormat(int format, int depthFormat = -1) override;
    std::unique_ptr<IPipelineState> Build() override;
    std::string GetLastError() const override { return m_lastError; }
private:
    static VkBlendFactor Blend(int value);
    static VkBlendOp BlendOp(int value);
    static VkCompareOp Compare(int value);
    VkDevice m_device;
    VkRenderPass m_renderPass;
    std::vector<uint8_t> m_vs;
    std::vector<uint8_t> m_ps;
    std::vector<VkVertexInputBindingDescription> m_bindings;
    std::vector<VkVertexInputAttributeDescription> m_attributes;
    VkPolygonMode m_polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags m_cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace m_frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool m_blend = false;
    VkBlendFactor m_srcBlend = VK_BLEND_FACTOR_ONE;
    VkBlendFactor m_dstBlend = VK_BLEND_FACTOR_ZERO;
    VkBlendOp m_blendOp = VK_BLEND_OP_ADD;
    VkBlendFactor m_srcBlendAlpha = VK_BLEND_FACTOR_ONE;
    VkBlendFactor m_dstBlendAlpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp m_blendOpAlpha = VK_BLEND_OP_ADD;
    bool m_depth = true;
    bool m_depthWrite = true;
    VkCompareOp m_depthCompare = VK_COMPARE_OP_LESS;
    VkPrimitiveTopology m_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    std::string m_lastError;
};

class VulkanPipelineStateFactory : public IPipelineStateFactory
{
public:
    VulkanPipelineStateFactory(VkDevice device, VkRenderPass renderPass)
        : m_device(device), m_renderPass(renderPass) {}
    std::unique_ptr<IPipelineStateBuilder> CreateBuilder() override
    { return std::make_unique<VulkanPipelineStateBuilder>(m_device, m_renderPass); }
private:
    VkDevice m_device;
    VkRenderPass m_renderPass;
};
#endif
