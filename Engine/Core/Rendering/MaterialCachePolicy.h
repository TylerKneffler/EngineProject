#pragma once

#include "Core/Model/MaterialCacheSettings.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace Engine::Rendering
{
// Process-wide material cache settings in effect.
//
// The project's settings are the defaults (SetProjectSettings, called at
// startup and when the editor changes them). A player/user settings layer
// replaces them with SetOverride and restores them with ClearOverride; the
// project defaults are kept underneath, so clearing an override never needs
// the project file again.
//
// Effective() is lock-free and safe to call per draw from any thread.
class MaterialCachePolicy
{
public:
    static MaterialCachePolicy& Get();

    void SetProjectSettings(const Engine::Model::MaterialCacheSettings& settings);
    void SetOverride(const Engine::Model::MaterialCacheSettings& settings);
    void ClearOverride();

    Engine::Model::MaterialCacheSettings GetProjectSettings() const;
    std::optional<Engine::Model::MaterialCacheSettings> GetOverride() const;
    bool HasOverride() const;

    Engine::Model::MaterialCacheSettings Effective() const
    {
        Engine::Model::MaterialCacheSettings result;
        result.renderMaterialCache = m_renderMaterialCache.load(std::memory_order_relaxed);
        result.descriptorSetsPerPool = m_descriptorSetsPerPool.load(std::memory_order_relaxed);
        result.descriptorCacheLimit = m_descriptorCacheLimit.load(std::memory_order_relaxed);
        return result;
    }
    bool RenderMaterialCacheEnabled() const
    { return m_renderMaterialCache.load(std::memory_order_relaxed); }
    // Incremented whenever the effective settings change.
    uint64_t Revision() const { return m_revision.load(std::memory_order_relaxed); }

private:
    void PublishLocked();

    mutable std::mutex m_mutex;
    Engine::Model::MaterialCacheSettings m_project;
    std::optional<Engine::Model::MaterialCacheSettings> m_override;
    std::atomic<bool> m_renderMaterialCache{ true };
    std::atomic<uint32_t> m_descriptorSetsPerPool{ 512 };
    std::atomic<uint32_t> m_descriptorCacheLimit{ 384 };
    std::atomic<uint64_t> m_revision{ 0 };
};
}
