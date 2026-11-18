#pragma once

#include "Grass/GrassPerformance.h"
#include "Core/Scene/SceneObserver.h"

#include <cstdint>
#include <unordered_set>

namespace Engine::Scene { class Scene; }

namespace Engine::Grass
{
// Per-scene GrassField totals for the current frame. Fields read them for the
// shared tile-work budget and to count shared GPU buffers once; every Add is
// also published to the scene's generic counters ("grass_*"), which is all
// the host, profiler and benchmark see. A new frame starts when the host
// resets the scene's counters. Scene owner thread only.
class GrassStats final : public Engine::Scene::ISceneObserver
{
public:
    // The scene's totals for this frame, created on first use.
    static GrassStats& For(Engine::Scene::Scene& scene);

    void Add(const GrassPerformance& value);
    // True the first time a GPU resource is counted this frame, so fields
    // sharing species buffers count their bytes once.
    bool AccountGpuResource(const void* resource)
    { return resource && m_gpuResources.insert(resource).second; }
    const GrassPerformance& Get() const { return m_frame; }

    void OnSceneDestroying() override;

private:
    explicit GrassStats(Engine::Scene::Scene& scene) : m_scene(scene) {}
    void SyncFrame();

    Engine::Scene::Scene& m_scene;
    uint64_t m_counterFrame = 0;
    GrassPerformance m_frame;
    std::unordered_set<const void*> m_gpuResources;
};
}
