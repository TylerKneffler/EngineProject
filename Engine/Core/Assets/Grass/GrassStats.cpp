#include "Grass/GrassStats.h"

#include "Core/Scene/Scene.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace Engine::Grass
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

void Publish(Engine::Scene::SceneCounters& counters, const GrassPerformance& value)
{
    using Engine::Scene::CounterUnit;
    constexpr const char* nested = "Grass total and nested work (CPU wall time)";
    constexpr const char* phases = "Grass exclusive phases (CPU wall time)";
    constexpr const char* work = "Grass";
    const auto ms = [&](const char* key, const char* group, const char* label, double v)
    { counters.Add(key, group, label, CounterUnit::Milliseconds, v); };
    const auto count = [&](const char* key, const char* label, uint64_t v)
    { counters.Add(key, work, label, CounterUnit::Count, static_cast<double>(v)); };
    const auto bytes = [&](const char* key, const char* label, uint64_t v)
    { counters.Add(key, work, label, CounterUnit::Bytes, static_cast<double>(v)); };

    ms("grass_total_ms", nested, "Total", value.totalMs);
    ms("grass_scan_ms", nested, "Placement indexing", value.scanMs);
    ms("grass_generation_ms", nested, "Instance preparation", value.generationMs);
    ms("grass_upload_ms", nested, "GPU upload", value.uploadMs);
    ms("grass_references_ms", phases, "References", value.referencesMs);
    ms("grass_sources_ms", phases, "Sources", value.sourcesMs);
    ms("grass_placement_ms", phases, "Placement configuration", value.placementMs);
    ms("grass_visibility_ms", phases, "Visibility and tiles", value.visibilityMs);
    ms("grass_retirement_ms", phases, "Retirement", value.retirementMs);
    ms("grass_scheduling_ms", phases, "Scheduling", value.schedulingMs);
    ms("grass_build_ms", phases, "Build and activation", value.buildMs);
    ms("grass_accounting_ms", phases, "Resource accounting", value.accountingMs);
    ms("grass_other_ms", phases, "Other", value.otherMs);
    count("grass_pending", "Pending tiles", value.pending);
    count("grass_lod_switches", "LOD switches", value.lodSwitches);
    count("grass_tiles_built", "Instance tiles prepared", value.tilesBuilt);
    count("grass_placements_scanned", "Placements scanned", value.placementsScanned);
    count("grass_resident_lods", "Resident LODs", value.residentLods);
    bytes("grass_gpu_bytes", "GPU memory", value.gpuBytes);
    count("grass_instances", "Placement instances", value.instances);
    bytes("grass_instance_upload_bytes", "Placement uploads", value.instanceUploadBytes);
    count("grass_active_tiles", "Active tiles", value.activeTiles);
    count("grass_lod_transition_tiles", "LOD transition tiles", value.transitionTiles);
    count("grass_fading_tiles", "Fading tiles", value.fadingTiles);
}
}

GrassStats& GrassStats::For(Engine::Scene::Scene& scene)
{
    GrassStats* stats = nullptr;
    {
        std::lock_guard lock(g_storesMutex);
        auto& entry = Stores()[&scene];
        if (!entry)
        {
            entry.reset(new GrassStats(scene));
            entry->m_counterFrame = scene.GetCounters().GetFrame();
            scene.AddObserver(entry.get());
        }
        stats = entry.get();
    }
    stats->SyncFrame();
    return *stats;
}

void GrassStats::SyncFrame()
{
    const uint64_t frame = m_scene.GetCounters().GetFrame();
    if (frame == m_counterFrame) return;
    m_counterFrame = frame;
    m_frame = {};
    m_gpuResources.clear();
}

void GrassStats::Add(const GrassPerformance& value)
{
    SyncFrame();
    m_frame += value;
    Publish(m_scene.GetCounters(), value);
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
