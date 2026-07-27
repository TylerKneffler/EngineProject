#include "PrefabAsset.h"

#include <filesystem>
#include <cctype>
#include <unordered_map>

std::shared_ptr<const PrefabAsset> PrefabAsset::Acquire(const std::string& path)
{
    static std::unordered_map<std::string, std::weak_ptr<const PrefabAsset>> registry;

    const std::filesystem::path normalized =
        std::filesystem::path(path).lexically_normal();
    std::error_code absoluteError;
    std::filesystem::path identity = std::filesystem::absolute(normalized, absoluteError);
    if (absoluteError)
        identity = normalized;
    std::string key = identity.lexically_normal().generic_string();
#ifdef _WIN32
    for (char& c : key)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif

    auto found = registry.find(key);
    if (found != registry.end())
        if (auto existing = found->second.lock())
            return existing;

    auto asset = std::shared_ptr<const PrefabAsset>(new PrefabAsset(normalized.generic_string()));
    registry[key] = asset;
    return asset;
}
