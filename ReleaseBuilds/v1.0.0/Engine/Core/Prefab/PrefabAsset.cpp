#include "Core/Prefab/PrefabAsset.h"
#include "Core/Memory/CacheStore.h"

#include <filesystem>

namespace Engine::Prefab
{
std::shared_ptr<const PrefabAsset> PrefabAsset::Acquire(const std::string& path)
{
    const std::filesystem::path normalized =
        std::filesystem::path(path).lexically_normal();
    const std::string key = Engine::Memory::CacheStore::PathKey(path);
    return Engine::Memory::CacheStore::Get().GetOrCreate<PrefabAsset>(
        Engine::Memory::CacheLifetime::LongTerm, "Prefab", key,
        [&]()
        {
            return std::shared_ptr<PrefabAsset>(
                new PrefabAsset(normalized.generic_string()));
        });
}
}
