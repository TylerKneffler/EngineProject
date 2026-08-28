#include "Core/Physics/Cloth/ClothSimulationMeshCache.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Memory/CacheStore.h"
#include <filesystem>

namespace Engine::Physics
{
struct CachedClothSimulationMesh
{
    std::filesystem::file_time_type writeTime{};
    uintmax_t fileSize = 0;
    std::shared_ptr<Engine::Components::Mesh> mesh;
};

std::shared_ptr<Engine::Components::Mesh> ClothSimulationMeshCache::Acquire(
    const std::string& path)
{
    constexpr const char* domain = "Physics.ClothSimulationMesh";
    const std::string resolvedPath =
        Engine::Components::Mesh::ResolveFilePath(path);
    const std::string key = Engine::Memory::CacheStore::PathKey(resolvedPath);
    std::error_code timeError;
    const auto writeTime =
        std::filesystem::last_write_time(resolvedPath, timeError);
    std::error_code sizeError;
    const uintmax_t fileSize =
        std::filesystem::file_size(resolvedPath, sizeError);
    if (timeError || sizeError)
        return nullptr;

    Engine::Memory::CacheStore& cache = Engine::Memory::CacheStore::Get();
    std::shared_ptr<CachedClothSimulationMesh> cached =
        cache.Find<CachedClothSimulationMesh>(domain, key);
    if (cached && cached->mesh && cached->writeTime == writeTime &&
        cached->fileSize == fileSize)
        return cached->mesh;

    cache.Erase(domain, key);
    cached = cache.GetOrCreate<CachedClothSimulationMesh>(
        Engine::Memory::CacheLifetime::LongTerm, domain, key,
        [&]() -> std::shared_ptr<CachedClothSimulationMesh>
        {
            auto loaded = std::make_shared<Engine::Components::Mesh>();
            try
            {
                loaded->LoadFromFile(path);
            }
            catch (...)
            {
                return nullptr;
            }
            if (loaded->GetVertices().size() < 3)
                return nullptr;
            auto entry = std::make_shared<CachedClothSimulationMesh>();
            entry->writeTime = writeTime;
            entry->fileSize = fileSize;
            entry->mesh = std::move(loaded);
            return entry;
        });
    return cached ? cached->mesh : nullptr;
}
}

