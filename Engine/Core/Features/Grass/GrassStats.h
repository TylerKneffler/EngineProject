#pragma once

#include "Core/Features/Grass/GrassPerformance.h"
#include "Core/Scene/SceneObserver.h"

#include <unordered_set>

namespace Engine::Scene { class Scene; }

namespace Engine::Features::Grass
{
// Per-scene GrassField counters. Every field adds its frame's work; the host
// loop resets them once per frame (GameApplication, EditorApplication), and
// the profiler and benchmark read the sum. Lives with the grass feature so
// the renderer does not carry grass state. Scene owner thread only.
class GrassStats final : public Engine::Scene::ISceneObserver
{
public:
    // The scene's counters, created on first use.
    static GrassStats& For(Engine::Scene::Scene& scene);
    // The scene's counters if any field has reported; never creates.
    static GrassStats* Find(const Engine::Scene::Scene& scene);

    void Reset() { m_frame = {}; m_gpuResources.clear(); }
    void Add(const GrassPerformance& value) { m_frame += value; }
    // True the first time a GPU resource is counted this frame, so fields
    // sharing species buffers count their bytes once.
    bool AccountGpuResource(const void* resource)
    { return resource && m_gpuResources.insert(resource).second; }
    const GrassPerformance& Get() const { return m_frame; }

    void OnSceneDestroying() override;

private:
    explicit GrassStats(Engine::Scene::Scene& scene) : m_scene(scene) {}
    Engine::Scene::Scene& m_scene;
    GrassPerformance m_frame;
    std::unordered_set<const void*> m_gpuResources;
};
}
