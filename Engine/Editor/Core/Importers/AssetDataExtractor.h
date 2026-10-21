#pragma once

#include "Core/Model/ImportedModel.h"
#include <filesystem>

namespace Engine::Editor
{
// Exports reusable engine data without changing the source package.
class AssetDataExtractor
{
public:
    static bool Supports(const std::filesystem::path& path);
    static Engine::Model::ModelImportResult Extract(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);
};
}
