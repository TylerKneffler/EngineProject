#pragma once

#include "Core/Model/AssetData.h"
#include <filesystem>
#include <optional>
#include <string>

// Persistent metadata stored beside an asset as <filename>.meta. The record
// owns the identity of the asset, so moving the asset and its sidecar preserves
// references independently of the asset's display path.
namespace Engine::Core
{
struct AssetRecord
{
    using AssetImportSettings = Engine::Model::AssetImportSettings;

    static constexpr int CurrentVersion = 1;

    int version = CurrentVersion;
    std::string id;
    std::string sourcePath;
    AssetImportSettings importSettings;

    static std::filesystem::path SidecarPath(const std::filesystem::path& assetPath);
    static std::optional<AssetRecord> Load(const std::filesystem::path& assetPath);
    static AssetRecord Ensure(const std::filesystem::path& assetPath,
        const std::filesystem::path& sourcePath,
        const AssetImportSettings& defaultSettings = {});
    static bool Move(const std::filesystem::path& oldAssetPath,
        const std::filesystem::path& newAssetPath, std::error_code& error);

    bool Save(const std::filesystem::path& assetPath) const;
};
}
