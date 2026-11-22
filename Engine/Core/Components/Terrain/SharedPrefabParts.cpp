#include "Core/Components/Terrain/SharedPrefabParts.h"

#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"

#include <cstdlib>

namespace Engine::Components
{
bool SharedPrefabParts::Enabled()
{
    static const bool enabled = [] {
        // On by default for running scenes (2026-11-22); =0 keeps per-layer objects.
        const char* value = std::getenv("ENGINE_SHARED_PREFAB_PARTS");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

std::shared_ptr<SharedPrefabParts> SharedPrefabParts::For(Engine::Scene::Scene& scene)
{
    auto registry = std::static_pointer_cast<SharedPrefabParts>(scene.sharedPrefabParts);
    if (!registry)
    {
        registry = std::make_shared<SharedPrefabParts>();
        scene.sharedPrefabParts = registry;
    }
    return registry;
}

SharedPrefabParts::Handle SharedPrefabParts::Attach(Engine::Scene::Scene& scene,
    const Part& part, Mesh::InstanceBlock block)
{
    std::lock_guard lock(m_mutex);
    auto& entry = m_entries[part.key];
    auto* object = entry.objectId ? scene.FindObjectById(entry.objectId) : nullptr;
    if (!object)
    {
        // World-space blocks: the object stays at the origin, unparented.
        object = scene.AddObject("Shared prefab part: " + part.name);
        object->runtimeOnly = true;
        object->layer = part.projectLayer;
        object->enabled = false;
        object->transform.position = {0, 0, 0};
        object->transform.rotation = {0, 0, 0};
        object->transform.scale = {1, 1, 1};
        auto* mesh = object->AddComponent<Mesh>();
        if (part.configure) part.configure(*object, *mesh);
        mesh->SetInstanceBlocks(part.source, {}, part.ranges, part.maximumLod);
        entry.objectId = scene.GetObjectId(object);
        // A re-created object must get every block that is still attached.
        entry.dirty = true;
    }
    const uint64_t id = m_nextBlock++;
    entry.blocks[id] = Block{std::move(block), false};
    auto token = std::make_shared<Token>();
    token->registry = std::static_pointer_cast<SharedPrefabParts>(scene.sharedPrefabParts);
    token->key = part.key;
    token->id = id;
    return token;
}

void SharedPrefabParts::Activate(const Handle& handle)
{
    const auto* token = static_cast<const Token*>(handle.get());
    if (!token) return;
    const auto registry = token->registry.lock();
    if (!registry) return;
    std::lock_guard lock(registry->m_mutex);
    const auto entry = registry->m_entries.find(token->key);
    if (entry == registry->m_entries.end()) return;
    const auto block = entry->second.blocks.find(token->id);
    if (block == entry->second.blocks.end() || block->second.active) return;
    block->second.active = true;
    entry->second.dirty = true;
}

SharedPrefabParts::Token::~Token()
{
    // Only the registry is touched: the scene may be gone. The mesh keeps the
    // block's placements alive until Flush replaces its list.
    const auto owner = registry.lock();
    if (!owner) return;
    std::lock_guard lock(owner->m_mutex);
    const auto entry = owner->m_entries.find(key);
    if (entry == owner->m_entries.end()) return;
    if (entry->second.blocks.erase(id)) entry->second.dirty = true;
}

void SharedPrefabParts::Flush(Engine::Scene::Scene& scene)
{
    if (auto registry = std::static_pointer_cast<SharedPrefabParts>(scene.sharedPrefabParts))
        registry->Apply(scene);
}

void SharedPrefabParts::Apply(Engine::Scene::Scene& scene)
{
    std::lock_guard lock(m_mutex);
    for (auto& [key, entry] : m_entries)
    {
        if (!entry.dirty) continue;
        entry.dirty = false;
        auto* object = entry.objectId ? scene.FindObjectById(entry.objectId) : nullptr;
        auto* mesh = object ? object->GetComponent<Mesh>() : nullptr;
        if (!mesh) continue;
        std::vector<Mesh::InstanceBlock> blocks;
        blocks.reserve(entry.blocks.size());
        for (const auto& [id, block] : entry.blocks)
            if (block.active) blocks.push_back(block.block);
        // An emptied part stays (disabled) for the next cell that needs it.
        object->enabled = !blocks.empty();
        mesh->SetInstanceBlocks(std::move(blocks));
    }
}

SharedPrefabParts::Statistics SharedPrefabParts::GetStatistics() const
{
    std::lock_guard lock(m_mutex);
    Statistics statistics;
    for (const auto& [key, entry] : m_entries)
    {
        if (entry.blocks.empty()) continue;
        ++statistics.objects;
        statistics.blocks += entry.blocks.size();
        for (const auto& [id, block] : entry.blocks)
            if (block.block.data) statistics.placements += block.block.data->transforms.size();
    }
    return statistics;
}
}
