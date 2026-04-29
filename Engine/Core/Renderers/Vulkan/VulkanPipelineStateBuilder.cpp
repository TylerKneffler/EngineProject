#include "pch.h"
#include "VulkanPipelineStateBuilder.h"
#include "VulkanShaderCompiler.h"

// ---------------------------------------------------------------------------
// VulkanPipelineState
// ---------------------------------------------------------------------------

VulkanPipelineState::VulkanPipelineState(VkDevice         device,
                                         VkPipeline       pipeline,
                                         VkPipelineLayout layout,
                                         VkRenderPass     renderPass)
    : m_device(device)
    , m_pipeline(pipeline)
    , m_layout(layout)
    , m_renderPass(renderPass)
{
}

VulkanPipelineState::~VulkanPipelineState()
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_device != VK_NULL_HANDLE)
    {
        if (m_pipeline   != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
        if (m_layout     != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        if (m_renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    }
#endif
}

// ---------------------------------------------------------------------------
// VulkanPipelineStateBuilder — construction
// ---------------------------------------------------------------------------

VulkanPipelineStateBuilder::VulkanPipelineStateBuilder(VkDevice device)
    : m_device(device)
{
}

// ---------------------------------------------------------------------------
// Shader setters
// ---------------------------------------------------------------------------

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetVertexShader(const IShader* shader)
{
    if (shader && shader->GetBytecodeSize() > 0)
    {
        const auto* data = static_cast<const uint8_t*>(shader->GetBytecode());
        m_vsBytecode.assign(data, data + shader->GetBytecodeSize());
        if (const auto* vk = dynamic_cast<const VulkanShader*>(shader))
            m_vsEntryPoint = vk->GetEntryPoint();
    }
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetPixelShader(const IShader* shader)
{
    if (shader && shader->GetBytecodeSize() > 0)
    {
        const auto* data = static_cast<const uint8_t*>(shader->GetBytecode());
        m_psBytecode.assign(data, data + shader->GetBytecodeSize());
        if (const auto* vk = dynamic_cast<const VulkanShader*>(shader))
            m_psEntryPoint = vk->GetEntryPoint();
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Rasterizer setters
// ---------------------------------------------------------------------------

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetFillMode(bool wireframe)
{
    m_wireframe = wireframe;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetCullMode(bool cullBackFaces)
{
    m_cullBack = cullBackFaces;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetFrontCounterClockwise(bool ccw)
{
    m_frontCCW = ccw;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDepthClipEnable(bool enable)
{
    m_depthClip = enable;
    return *this;
}

// ---------------------------------------------------------------------------
// Blend setters
// ---------------------------------------------------------------------------

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetBlendEnable(bool enable)
{
    m_blendEnable = enable;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetSrcBlend(int mode)
{
    m_srcBlend = mode;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDestBlend(int mode)
{
    m_destBlend = mode;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetBlendOp(int op)
{
    m_blendOp = op;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetSrcBlendAlpha(int mode)
{
    m_srcBlendAlpha = mode;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDestBlendAlpha(int mode)
{
    m_destBlendAlpha = mode;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetBlendOpAlpha(int op)
{
    m_blendOpAlpha = op;
    return *this;
}

// ---------------------------------------------------------------------------
// Depth/stencil setters
// ---------------------------------------------------------------------------

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDepthEnable(bool enable)
{
    m_depthEnable = enable;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDepthWriteEnable(bool enable)
{
    m_depthWriteEnable = enable;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetDepthFunc(int func)
{
    m_depthFunc = func;
    return *this;
}

// ---------------------------------------------------------------------------
// Input layout
// ---------------------------------------------------------------------------

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetInputLayout(
    const VertexElement* elements, uint32_t elementCount)
{
    m_inputElements.clear();
    for (uint32_t i = 0; i < elementCount; ++i)
    {
        InputElement e{};
        e.semantic      = elements[i].semanticName ? elements[i].semanticName : "";
        e.semanticIndex = elements[i].semanticIndex;
        e.format        = elements[i].format;
        e.inputSlot     = elements[i].inputSlot;
        e.byteOffset    = elements[i].alignedByteOffset;
        e.perInstance   = elements[i].perInstance;
        m_inputElements.push_back(std::move(e));
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Topology / RT format
// ---------------------------------------------------------------------------

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetPrimitiveTopology(PrimitiveTopology topology)
{
    m_topology = topology;
    return *this;
}

IPipelineStateBuilder& VulkanPipelineStateBuilder::SetRenderTargetFormat(int format,
                                                                           int depthFormat)
{
    m_rtFormat    = format;
    m_depthFormat = depthFormat;
    return *this;
}

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

#if defined(ENGINE_VULKAN_ENABLED)

VkFormat VulkanPipelineStateBuilder::DxgiToVkFormat(int dxgiFormat) const
{
    // Most common DXGI_FORMAT values used by the engine
    switch (dxgiFormat)
    {
        case  2: return VK_FORMAT_R32G32B32A32_SFLOAT;   // DXGI_FORMAT_R32G32B32A32_FLOAT
        case  6: return VK_FORMAT_R32G32B32_SFLOAT;      // DXGI_FORMAT_R32G32B32_FLOAT
        case 16: return VK_FORMAT_R32G32_SFLOAT;         // DXGI_FORMAT_R32G32_FLOAT
        case 28: return VK_FORMAT_R8G8B8A8_UNORM;        // DXGI_FORMAT_R8G8B8A8_UNORM
        case 40: return VK_FORMAT_D32_SFLOAT;            // DXGI_FORMAT_D32_FLOAT
        case 41: return VK_FORMAT_R32_SFLOAT;            // DXGI_FORMAT_R32_FLOAT
        case 45: return VK_FORMAT_D24_UNORM_S8_UINT;     // DXGI_FORMAT_D24_UNORM_S8_UINT
        case 87: return VK_FORMAT_B8G8R8A8_UNORM;        // DXGI_FORMAT_B8G8R8A8_UNORM
        default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

VkBlendFactor VulkanPipelineStateBuilder::D3DBlendToVk(int d3dBlend) const
{
    // D3D12_BLEND enum values
    switch (d3dBlend)
    {
        case 1:  return VK_BLEND_FACTOR_ZERO;                   // D3D12_BLEND_ZERO
        case 2:  return VK_BLEND_FACTOR_ONE;                    // D3D12_BLEND_ONE
        case 3:  return VK_BLEND_FACTOR_SRC_COLOR;              // D3D12_BLEND_SRC_COLOR
        case 4:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;    // D3D12_BLEND_INV_SRC_COLOR
        case 5:  return VK_BLEND_FACTOR_SRC_ALPHA;              // D3D12_BLEND_SRC_ALPHA
        case 6:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;    // D3D12_BLEND_INV_SRC_ALPHA
        case 7:  return VK_BLEND_FACTOR_DST_ALPHA;              // D3D12_BLEND_DEST_ALPHA
        case 8:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;    // D3D12_BLEND_INV_DEST_ALPHA
        case 9:  return VK_BLEND_FACTOR_DST_COLOR;              // D3D12_BLEND_DEST_COLOR
        case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;    // D3D12_BLEND_INV_DEST_COLOR
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp VulkanPipelineStateBuilder::D3DBlendOpToVk(int d3dOp) const
{
    // D3D12_BLEND_OP enum values
    switch (d3dOp)
    {
        case 1: return VK_BLEND_OP_ADD;              // D3D12_BLEND_OP_ADD
        case 2: return VK_BLEND_OP_SUBTRACT;         // D3D12_BLEND_OP_SUBTRACT
        case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; // D3D12_BLEND_OP_REV_SUBTRACT
        case 4: return VK_BLEND_OP_MIN;              // D3D12_BLEND_OP_MIN
        case 5: return VK_BLEND_OP_MAX;              // D3D12_BLEND_OP_MAX
        default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp VulkanPipelineStateBuilder::D3DCompareToVk(int d3dFunc) const
{
    // D3D12_COMPARISON_FUNC enum values
    switch (d3dFunc)
    {
        case 1: return VK_COMPARE_OP_NEVER;            // D3D12_COMPARISON_FUNC_NEVER
        case 2: return VK_COMPARE_OP_LESS;             // D3D12_COMPARISON_FUNC_LESS
        case 3: return VK_COMPARE_OP_EQUAL;            // D3D12_COMPARISON_FUNC_EQUAL
        case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;    // D3D12_COMPARISON_FUNC_LESS_EQUAL
        case 5: return VK_COMPARE_OP_GREATER;          // D3D12_COMPARISON_FUNC_GREATER
        case 6: return VK_COMPARE_OP_NOT_EQUAL;        // D3D12_COMPARISON_FUNC_NOT_EQUAL
        case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL; // D3D12_COMPARISON_FUNC_GREATER_EQUAL
        case 8: return VK_COMPARE_OP_ALWAYS;           // D3D12_COMPARISON_FUNC_ALWAYS
        default: return VK_COMPARE_OP_LESS;
    }
}

uint32_t VulkanPipelineStateBuilder::SemanticToLocation(const char* semantic) const
{
    if (!semantic) return 0;
    if (_stricmp(semantic, "POSITION")  == 0) return 0;
    if (_stricmp(semantic, "NORMAL")    == 0) return 1;
    if (_stricmp(semantic, "TEXCOORD")  == 0) return 2;
    if (_stricmp(semantic, "COLOR")     == 0) return 3;
    if (_stricmp(semantic, "TANGENT")   == 0) return 4;
    if (_stricmp(semantic, "BINORMAL")  == 0) return 5;
    return 0;
}

#endif // ENGINE_VULKAN_ENABLED

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

std::unique_ptr<IPipelineState> VulkanPipelineStateBuilder::Build()
{
#if !defined(ENGINE_VULKAN_ENABLED)
    m_lastError = "Vulkan not enabled in this build";
    return nullptr;
#else
    if (m_device == VK_NULL_HANDLE)
    {
        m_lastError = "No Vulkan device";
        return nullptr;
    }
    if (m_vsBytecode.empty() || m_psBytecode.empty())
    {
        m_lastError = "Vertex or pixel shader bytecode is missing";
        return nullptr;
    }

    // --- Shader modules ---
    auto createModule = [&](const std::vector<uint8_t>& spv,
                            VkShaderModule& outModule) -> bool
    {
        VkShaderModuleCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = spv.size();
        info.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
        return vkCreateShaderModule(m_device, &info, nullptr, &outModule) == VK_SUCCESS;
    };

    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkShaderModule psModule = VK_NULL_HANDLE;

    if (!createModule(m_vsBytecode, vsModule))
    {
        m_lastError = "Failed to create vertex shader module";
        return nullptr;
    }
    if (!createModule(m_psBytecode, psModule))
    {
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        m_lastError = "Failed to create pixel shader module";
        return nullptr;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName  = m_vsEntryPoint.c_str();

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = psModule;
    stages[1].pName  = m_psEntryPoint.c_str();

    // --- Vertex input ---
    std::vector<VkVertexInputAttributeDescription> attrDescs;
    attrDescs.reserve(m_inputElements.size());
    for (const auto& elem : m_inputElements)
    {
        VkVertexInputAttributeDescription attr{};
        attr.location = SemanticToLocation(elem.semantic.c_str()) + elem.semanticIndex;
        attr.binding  = elem.inputSlot;
        attr.format   = DxgiToVkFormat(elem.format);
        attr.offset   = elem.byteOffset;
        attrDescs.push_back(attr);
    }

    // Single binding (binding 0, per-vertex)
    VkVertexInputBindingDescription bindDesc{};
    bindDesc.binding   = 0;
    bindDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    // Compute stride from attribute offsets + sizes (rough estimate: last offset + 12)
    if (!attrDescs.empty())
    {
        uint32_t maxEnd = 0;
        for (const auto& elem : m_inputElements)
        {
            uint32_t fmtSize = 12; // default R32G32B32
            switch (elem.format)
            {
                case 2:  fmtSize = 16; break; // R32G32B32A32_FLOAT
                case 16: fmtSize = 8;  break; // R32G32_FLOAT
                case 41: fmtSize = 4;  break; // R32_FLOAT
                case 28: fmtSize = 4;  break; // R8G8B8A8_UNORM
                default: fmtSize = 12; break;
            }
            maxEnd = (std::max)(maxEnd, elem.byteOffset + fmtSize);
        }
        bindDesc.stride = maxEnd;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (!attrDescs.empty())
    {
        vertexInput.vertexBindingDescriptionCount   = 1;
        vertexInput.pVertexBindingDescriptions      = &bindDesc;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
        vertexInput.pVertexAttributeDescriptions    = attrDescs.data();
    }

    // --- Input assembly ---
    VkPrimitiveTopology vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (m_topology)
    {
        case PrimitiveTopology::PointList:     vkTopology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;    break;
        case PrimitiveTopology::LineList:      vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;     break;
        case PrimitiveTopology::LineStrip:     vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;    break;
        case PrimitiveTopology::TriangleList:  vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case PrimitiveTopology::TriangleStrip: vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = vkTopology;

    // --- Dynamic viewport/scissor ---
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    // --- Rasterizer ---
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = m_wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = m_cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rasterizer.frontFace   = m_frontCCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;

    // --- Multisample ---
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // --- Depth/stencil ---
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = m_depthEnable      ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = m_depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = D3DCompareToVk(m_depthFunc);

    // --- Blend ---
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable         = m_blendEnable ? VK_TRUE : VK_FALSE;
    colorBlendAttachment.srcColorBlendFactor = D3DBlendToVk(m_srcBlend);
    colorBlendAttachment.dstColorBlendFactor = D3DBlendToVk(m_destBlend);
    colorBlendAttachment.colorBlendOp        = D3DBlendOpToVk(m_blendOp);
    colorBlendAttachment.srcAlphaBlendFactor = D3DBlendToVk(m_srcBlendAlpha);
    colorBlendAttachment.dstAlphaBlendFactor = D3DBlendToVk(m_destBlendAlpha);
    colorBlendAttachment.alphaBlendOp        = D3DBlendOpToVk(m_blendOpAlpha);

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments    = &colorBlendAttachment;

    // --- Render pass (compatible with VulkanView) ---
    const bool hasDepth = (m_depthFormat >= 0);

    VkAttachmentDescription colorAtt{};
    colorAtt.format         = DxgiToVkFormat(m_rtFormat);
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAtt{};
    depthAtt.format         = hasDepth ? DxgiToVkFormat(m_depthFormat) : VK_FORMAT_D32_SFLOAT;
    depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    if (hasDepth) subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::vector<VkAttachmentDescription> attachments = { colorAtt };
    if (hasDepth) attachments.push_back(depthAtt);

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpInfo.pAttachments    = attachments.data();
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies   = &dep;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &renderPass) != VK_SUCCESS)
    {
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        m_lastError = "Failed to create render pass";
        return nullptr;
    }

    // --- Pipeline layout (push constants only) ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = 128;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
    {
        vkDestroyRenderPass(m_device, renderPass, nullptr);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        m_lastError = "Failed to create pipeline layout";
        return nullptr;
    }

    // --- Graphics pipeline ---
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = hasDepth ? &depthStencil : nullptr;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = pipelineLayout;
    pipelineInfo.renderPass          = renderPass;
    pipelineInfo.subpass             = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                  &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
        vkDestroyRenderPass(m_device, renderPass, nullptr);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        m_lastError = "vkCreateGraphicsPipelines failed";
        return nullptr;
    }

    // Shader modules are no longer needed after pipeline creation
    vkDestroyShaderModule(m_device, vsModule, nullptr);
    vkDestroyShaderModule(m_device, psModule, nullptr);

    m_lastError.clear();
    return std::make_unique<VulkanPipelineState>(
        m_device, pipeline, pipelineLayout, renderPass);
#endif
}
