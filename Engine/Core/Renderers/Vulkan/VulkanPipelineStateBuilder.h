#pragma once
#include "Core/Graphics/IPipelineState.h"

#if defined(ENGINE_VULKAN_ENABLED)
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR 1
    #endif
    #include <vulkan/vulkan.h>
#else
    typedef void* VkDevice;
    typedef void* VkPipeline;
    typedef void* VkPipelineLayout;
    typedef void* VkRenderPass;
    #define VK_NULL_HANDLE nullptr
#endif

#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// VulkanPipelineState — Owning wrapper for VkPipeline + VkPipelineLayout +
//                       VkRenderPass created by VulkanPipelineStateBuilder
// ---------------------------------------------------------------------------
class VulkanPipelineState : public IPipelineState
{
public:
    VulkanPipelineState(VkDevice         device,
                        VkPipeline       pipeline,
                        VkPipelineLayout layout,
                        VkRenderPass     renderPass);
    ~VulkanPipelineState() override;

    void*            GetNativeHandle()    const override { return static_cast<void*>(m_pipeline);  }
    VkPipeline       GetVkPipeline()      const          { return m_pipeline;  }
    VkPipelineLayout GetVkPipelineLayout() const         { return m_layout;    }
    VkRenderPass     GetVkRenderPass()    const          { return m_renderPass; }

private:
    VkDevice         m_device     = VK_NULL_HANDLE;
    VkPipeline       m_pipeline   = VK_NULL_HANDLE;
    VkPipelineLayout m_layout     = VK_NULL_HANDLE;
    VkRenderPass     m_renderPass = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// VulkanPipelineStateBuilder — Fluent Vulkan pipeline builder
//
// DXGI_FORMAT and D3D12_BLEND enum integers are accepted for compatibility
// with the API-neutral interface; they are mapped to Vulkan equivalents in
// Build().
//
// Push constants (128 bytes, all graphics stages) are declared automatically
// in the generated VkPipelineLayout, matching VulkanGraphicsContext's usage.
// ---------------------------------------------------------------------------
class VulkanPipelineStateBuilder : public IPipelineStateBuilder
{
public:
    explicit VulkanPipelineStateBuilder(VkDevice device);
    ~VulkanPipelineStateBuilder() override = default;

    IPipelineStateBuilder& SetVertexShader(const IShader* shader)  override;
    IPipelineStateBuilder& SetPixelShader(const IShader* shader)   override;

    IPipelineStateBuilder& SetFillMode(bool wireframe)              override;
    IPipelineStateBuilder& SetCullMode(bool cullBackFaces)          override;
    IPipelineStateBuilder& SetFrontCounterClockwise(bool ccw)       override;
    IPipelineStateBuilder& SetDepthClipEnable(bool enable)          override;

    IPipelineStateBuilder& SetBlendEnable(bool enable)              override;
    IPipelineStateBuilder& SetSrcBlend(int mode)                    override;
    IPipelineStateBuilder& SetDestBlend(int mode)                   override;
    IPipelineStateBuilder& SetBlendOp(int op)                       override;
    IPipelineStateBuilder& SetSrcBlendAlpha(int mode)               override;
    IPipelineStateBuilder& SetDestBlendAlpha(int mode)              override;
    IPipelineStateBuilder& SetBlendOpAlpha(int op)                  override;

    IPipelineStateBuilder& SetDepthEnable(bool enable)              override;
    IPipelineStateBuilder& SetDepthWriteEnable(bool enable)         override;
    IPipelineStateBuilder& SetDepthFunc(int func)                   override;

    IPipelineStateBuilder& SetInputLayout(
        const VertexElement* elements, uint32_t elementCount)       override;

    IPipelineStateBuilder& SetPrimitiveTopology(PrimitiveTopology topology) override;
    IPipelineStateBuilder& SetRenderTargetFormat(int format, int depthFormat = -1) override;

    std::unique_ptr<IPipelineState> Build()          override;
    std::string                     GetLastError()   const override { return m_lastError; }

private:
#if defined(ENGINE_VULKAN_ENABLED)
    // Conversion helpers
    VkFormat        DxgiToVkFormat(int dxgiFormat) const;
    VkBlendFactor   D3DBlendToVk(int d3dBlend)     const;
    VkBlendOp       D3DBlendOpToVk(int d3dOp)      const;
    VkCompareOp     D3DCompareToVk(int d3dFunc)     const;
    uint32_t        SemanticToLocation(const char* semantic) const;
#endif

    VkDevice    m_device = VK_NULL_HANDLE;
    std::string m_lastError;

    // Shader bytecode (SPIR-V)
    std::vector<uint8_t> m_vsBytecode;
    std::vector<uint8_t> m_psBytecode;
    std::string          m_vsEntryPoint = "main";
    std::string          m_psEntryPoint = "main";

    // Input layout
    struct InputElement
    {
        std::string semantic;
        uint32_t    semanticIndex;
        int         format;       // DXGI_FORMAT value
        uint32_t    inputSlot;
        uint32_t    byteOffset;
        bool        perInstance;
    };
    std::vector<InputElement> m_inputElements;

    // Rasterizer
    bool m_wireframe  = false;
    bool m_cullBack   = true;
    bool m_frontCCW   = true;
    bool m_depthClip  = true;

    // Blend
    bool m_blendEnable    = false;
    int  m_srcBlend       = 2; // D3D12_BLEND_ONE
    int  m_destBlend      = 1; // D3D12_BLEND_ZERO
    int  m_blendOp        = 1; // D3D12_BLEND_OP_ADD
    int  m_srcBlendAlpha  = 2;
    int  m_destBlendAlpha = 1;
    int  m_blendOpAlpha   = 1;

    // Depth/stencil
    bool m_depthEnable      = true;
    bool m_depthWriteEnable = true;
    int  m_depthFunc        = 2; // D3D12_COMPARISON_FUNC_LESS

    // Topology
    PrimitiveTopology m_topology = PrimitiveTopology::TriangleList;

    // Render target format (DXGI_FORMAT values, -1 = no depth)
    int m_rtFormat    = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
    int m_depthFormat = 40; // DXGI_FORMAT_D32_FLOAT
};

// ---------------------------------------------------------------------------
// VulkanPipelineStateFactory — Creates VulkanPipelineStateBuilder instances
// ---------------------------------------------------------------------------
class VulkanPipelineStateFactory : public IPipelineStateFactory
{
public:
    explicit VulkanPipelineStateFactory(VkDevice device) : m_device(device) {}

    std::unique_ptr<IPipelineStateBuilder> CreateBuilder() override
    {
        return std::make_unique<VulkanPipelineStateBuilder>(m_device);
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
};
