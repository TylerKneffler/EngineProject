#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanCommon.h"
#include <map>
namespace Engine::Renderers
{
// depthImage: the scene's D32_SFLOAT_S8_UINT depth (TRANSFER_SRC capable),
// in DEPTH_STENCIL_ATTACHMENT_OPTIMAL while the pass is paused; lets
// compute passes copy it (VulkanGraphicsContext::CopySceneDepth).
struct VulkanScenePassScope { VkRenderPassBeginInfo resume{}; bool paused=false; VkImage depthImage=VK_NULL_HANDLE; };
inline auto& VulkanScenePassScopes()
{ static thread_local std::map<VkCommandBuffer,VulkanScenePassScope> scopes; return scopes; }
inline void RegisterVulkanScenePass(VkCommandBuffer command, VkRenderPass loadPass,
    VkFramebuffer framebuffer, uint32_t width, uint32_t height,
    VkImage depthImage = VK_NULL_HANDLE)
{
    VulkanScenePassScope scope;
    scope.depthImage=depthImage;
    scope.resume = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    scope.resume.renderPass=loadPass; scope.resume.framebuffer=framebuffer;
    scope.resume.renderArea.extent={width,height};
    VulkanScenePassScopes()[command]=scope;
}
inline void PauseVulkanScenePass(VkCommandBuffer command)
{
    auto found=VulkanScenePassScopes().find(command);
    if (found!=VulkanScenePassScopes().end() && !found->second.paused)
    { vkCmdEndRenderPass(command); found->second.paused=true; }
}
inline const VulkanScenePassScope* FindVulkanScenePass(VkCommandBuffer command)
{
    auto found=VulkanScenePassScopes().find(command);
    return found!=VulkanScenePassScopes().end() ? &found->second : nullptr;
}
inline void ResumeVulkanScenePass(VkCommandBuffer command)
{
    auto found=VulkanScenePassScopes().find(command);
    if (found!=VulkanScenePassScopes().end() && found->second.paused)
    { vkCmdBeginRenderPass(command,&found->second.resume,VK_SUBPASS_CONTENTS_INLINE); found->second.paused=false; }
}
}
#endif
