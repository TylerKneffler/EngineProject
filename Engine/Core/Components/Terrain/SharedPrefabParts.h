#pragma once

#include "Core/Components/Mesh/Mesh.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Core { class Object; }

namespace Engine::Components
{
// Shared prefab parts (.md/todo.md "Next" 1): one scene object per distinct
// prefab part configuration (source geometry, material, level, shadow role and
// mesh settings) for the whole scene, instead of one per part per streamed
// layer. Layers attach their placements as world-space blocks (Mesh instance
// blocks); the renderer culls blocks on the CPU and placements on the GPU, so
// cost follows distinct parts, and streaming a cell edits block lists without
// creating objects.
//
// Attach runs on the scene thread inside prefab preparation and creates the
// part's object the first time. A block draws once activated (when its layer
// has finished that asset, as the per-layer objects were enabled). Releasing
// the handle only queues the detach; Flush, once per frame on the scene thread,
// applies every change, so a handle may outlive the scene safely.
// On by default in running scenes (the editor keeps per-layer objects for
// picking and gizmos); ENGINE_SHARED_PREFAB_PARTS=0 turns it off.
class SharedPrefabParts
{
public:
    struct Part
    {
        std::string key, name;
        int projectLayer = 0;
        std::shared_ptr<Mesh> source;
        std::vector<Mesh::InstanceRange> ranges;
        int maximumLod = 0;
        // Sets the new object's mesh fields and adds its material.
        std::function<void(Engine::Core::Object&, Mesh&)> configure;
    };
    using Handle = std::shared_ptr<void>;

    static bool Enabled();
    static std::shared_ptr<SharedPrefabParts> For(Engine::Scene::Scene& scene);
    Handle Attach(Engine::Scene::Scene& scene, const Part& part, Mesh::InstanceBlock block);
    static void Activate(const Handle& handle);
    static void Flush(Engine::Scene::Scene& scene);

    struct Statistics { size_t objects = 0, blocks = 0, placements = 0; };
    Statistics GetStatistics() const;

private:
    struct Block { Mesh::InstanceBlock block; bool active = false; };
    struct Entry
    {
        uint64_t objectId = 0;
        std::map<uint64_t, Block> blocks;
        bool dirty = false;
    };
    struct Token
    {
        std::weak_ptr<SharedPrefabParts> registry;
        std::string key;
        uint64_t id = 0;
        ~Token();
    };
    void Apply(Engine::Scene::Scene& scene);

    mutable std::mutex m_mutex;
    std::map<std::string, Entry> m_entries;
    uint64_t m_nextBlock = 1;
};
}
