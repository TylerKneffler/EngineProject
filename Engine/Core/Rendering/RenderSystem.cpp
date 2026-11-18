#include "Core/Rendering/RenderSystem.h"

#include "Core/Rendering/SceneRenderer.h"

namespace Engine::Rendering
{
RenderSystem& RenderSystem::Instance()
{
    static RenderSystem system;
    return system;
}

SceneRenderer& RenderSystem::For(Scene& scene)
{
    auto& system = Instance();
    std::unique_ptr<SceneRenderer> replaced;
    {
        std::lock_guard<std::mutex> lock(system.m_mutex);
        auto& slot = system.m_renderers[&scene];
        if (slot && slot->GetGraphicsProvider() == scene.GetGraphicsProvider())
            return *slot;
        replaced = std::move(slot);
    }
    // Build outside the lock: initialisation creates GPU resources and the
    // replaced renderer unregisters from the scene while it is destroyed.
    replaced.reset();
    scene.AssertOwnerThread();
    auto created = std::make_unique<SceneRenderer>(scene, scene.GetGraphicsProvider());
    std::lock_guard<std::mutex> lock(system.m_mutex);
    auto& slot = system.m_renderers[&scene];
    slot = std::move(created);
    return *slot;
}

SceneRenderer* RenderSystem::Find(const Scene& scene)
{
    auto& system = Instance();
    std::lock_guard<std::mutex> lock(system.m_mutex);
    const auto found = system.m_renderers.find(&scene);
    return found != system.m_renderers.end() ? found->second.get() : nullptr;
}

void RenderSystem::Release(const Scene& scene)
{
    auto& system = Instance();
    std::unique_ptr<SceneRenderer> released;
    {
        std::lock_guard<std::mutex> lock(system.m_mutex);
        const auto found = system.m_renderers.find(&scene);
        if (found == system.m_renderers.end())
            return;
        released = std::move(found->second);
        system.m_renderers.erase(found);
    }
}

RenderSystem::Scene::RetiredGraphicsResources RenderSystem::TakeRetiredGraphicsResources(Scene& scene)
{
    auto retired = scene.TakeRetiredGraphicsResources();
    if (auto* renderer = Find(scene))
        renderer->TakeRetiredGraphicsResources(retired);
    return retired;
}
}
