#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanCommon.h"
#include <map>
namespace Engine::Renderers
{
struct VulkanScenePassScope { VkRenderPassBeginInfo resume{}; bool paused=false; };
inline auto& VulkanScenePassScopes()
{ static thread_local std::map<VkCommandBuffer,VulkanScenePassScope> scopes; return scopes; }
inline void RegisterVulkanScenePass(VkCommandBuffer command, VkRenderPass loadPass,
    VkFramebuffer framebuffer, uint32_t width, uint32_t height)
{
    VulkanScenePassScope scope;
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
inline void ResumeVulkanScenePass(VkCommandBuffer command)
{
    auto found=VulkanScenePassScopes().find(command);
    if (found!=VulkanScenePassScopes().end() && found->second.paused)
    { vkCmdBeginRenderPass(command,&found->second.resume,VK_SUBPASS_CONTENTS_INLINE); found->second.paused=false; }
}
}
#endif
