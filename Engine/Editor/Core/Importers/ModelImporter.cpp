#include "ModelImporter.h"

#include "FbxModelDecoder.h"
#include "GltfImporter.h"
#include "ModelAssetWriter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace
{
std::string LowerExtension(const std::string& value)
{
    std::string result = std::filesystem::path(value).extension().string();
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
}

bool ModelImporter::SupportsExtension(const std::string& extension)
{
    std::string normalized = extension;
    if (normalized.empty() || normalized.front() != '.') normalized.insert(normalized.begin(), '.');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized == ".gltf" || normalized == ".glb" || normalized == ".fbx";
}

ModelImportResult ModelImporter::Import(const std::string& sourcePath,
    const std::string& assetsDirectory)
{
    const std::string extension = LowerExtension(sourcePath);
    if (extension == ".gltf" || extension == ".glb")
    {
        return GltfImporter::Import(sourcePath, assetsDirectory);
    }
    if (extension == ".fbx")
    {
        ImportedModel model;
        std::string error;
        if (!FbxModelDecoder::Decode(sourcePath, model, error))
            return { false, {}, {}, error.empty() ? "Could not decode FBX" : error };
        return ModelAssetWriter::Write(model, sourcePath, assetsDirectory);
    }
    return { false, {}, {}, "Unsupported model format: " + extension };
}
