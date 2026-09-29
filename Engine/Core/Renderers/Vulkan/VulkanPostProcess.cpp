#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanPostProcess.h"
#include "Core/Graphics/PostProcess.h"
#include <filesystem>
#include <fstream>

#ifndef ENGINE_VULKAN_SHADER_PATH
#define ENGINE_VULKAN_SHADER_PATH "VulkanShaders/"
#endif

namespace Engine::Renderers
{
namespace
{
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
}

VulkanPostProcess::~VulkanPostProcess()
{
    if (!m_device) return;
    DestroyImages();
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descriptorPool) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_descriptorLayout) vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
}

void VulkanPostProcess::Init(VkPhysicalDevice physicalDevice, VkDevice device,
    VkRenderPass sceneRenderPass, VkRenderPass outputRenderPass,
    uint32_t width, uint32_t height)
{
    m_physicalDevice = physicalDevice; m_device = device;
    m_sceneRenderPass = sceneRenderPass; m_outputRenderPass = outputRenderPass;
    VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.f; VkCheck(vkCreateSampler(device, &sampler, nullptr, &m_sampler), "vkCreateSampler(post process)");
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0; bindings[0].descriptorCount = 1; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1; bindings[1].descriptorCount = 1; bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layout.bindingCount = 2; layout.pBindings = bindings;
    VkCheck(vkCreateDescriptorSetLayout(device, &layout, nullptr, &m_descriptorLayout), "vkCreateDescriptorSetLayout(post process)");
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 } };
    VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = 1; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
    VkCheck(vkCreateDescriptorPool(device, &pool, nullptr, &m_descriptorPool), "vkCreateDescriptorPool(post process)");
    VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocation.descriptorPool = m_descriptorPool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &m_descriptorLayout;
    VkCheck(vkAllocateDescriptorSets(device, &allocation, &m_descriptorSet), "vkAllocateDescriptorSets(post process)");
    VkPushConstantRange push{}; push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; push.size = 16;
    VkPipelineLayoutCreateInfo pipelineLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayout.setLayoutCount = 1; pipelineLayout.pSetLayouts = &m_descriptorLayout;
    pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &push;
    VkCheck(vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &m_pipelineLayout), "vkCreatePipelineLayout(post process)");
    auto vsWords = LoadShader("PostProcess.VS.spv"), psWords = LoadShader("PostProcess.PS.spv");
    VkShaderModuleCreateInfo module{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    module.codeSize = vsWords.size() * sizeof(uint32_t); module.pCode = vsWords.data(); VkCheck(vkCreateShaderModule(device, &module, nullptr, &vs), "vkCreateShaderModule(post VS)");
    module.codeSize = psWords.size() * sizeof(uint32_t); module.pCode = psWords.data(); VkCheck(vkCreateShaderModule(device, &module, nullptr, &ps), "vkCreateShaderModule(post PS)");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "VSMain";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = ps; stages[1].pName = "PSMain";
    VkPipelineVertexInputStateCreateInfo vertex{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO }; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO }; viewport.viewportCount = 1; viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO }; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; blend.attachmentCount = 1; blend.pAttachments = &attachment;
    VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO }; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertex; info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend; info.pDepthStencilState = &depth; info.pDynamicState = &dynamic;
    info.layout = m_pipelineLayout; info.renderPass = outputRenderPass;
    VkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline), "vkCreateGraphicsPipelines(post process)");
    vkDestroyShaderModule(device, vs, nullptr); vkDestroyShaderModule(device, ps, nullptr);
    CreateImages(width, height);
}

void VulkanPostProcess::DestroyImages()
{
    if (m_sceneFramebuffer) vkDestroyFramebuffer(m_device, m_sceneFramebuffer, nullptr);
    VulkanDestroyImage(m_device, m_depth); VulkanDestroyImage(m_device, m_sceneColor);
    m_sceneFramebuffer = VK_NULL_HANDLE;
}

void VulkanPostProcess::CreateImages(uint32_t width, uint32_t height)
{
    m_width = width; m_height = height;
    m_sceneColor = VulkanCreateImage(m_physicalDevice, m_device, width, height,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    m_depth = VulkanCreateImage(m_physicalDevice, m_device, width, height,
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageView attachments[] = { m_sceneColor.view, m_depth.view };
    VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebuffer.renderPass = m_sceneRenderPass; framebuffer.attachmentCount = 2; framebuffer.pAttachments = attachments;
    framebuffer.width = width; framebuffer.height = height; framebuffer.layers = 1;
    VkCheck(vkCreateFramebuffer(m_device, &framebuffer, nullptr, &m_sceneFramebuffer), "vkCreateFramebuffer(linear scene)");
    VkDescriptorImageInfo image{}; image.imageView = m_sceneColor.view; image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo sampler{}; sampler.sampler = m_sampler;
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; writes[0].dstSet = m_descriptorSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].pImageInfo = &image;
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; writes[1].dstSet = m_descriptorSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; writes[1].pImageInfo = &sampler;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
}

void VulkanPostProcess::Resize(uint32_t width, uint32_t height)
{ if (width && height && (width != m_width || height != m_height)) { DestroyImages(); CreateImages(width, height); } }

void VulkanPostProcess::BeginScene(VkCommandBuffer command, const float clearColor[4])
{
    VkClearValue clears[2]{}; clears[0].color = { { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } }; clears[1].depthStencil = { 1.f, 0 };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO }; begin.renderPass = m_sceneRenderPass; begin.framebuffer = m_sceneFramebuffer;
    begin.renderArea.extent = { m_width, m_height }; begin.clearValueCount = 2; begin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{ 0.f, static_cast<float>(m_height), static_cast<float>(m_width), -static_cast<float>(m_height), 0.f, 1.f };
    VkRect2D scissor{ {0,0}, {m_width,m_height} }; vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
}
void VulkanPostProcess::EndScene(VkCommandBuffer command) { vkCmdEndRenderPass(command); }

void VulkanPostProcess::BeginComposition(VkCommandBuffer command, VkFramebuffer output,
    uint32_t width, uint32_t height, const float clearColor[4])
{
    VkClearValue clears[2]{}; clears[0].color = { { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } }; clears[1].depthStencil = { 1.f, 0 };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO }; begin.renderPass = m_outputRenderPass; begin.framebuffer = output;
    begin.renderArea.extent = { width, height }; begin.clearValueCount = 2; begin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{ 0.f, static_cast<float>(height), static_cast<float>(width), -static_cast<float>(height), 0.f, 1.f };
    VkRect2D scissor{ {0,0}, {width,height} }; vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    const auto settings = Engine::Graphics::GetPostProcessSettings();
    struct Constants { float exposure; uint32_t op; float padding[2]; } constants{ settings.exposure, settings.toneMapping, {} };
    vkCmdPushConstants(command, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    vkCmdDraw(command, 3, 1, 0, 0);
}
void VulkanPostProcess::EndComposition(VkCommandBuffer command) { vkCmdEndRenderPass(command); }
}
#endif
