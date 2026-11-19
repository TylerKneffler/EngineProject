#include "pch.h"
#include "Core/Graphics/DepthConvention.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanPostProcess.h"
#include "VulkanRenderCore.h"
#include "VulkanRenderPassScope.h"

#ifndef ENGINE_VULKAN_SHADER_PATH
#define ENGINE_VULKAN_SHADER_PATH "VulkanShaders/"
#endif

namespace Engine::Renderers
{
namespace
{
constexpr VkFormat HdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Exposure results: full float, per-frame adaptation steps are far below
// half precision.
constexpr VkFormat ExposureFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

std::vector<uint32_t> LoadShader(const char* name)
{
    std::filesystem::path path = std::filesystem::path("VulkanShaders") / name;
    if (!std::filesystem::is_regular_file(path))
        path = std::filesystem::path(ENGINE_VULKAN_SHADER_PATH) / name;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Vulkan post-process shader not found: " + path.string());
    const auto size = static_cast<size_t>(stream.tellg());
    std::vector<uint32_t> words((size + 3u) / 4u);
    stream.seekg(0); stream.read(reinterpret_cast<char*>(words.data()), size);
    return words;
}

VkShaderModule CreateModule(VkDevice device, const char* name)
{
    const auto words = LoadShader(name);
    VkShaderModuleCreateInfo module{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    module.codeSize = words.size() * sizeof(uint32_t); module.pCode = words.data();
    VkShaderModule result = VK_NULL_HANDLE;
    VkCheck(vkCreateShaderModule(device, &module, nullptr, &result), name);
    return result;
}

// Matches the D3D convention used by the shared HLSL: uv (0, 0) at the top.
void SetFlippedViewport(VkCommandBuffer command, uint32_t width, uint32_t height)
{
    VkViewport viewport{ 0.f, static_cast<float>(height), static_cast<float>(width),
        -static_cast<float>(height), 0.f, 1.f };
    VkRect2D scissor{ {0, 0}, {width, height} };
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
}
}

VulkanPostProcess::~VulkanPostProcess()
{
    if (!m_device) return;
    DestroyImages();
    DestroyExposureImages();
    if (m_exposurePass) vkDestroyRenderPass(m_device, m_exposurePass, nullptr);
    for (VkPipeline pipeline : { m_exposureLuminancePipeline, m_exposureHistogramPipeline,
        m_exposureAdaptPipeline })
        if (pipeline) vkDestroyPipeline(m_device, pipeline, nullptr);
    if (m_sceneLoadPass) vkDestroyRenderPass(m_device,m_sceneLoadPass,nullptr);
    if (m_bloomClearPass) vkDestroyRenderPass(m_device, m_bloomClearPass, nullptr);
    if (m_bloomLoadPass) vkDestroyRenderPass(m_device, m_bloomLoadPass, nullptr);
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_bloomDownsamplePipeline) vkDestroyPipeline(m_device, m_bloomDownsamplePipeline, nullptr);
    if (m_bloomUpsamplePipeline) vkDestroyPipeline(m_device, m_bloomUpsamplePipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descriptorPool) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_descriptorLayout) vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
}

VkPipeline VulkanPostProcess::CreatePipeline(VkShaderModule vs, VkShaderModule ps,
    VkRenderPass renderPass, bool additive)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "VSMain";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = ps; stages[1].pName = "PSMain";
    VkPipelineVertexInputStateCreateInfo vertex{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO }; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO }; viewport.viewportCount = 1; viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO }; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = 0xf;
    if (additive)
    {
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.srcAlphaBlendFactor = attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; blend.attachmentCount = 1; blend.pAttachments = &attachment;
    VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO }; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertex; info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend; info.pDepthStencilState = &depth; info.pDynamicState = &dynamic;
    info.layout = m_pipelineLayout; info.renderPass = renderPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCheck(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines(post process)");
    return pipeline;
}

void VulkanPostProcess::Init(VkPhysicalDevice physicalDevice, VkDevice device,
    VkRenderPass sceneRenderPass, VkRenderPass outputRenderPass,
    uint32_t width, uint32_t height)
{
    using Engine::Graphics::MaxBloomLevels;
    m_physicalDevice = physicalDevice; m_device = device;
    m_sceneRenderPass = sceneRenderPass; m_outputRenderPass = outputRenderPass;
    m_sceneLoadPass = VulkanRenderCore::CreateRenderPassForDevice(device,
        HdrFormat,true,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,true);
    m_bloomClearPass = VulkanRenderCore::CreateRenderPassForDevice(device,
        HdrFormat, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    m_bloomLoadPass = VulkanRenderCore::CreateRenderPassForDevice(device,
        HdrFormat, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
    m_exposurePass = VulkanRenderCore::CreateRenderPassForDevice(device,
        ExposureFormat, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.f; VkCheck(vkCreateSampler(device, &sampler, nullptr, &m_sampler), "vkCreateSampler(post process)");
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t binding = 0; binding < 4; ++binding)
    {
        bindings[binding].binding = binding; bindings[binding].descriptorCount = 1;
        bindings[binding].descriptorType = binding == 1
            ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layout.bindingCount = 4; layout.pBindings = bindings;
    VkCheck(vkCreateDescriptorSetLayout(device, &layout, nullptr, &m_descriptorLayout), "vkCreateDescriptorSetLayout(post process)");
    // Compose 0, bloom levels, compose 1, exposure map, adapt 0 and 1.
    constexpr uint32_t setCount = 1u + MaxBloomLevels + 4u;
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3u * setCount }, { VK_DESCRIPTOR_TYPE_SAMPLER, setCount } };
    VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = setCount; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
    VkCheck(vkCreateDescriptorPool(device, &pool, nullptr, &m_descriptorPool), "vkCreateDescriptorPool(post process)");
    std::array<VkDescriptorSetLayout, setCount> layouts;
    layouts.fill(m_descriptorLayout);
    std::array<VkDescriptorSet, setCount> sets{};
    VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocation.descriptorPool = m_descriptorPool; allocation.descriptorSetCount = setCount; allocation.pSetLayouts = layouts.data();
    VkCheck(vkAllocateDescriptorSets(device, &allocation, sets.data()), "vkAllocateDescriptorSets(post process)");
    m_descriptorSet = sets[0];
    for (uint32_t level = 0; level < MaxBloomLevels; ++level)
        m_bloomSets[level] = sets[1 + level];
    m_composeSet1 = sets[1 + MaxBloomLevels];
    m_exposureMapSet = sets[2 + MaxBloomLevels];
    m_adaptSets[0] = sets[3 + MaxBloomLevels];
    m_adaptSets[1] = sets[4 + MaxBloomLevels];
    VkPushConstantRange push{}; push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(Engine::Graphics::PostProcessConstants);
    VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayout.setLayoutCount = 1; pipelineLayout.pSetLayouts = &m_descriptorLayout;
    pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &push;
    VkCheck(vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &m_pipelineLayout), "vkCreatePipelineLayout(post process)");
    VkShaderModule vs = CreateModule(device, "PostProcess.VS.spv");
    VkShaderModule ps = CreateModule(device, "PostProcess.PS.spv");
    VkShaderModule down = CreateModule(device, "PostProcess.BloomDown.PS.spv");
    VkShaderModule up = CreateModule(device, "PostProcess.BloomUp.PS.spv");
    m_pipeline = CreatePipeline(vs, ps, outputRenderPass, false);
    m_bloomDownsamplePipeline = CreatePipeline(vs, down, m_bloomClearPass, false);
    m_bloomUpsamplePipeline = CreatePipeline(vs, up, m_bloomLoadPass, true);
    VkShaderModule luminance = CreateModule(device, "PostProcess.ExposureLuminance.PS.spv");
    VkShaderModule histogram = CreateModule(device, "PostProcess.ExposureHistogram.PS.spv");
    VkShaderModule adapt = CreateModule(device, "PostProcess.ExposureAdapt.PS.spv");
    m_exposureLuminancePipeline = CreatePipeline(vs, luminance, m_bloomClearPass, false);
    m_exposureHistogramPipeline = CreatePipeline(vs, histogram, m_bloomClearPass, false);
    m_exposureAdaptPipeline = CreatePipeline(vs, adapt, m_exposurePass, false);
    for (VkShaderModule module : { vs, ps, down, up, luminance, histogram, adapt })
        vkDestroyShaderModule(device, module, nullptr);
    CreateExposureImages();
    CreateImages(width, height);
}

void VulkanPostProcess::CreateExposureImages()
{
    using namespace Engine::Graphics;
    const auto create = [&](uint32_t width, uint32_t height, VkFormat format,
        VkRenderPass pass, VulkanImageResource& image, VkFramebuffer& framebuffer)
    {
        image = VulkanCreateImage(m_physicalDevice, m_device, width, height, format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        info.renderPass = pass; info.attachmentCount = 1; info.pAttachments = &image.view;
        info.width = width; info.height = height; info.layers = 1;
        VkCheck(vkCreateFramebuffer(m_device, &info, nullptr, &framebuffer), "vkCreateFramebuffer(exposure)");
    };
    create(ExposureMapWidth, ExposureMapHeight, HdrFormat, m_bloomClearPass,
        m_exposureMap, m_exposureMapFramebuffer);
    create(ExposureBins, 1, HdrFormat, m_bloomClearPass,
        m_exposureHistogram, m_exposureHistogramFramebuffer);
    for (uint32_t index = 0; index < 2; ++index)
        create(1, 1, ExposureFormat, m_exposurePass, m_exposure[index],
            m_exposureFramebuffers[index]);
    m_exposureImagesReady = false;
    m_exposureState.Reset();
}

void VulkanPostProcess::DestroyExposureImages()
{
    for (VkFramebuffer* framebuffer : { &m_exposureMapFramebuffer,
        &m_exposureHistogramFramebuffer, &m_exposureFramebuffers[0], &m_exposureFramebuffers[1] })
    {
        if (*framebuffer) vkDestroyFramebuffer(m_device, *framebuffer, nullptr);
        *framebuffer = VK_NULL_HANDLE;
    }
    VulkanDestroyImage(m_device, m_exposureMap);
    VulkanDestroyImage(m_device, m_exposureHistogram);
    VulkanDestroyImage(m_device, m_exposure[0]);
    VulkanDestroyImage(m_device, m_exposure[1]);
}

void VulkanPostProcess::DestroyImages()
{
    for (auto& framebuffer : m_bloomFramebuffers)
    {
        if (framebuffer) vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    for (auto& view : m_bloomViews)
    {
        if (view) vkDestroyImageView(m_device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    VulkanDestroyImage(m_device, m_bloom);
    if (m_sceneFramebuffer) vkDestroyFramebuffer(m_device, m_sceneFramebuffer, nullptr);
    VulkanDestroyImage(m_device, m_depth); VulkanDestroyImage(m_device, m_sceneColor);
    m_sceneFramebuffer = VK_NULL_HANDLE;
}

void VulkanPostProcess::CreateImages(uint32_t width, uint32_t height)
{
    m_width = width; m_height = height;
    m_sceneColor = VulkanCreateImage(m_physicalDevice, m_device, width, height,
        HdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    m_depth = VulkanCreateImage(m_physicalDevice, m_device, width, height,
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageView attachments[] = { m_sceneColor.view, m_depth.view };
    VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebuffer.renderPass = m_sceneRenderPass; framebuffer.attachmentCount = 2; framebuffer.pAttachments = attachments;
    framebuffer.width = width; framebuffer.height = height; framebuffer.layers = 1;
    VkCheck(vkCreateFramebuffer(m_device, &framebuffer, nullptr, &m_sceneFramebuffer), "vkCreateFramebuffer(linear scene)");

    m_bloomLevels = Engine::Graphics::BloomLevelCount(width, height);
    const uint32_t bloomWidth = Engine::Graphics::BloomLevelExtent(width, 0);
    const uint32_t bloomHeight = Engine::Graphics::BloomLevelExtent(height, 0);
    m_bloom = VulkanCreateImage(m_physicalDevice, m_device, bloomWidth, bloomHeight,
        HdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, m_bloomLevels);
    for (uint32_t level = 0; level < m_bloomLevels; ++level)
    {
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = m_bloom.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = HdrFormat;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 };
        VkCheck(vkCreateImageView(m_device, &view, nullptr, &m_bloomViews[level]), "vkCreateImageView(bloom)");
        VkFramebufferCreateInfo levelFramebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        levelFramebuffer.renderPass = m_bloomClearPass; levelFramebuffer.attachmentCount = 1;
        levelFramebuffer.pAttachments = &m_bloomViews[level];
        levelFramebuffer.width = std::max(1u, bloomWidth >> level);
        levelFramebuffer.height = std::max(1u, bloomHeight >> level);
        levelFramebuffer.layers = 1;
        VkCheck(vkCreateFramebuffer(m_device, &levelFramebuffer, nullptr, &m_bloomFramebuffers[level]), "vkCreateFramebuffer(bloom)");
    }

    // Each set's unused image bindings point at the scene, which is always
    // shader-readable while post-processing renders.
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSet> writes;
    images.reserve(4u * (5u + m_bloomLevels));
    const auto writeSet = [&](VkDescriptorSet set, VkImageView source, VkImageView secondary,
        VkImageView tertiary)
    {
        const VkImageView views[4] = { source, VK_NULL_HANDLE, secondary, tertiary };
        for (uint32_t binding = 0; binding < 4; ++binding)
        {
            VkDescriptorImageInfo info{};
            if (binding == 1) info.sampler = m_sampler;
            else { info.imageView = views[binding]; info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
            images.push_back(info);
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set; write.dstBinding = binding; write.descriptorCount = 1;
            write.descriptorType = binding == 1 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &images.back();
            writes.push_back(write);
        }
    };
    const VkImageView scene = m_sceneColor.view;
    writeSet(m_descriptorSet, scene, m_bloomViews[0], m_exposure[0].view);
    writeSet(m_composeSet1, scene, m_bloomViews[0], m_exposure[1].view);
    for (uint32_t level = 0; level < m_bloomLevels; ++level)
        writeSet(m_bloomSets[level], m_bloomViews[level], scene, scene);
    writeSet(m_exposureMapSet, m_exposureMap.view, scene, scene);
    writeSet(m_adaptSets[0], m_exposureHistogram.view, m_exposure[1].view, scene);
    writeSet(m_adaptSets[1], m_exposureHistogram.view, m_exposure[0].view, scene);
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VulkanPostProcess::Resize(uint32_t width, uint32_t height)
{ if (width && height && (width != m_width || height != m_height)) { DestroyImages(); CreateImages(width, height); } }

void VulkanPostProcess::BeginScene(VkCommandBuffer command, const float clearColor[4])
{
    VkClearValue clears[2]{}; clears[0].color = { { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } }; clears[1].depthStencil = { Engine::Graphics::Depth::ClearValue, 0 };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO }; begin.renderPass = m_sceneRenderPass; begin.framebuffer = m_sceneFramebuffer;
    begin.renderArea.extent = { m_width, m_height }; begin.clearValueCount = 2; begin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    RegisterVulkanScenePass(command,m_sceneLoadPass,m_sceneFramebuffer,m_width,m_height,m_depth.image);
    SetFlippedViewport(command, m_width, m_height);
}
void VulkanPostProcess::EndScene(VkCommandBuffer command)
{ ResumeVulkanScenePass(command); vkCmdEndRenderPass(command); VulkanScenePassScopes().erase(command); }

void VulkanPostProcess::RenderBloom(VkCommandBuffer command)
{
    const uint32_t bloomWidth = Engine::Graphics::BloomLevelExtent(m_width, 0);
    const uint32_t bloomHeight = Engine::Graphics::BloomLevelExtent(m_height, 0);
    const auto drawLevel = [&](uint32_t target, VkDescriptorSet source, bool load,
        uint32_t sourceWidth, uint32_t sourceHeight)
    {
        const uint32_t width = std::max(1u, bloomWidth >> target);
        const uint32_t height = std::max(1u, bloomHeight >> target);
        VkClearValue clear{};
        VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass = load ? m_bloomLoadPass : m_bloomClearPass;
        begin.framebuffer = m_bloomFramebuffers[target];
        begin.renderArea.extent = { width, height };
        begin.clearValueCount = 1; begin.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        SetFlippedViewport(command, width, height);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &source, 0, nullptr);
        const auto constants = Engine::Graphics::MakeBloomPassConstants(sourceWidth, sourceHeight);
        vkCmdPushConstants(command, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
    };
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomDownsamplePipeline);
    drawLevel(0, m_descriptorSet, false, m_width, m_height);
    for (uint32_t level = 1; level < m_bloomLevels; ++level)
        drawLevel(level, m_bloomSets[level - 1], false,
            std::max(1u, bloomWidth >> (level - 1)), std::max(1u, bloomHeight >> (level - 1)));
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomUpsamplePipeline);
    for (uint32_t level = m_bloomLevels; level-- > 1;)
        drawLevel(level - 1, m_bloomSets[level], true,
            std::max(1u, bloomWidth >> level), std::max(1u, bloomHeight >> level));
}

VkDescriptorSet VulkanPostProcess::RenderExposure(VkCommandBuffer command,
    const Engine::Graphics::PostProcessSettings& settings)
{
    using namespace Engine::Graphics;
    const ExposureFrame frame = m_exposureState.Next();
    const auto constants = MakeExposureConstants(settings, frame.deltaSeconds, frame.history);
    const auto pass = [&](VkPipeline pipeline, VkRenderPass renderPass,
        VkFramebuffer framebuffer, uint32_t width, uint32_t height, VkDescriptorSet source)
    {
        VkClearValue clear{};
        VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass = renderPass; begin.framebuffer = framebuffer;
        begin.renderArea.extent = { width, height };
        begin.clearValueCount = 1; begin.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        SetFlippedViewport(command, width, height);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &source, 0, nullptr);
        vkCmdPushConstants(command, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
    };
    pass(m_exposureLuminancePipeline, m_bloomClearPass, m_exposureMapFramebuffer,
        ExposureMapWidth, ExposureMapHeight, m_descriptorSet);
    pass(m_exposureHistogramPipeline, m_bloomClearPass, m_exposureHistogramFramebuffer,
        ExposureBins, 1, m_exposureMapSet);
    pass(m_exposureAdaptPipeline, m_exposurePass, m_exposureFramebuffers[frame.target],
        1, 1, m_adaptSets[frame.target]);
    return frame.target ? m_composeSet1 : m_descriptorSet;
}

void VulkanPostProcess::BeginComposition(VkCommandBuffer command, VkFramebuffer output,
    uint32_t width, uint32_t height, const float clearColor[4])
{
    auto settings = Engine::Graphics::GetPostProcessSettings();
    if (!m_exposureImagesReady)
    {
        // Clear passes move the exposure images into the shader-read layout
        // every descriptor set referencing them expects.
        VkClearValue clear{};
        const auto clearImage = [&](VkRenderPass pass, VkFramebuffer framebuffer,
            uint32_t imageWidth, uint32_t imageHeight)
        {
            VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            begin.renderPass = pass; begin.framebuffer = framebuffer;
            begin.renderArea.extent = { imageWidth, imageHeight };
            begin.clearValueCount = 1; begin.pClearValues = &clear;
            vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(command);
        };
        clearImage(m_bloomClearPass, m_exposureMapFramebuffer,
            Engine::Graphics::ExposureMapWidth, Engine::Graphics::ExposureMapHeight);
        clearImage(m_bloomClearPass, m_exposureHistogramFramebuffer,
            Engine::Graphics::ExposureBins, 1);
        for (uint32_t index = 0; index < 2; ++index)
            clearImage(m_exposurePass, m_exposureFramebuffers[index], 1, 1);
        m_exposureImagesReady = true;
    }
    if (settings.bloomIntensity > 0.f && m_bloom.image)
        RenderBloom(command);
    else
        settings.bloomIntensity = 0.f;
    VkDescriptorSet composeSet = m_descriptorSet;
    if (settings.autoExposure && m_exposureMap.image)
        composeSet = RenderExposure(command, settings);
    else
    {
        settings.autoExposure = false;
        m_exposureState.Reset();
    }
    VkClearValue clears[2]{}; clears[0].color = { { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } }; clears[1].depthStencil = { Engine::Graphics::Depth::ClearValue, 0 };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO }; begin.renderPass = m_outputRenderPass; begin.framebuffer = output;
    begin.renderArea.extent = { width, height }; begin.clearValueCount = 2; begin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    SetFlippedViewport(command, width, height);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &composeSet, 0, nullptr);
    const auto constants = Engine::Graphics::MakeComposeConstants(settings, m_bloomLevels);
    vkCmdPushConstants(command, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    vkCmdDraw(command, 3, 1, 0, 0);
}
void VulkanPostProcess::EndComposition(VkCommandBuffer command) { vkCmdEndRenderPass(command); }
}
#endif
