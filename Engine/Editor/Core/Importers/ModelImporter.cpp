#include "ModelImporter.h"

#include "FbxModelDecoder.h"
#include "GltfImporter.h"
#include "ModelAssetWriter.h"
#include "PmxModelDecoder.h"
#include "Core/AssetRecord.h"
#include "VmdSceneImporter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

namespace Engine::Editor
{
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
    return normalized == ".gltf" || normalized == ".glb" || normalized == ".fbx" ||
        normalized == ".pmx" || normalized == ".vmd";
}

Engine::Model::ModelImportResult ModelImporter::Import(const std::string& sourcePath,
    const std::string& assetsDirectory, float pmxUnitScaleToMeters,
    PmxImportOptions pmxOptions)
{
    const std::string extension = LowerExtension(sourcePath);
    if (extension == ".vmd")
        return VmdSceneImporter::Import(sourcePath, assetsDirectory);
    if (extension == ".gltf" || extension == ".glb")
    {
        return GltfImporter::Import(sourcePath, assetsDirectory);
    }
    if (extension == ".fbx")
    {
        Engine::Model::ImportedModel model;
        std::string error;
        if (!FbxModelDecoder::Decode(sourcePath, model, error))
            return { false, {}, {}, error.empty() ? "Could not decode FBX" : error };
        return ModelAssetWriter::Write(model, sourcePath, assetsDirectory);
    }
    if (extension == ".pmx")
    {
        if (!std::isfinite(pmxUnitScaleToMeters) ||
            pmxUnitScaleToMeters <= 0.f || pmxUnitScaleToMeters > 1000.f)
            return { false, {}, {}, "PMX unit scale must be greater than zero and at most 1000" };
        Engine::Model::ImportedModel model;
        std::string error;
        PmxImportReport report;
        if (!PmxModelDecoder::Decode(sourcePath, model, error, &report, pmxOptions))
            return { false, {}, {}, error.empty() ? "Could not decode PMX" : error };
        model.unitScaleToMeters = pmxUnitScaleToMeters;
        auto result = ModelAssetWriter::Write(model, sourcePath, assetsDirectory);
        if (result.success)
        {
            result.message.clear();
            for (const auto& warning : report.warnings)
            {
                if (!result.message.empty()) result.message += "; ";
                result.message += warning;
            }
            if (auto record = Engine::Core::AssetRecord::Load(result.prefabPath))
            {
                record->importSettings["pmxImportedGroups"] =
                    static_cast<double>(report.importedSupportedGroupMorphs);
                record->importSettings["pmxKinematicRigidBodies"] =
                    static_cast<double>(report.importedKinematicRigidBodies);
                record->importSettings["pmxDynamicRigidBodies"] =
                    static_cast<double>(report.importedDynamicRigidBodies);
                record->importSettings["pmxPhysicsJoints"] =
                    static_cast<double>(report.importedPhysicsJoints);
                record->importSettings["pmxSoftBodies"] =
                    static_cast<double>(report.importedSoftBodies);
                record->importSettings["pmxAppendRules"] =
                    static_cast<double>(report.importedAppendRules);
                record->importSettings["pmxIkChains"] =
                    static_cast<double>(report.importedIkChains);
                record->importSettings["pmxBoneMorphs"] =
                    static_cast<double>(report.importedBoneMorphs);
                record->importSettings["pmxMaterialMorphs"] =
                    static_cast<double>(report.importedMaterialMorphs);
                record->importSettings["pmxUnitScaleToMeters"] =
                    static_cast<double>(pmxUnitScaleToMeters);
                record->importSettings["pmxImportMorphs"] = pmxOptions.morphs;
                record->importSettings["pmxImportRigConstraints"] =
                    pmxOptions.rigConstraints;
                record->importSettings["pmxImportFollowBoneColliders"] =
                    pmxOptions.followBoneColliders;
                record->importSettings["pmxImportDynamicPhysics"] =
                    pmxOptions.dynamicPhysics;
                record->importSettings["pmxWarnings"] = result.message;
                if (!record->Save(result.prefabPath))
                    result.message += "; could not save PMX import report";
            }
        }
        return result;
    }
    return { false, {}, {}, "Unsupported model format: " + extension };
}
}
