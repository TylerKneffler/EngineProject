#pragma once

#include <mutex>

namespace Engine::Renderers
{
// Native texture uploads may submit from the simulation owner while the
// render owner submits a frame. Keep each submit and its fence signal ordered.
inline std::mutex& DX12QueueMutex()
{
    static std::mutex mutex;
    return mutex;
}
}
