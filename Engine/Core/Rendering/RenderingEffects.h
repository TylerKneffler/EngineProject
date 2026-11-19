#pragma once

#include "Core/Model/RenderingEffectsSettings.h"
#include <mutex>

namespace Engine::Rendering
{
// The project's rendering effects settings (ProjectSettings::renderingEffects),
// shared by every scene renderer. The editor and game configure it when the
// project loads and whenever Preferences change; renderers read a copy per
// frame. Unconfigured, it holds the defaults.
class RenderingEffects
{
public:
    static RenderingEffects& Get()
    {
        static RenderingEffects instance;
        return instance;
    }

    void Configure(Engine::Model::RenderingEffectsSettings settings)
    {
        settings.Clamp();
        std::lock_guard lock(m_mutex);
        m_settings = settings;
    }

    Engine::Model::RenderingEffectsSettings Settings() const
    {
        std::lock_guard lock(m_mutex);
        return m_settings;
    }

private:
    mutable std::mutex m_mutex;
    Engine::Model::RenderingEffectsSettings m_settings;
};
}
