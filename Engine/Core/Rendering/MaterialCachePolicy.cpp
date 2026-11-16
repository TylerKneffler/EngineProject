#include "Core/Rendering/MaterialCachePolicy.h"

namespace Engine::Rendering
{
MaterialCachePolicy& MaterialCachePolicy::Get()
{
    static MaterialCachePolicy policy;
    return policy;
}

void MaterialCachePolicy::SetProjectSettings(
    const Engine::Model::MaterialCacheSettings& settings)
{
    std::lock_guard lock(m_mutex);
    m_project = settings;
    m_project.Normalize();
    PublishLocked();
}

void MaterialCachePolicy::SetOverride(
    const Engine::Model::MaterialCacheSettings& settings)
{
    std::lock_guard lock(m_mutex);
    m_override = settings;
    m_override->Normalize();
    PublishLocked();
}

void MaterialCachePolicy::ClearOverride()
{
    std::lock_guard lock(m_mutex);
    m_override.reset();
    PublishLocked();
}

Engine::Model::MaterialCacheSettings MaterialCachePolicy::GetProjectSettings() const
{
    std::lock_guard lock(m_mutex);
    return m_project;
}

std::optional<Engine::Model::MaterialCacheSettings> MaterialCachePolicy::GetOverride() const
{
    std::lock_guard lock(m_mutex);
    return m_override;
}

bool MaterialCachePolicy::HasOverride() const
{
    std::lock_guard lock(m_mutex);
    return m_override.has_value();
}

void MaterialCachePolicy::PublishLocked()
{
    const auto& effective = m_override ? *m_override : m_project;
    if (effective == Effective())
        return;
    m_renderMaterialCache.store(effective.renderMaterialCache, std::memory_order_relaxed);
    m_descriptorSetsPerPool.store(effective.descriptorSetsPerPool, std::memory_order_relaxed);
    m_descriptorCacheLimit.store(effective.descriptorCacheLimit, std::memory_order_relaxed);
    m_revision.fetch_add(1, std::memory_order_release);
}
}
