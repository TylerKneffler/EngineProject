#pragma once

#include <mutex>

namespace Engine::Renderers
{
// Texture uploads can submit from the simulation owner while the render owner
// submits a frame. Vulkan queues require external synchronization.
inline std::mutex& VulkanQueueMutex()
{
    static std::mutex mutex;
    return mutex;
}
}
