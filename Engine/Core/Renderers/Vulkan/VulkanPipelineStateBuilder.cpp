#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanPipelineStateBuilder.h"
#include <cstring>

VulkanPipelineState::~VulkanPipelineState()
{
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_layout) vkDestroyPipelineLayout(m_device, m_layout, nullptr);
}

static void CopyShader(std::vector<uint8_t>& target, const IShader* shader)
{
    target.clear();
    if (!shader || !shader->GetBytecode()) return;
    const auto* bytes = static_cast<const uint8_t*>(shader->GetBytecode());
    target.assign(bytes, bytes + shader->GetBytecodeSize());
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetVertexShader(const IShader* s) { CopyShader(m_vs, s); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetPixelShader(const IShader* s) { CopyShader(m_ps, s); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetFillMode(bool w) { m_polygonMode = w ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetCullMode(bool b) { m_cullMode = b ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE; return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetFrontCounterClockwise(bool c) { m_frontFace = c ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE; return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDepthClipEnable(bool) { return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetBlendEnable(bool b) { m_blend = b; return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetSrcBlend(int v) { m_srcBlend = Blend(v); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDestBlend(int v) { m_dstBlend = Blend(v); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetBlendOp(int v) { m_blendOp = BlendOp(v); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetSrcBlendAlpha(int v) { m_srcBlendAlpha = Blend(v); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDestBlendAlpha(int v) { m_dstBlendAlpha = Blend(v); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetBlendOpAlpha(int v) { m_blendOpAlpha = BlendOp(v); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDepthEnable(bool b) { m_depth = b; return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDepthWriteEnable(bool b) { m_depthWrite = b; return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDepthFunc(int v) { m_depthCompare = Compare(v); return *this; }
IPipelineStateBuilder& VulkanPipelineStateBuilder::SetRenderTargetFormat(int, int) { return *this; }

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetInputLayout(const VertexElement* elements, uint32_t count)
{
    m_bindings.clear(); m_attributes.clear();
    for (uint32_t i = 0; elements && i < count; ++i)
    {
        auto binding = std::find_if(m_bindings.begin(), m_bindings.end(), [&](const auto& b) { return b.binding == elements[i].inputSlot; });
        if (binding == m_bindings.end())
        {
            VkVertexInputBindingDescription desc{};
            desc.binding = elements[i].inputSlot;
            desc.inputRate = elements[i].perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            m_bindings.push_back(desc);
        }
        VkVertexInputAttributeDescription attribute{};
        attribute.location = i;
        attribute.binding = elements[i].inputSlot;
        attribute.format = elements[i].format == 6 ? VK_FORMAT_R32G32B32_SFLOAT
                                                   : static_cast<VkFormat>(elements[i].format);
        attribute.offset = elements[i].alignedByteOffset;
        m_attributes.push_back(attribute);
    }
    // The current engine uses one POSITION/NORMAL stream of float3 values.
    for (auto& binding : m_bindings) binding.stride = 24;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetPrimitiveTopology(PrimitiveTopology t)
{
    switch (t)
    {
        case PrimitiveTopology::PointList: m_topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case PrimitiveTopology::LineList: m_topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case PrimitiveTopology::LineStrip: m_topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case PrimitiveTopology::TriangleList: m_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case PrimitiveTopology::TriangleStrip: m_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    }
    return *this;
}

std::unique_ptr<IPipelineState> VulkanPipelineStateBuilder::Build()
{
    try
    {
        if (m_vs.empty() || m_ps.empty()) throw std::runtime_error("Vulkan pipeline requires vertex and pixel shaders");
        VkShaderModuleCreateInfo moduleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
        moduleInfo.codeSize = m_vs.size(); moduleInfo.pCode = reinterpret_cast<const uint32_t*>(m_vs.data());
        VkCheck(vkCreateShaderModule(m_device, &moduleInfo, nullptr, &vs), "vkCreateShaderModule(VS)");
        moduleInfo.codeSize = m_ps.size(); moduleInfo.pCode = reinterpret_cast<const uint32_t*>(m_ps.data());
        VkCheck(vkCreateShaderModule(m_device, &moduleInfo, nullptr, &ps), "vkCreateShaderModule(PS)");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "VSMain";
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = ps; stages[1].pName = "PSMain";
        VkPipelineVertexInputStateCreateInfo vertex{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertex.vertexBindingDescriptionCount = static_cast<uint32_t>(m_bindings.size()); vertex.pVertexBindingDescriptions = m_bindings.data();
        vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(m_attributes.size()); vertex.pVertexAttributeDescriptions = m_attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = m_topology;
        VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1; viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = m_polygonMode; raster.cullMode = m_cullMode; raster.frontFace = m_frontFace; raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState colorBlend{};
        colorBlend.blendEnable = m_blend; colorBlend.srcColorBlendFactor = m_srcBlend; colorBlend.dstColorBlendFactor = m_dstBlend;
        colorBlend.colorBlendOp = m_blendOp; colorBlend.srcAlphaBlendFactor = m_srcBlendAlpha; colorBlend.dstAlphaBlendFactor = m_dstBlendAlpha;
        colorBlend.alphaBlendOp = m_blendOpAlpha; colorBlend.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1; blend.pAttachments = &colorBlend;
        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = m_depth; depth.depthWriteEnable = m_depthWrite; depth.depthCompareOp = m_depthCompare;
        VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = ARRAYSIZE(states); dynamic.pDynamicStates = states;
        VkPushConstantRange push{}; push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; push.size = 128;
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &push;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkCheck(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
        VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineInfo.stageCount = 2; pipelineInfo.pStages = stages; pipelineInfo.pVertexInputState = &vertex;
        pipelineInfo.pInputAssemblyState = &assembly; pipelineInfo.pViewportState = &viewport;
        pipelineInfo.pRasterizationState = &raster; pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &blend; pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pDynamicState = &dynamic; pipelineInfo.layout = layout; pipelineInfo.renderPass = m_renderPass;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        vkDestroyShaderModule(m_device, vs, nullptr); vkDestroyShaderModule(m_device, ps, nullptr);
        if (result != VK_SUCCESS) { vkDestroyPipelineLayout(m_device, layout, nullptr); VkCheck(result, "vkCreateGraphicsPipelines"); }
        return std::make_unique<VulkanPipelineState>(m_device, pipeline, layout);
    }
    catch (const std::exception& error) { m_lastError = error.what(); return nullptr; }
}

VkBlendFactor VulkanPipelineStateBuilder::Blend(int v)
{
    switch (v) { case 0:return VK_BLEND_FACTOR_ZERO; case 1:return VK_BLEND_FACTOR_ONE; case 2:return VK_BLEND_FACTOR_SRC_COLOR;
    case 3:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; case 4:return VK_BLEND_FACTOR_SRC_ALPHA; case 5:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6:return VK_BLEND_FACTOR_DST_ALPHA; case 7:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; case 8:return VK_BLEND_FACTOR_DST_COLOR;
    case 9:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; default:return VK_BLEND_FACTOR_ONE; }
}
VkBlendOp VulkanPipelineStateBuilder::BlendOp(int v) { return static_cast<VkBlendOp>(std::clamp(v, 0, 4)); }
VkCompareOp VulkanPipelineStateBuilder::Compare(int v) { return static_cast<VkCompareOp>(std::clamp(v, 0, 7)); }
#endif
