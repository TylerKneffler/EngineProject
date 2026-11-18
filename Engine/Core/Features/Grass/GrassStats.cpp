#include "Core/Features/Grass/GrassStats.h"

#include "Core/Scene/Scene.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace Engine::Features::Grass
{
namespace
{
// Scenes are owned elsewhere; each store removes itself when its scene is
// destroyed. The mutex guards the map only (scenes may live on different
// owner threads, e.g. editor and play scenes).
std::mutex g_storesMutex;
std::unordered_map<const Engine::Scene::Scene*, std::unique_ptr<GrassStats>>& Stores()
{
    static std::unordered_map<const Engine::Scene::Scene*, std::unique_ptr<GrassStats>> stores;
    return stores;
}
}

GrassStats& GrassStats::For(Engine::Scene::Scene& scene)
{
    std::lock_guard lock(g_storesMutex);
    auto& entry = Stores()[&scene];
    if (!entry)
    {
        entry.reset(new GrassStats(scene));
        scene.AddObserver(entry.get());
    }
    return *entry;
}

GrassStats* GrassStats::Find(const Engine::Scene::Scene& scene)
{
    std::lock_guard lock(g_storesMutex);
    const auto found = Stores().find(&scene);
    return found != Stores().end() ? found->second.get() : nullptr;
}

void GrassStats::OnSceneDestroying()
{
    // The scene drops its observers itself; only forget the store.
    std::unique_ptr<GrassStats> self;
    {
        std::lock_guard lock(g_storesMutex);
        const auto found = Stores().find(&m_scene);
        if (found != Stores().end()) self = std::move(found->second), Stores().erase(found);
    }
    // `self` (this) is destroyed here, after the lock is released.
}
}
