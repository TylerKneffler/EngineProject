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
#include <vector>

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

bool IsHairOrEar(const std::string& name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* word : { "hair", "bang", "fringe", "ahoge", "ear",
        "pony", "髪", "耳", "毛" })
        if (lower.find(word) != std::string::npos) return true;
    return false;
}

void ApplyPmxPhysicsQuality(Engine::Model::ImportedModel& model,
    PmxImportOptions options, PmxImportReport& report)
{
    if (!options.dynamicPhysics ||
        options.physicsQuality == PmxPhysicsQuality::High) return;

    const size_t count = model.rigidBodies.size();
    std::vector<bool> protectedBody(count, false);
    for (const auto& morph : model.impulseMorphs)
        for (const auto& offset : morph.offsets)
            if (offset.bodyIndex >= 0 && static_cast<size_t>(offset.bodyIndex) < count)
                protectedBody[offset.bodyIndex] = true;
    for (const auto& soft : model.softBodies)
        for (const auto& anchor : soft.anchors)
            if (anchor.bodyIndex >= 0 && static_cast<size_t>(anchor.bodyIndex) < count)
                protectedBody[anchor.bodyIndex] = true;

    std::vector<bool> skinnedNode(model.nodes.size(), false);
    for (const auto& skin : model.skins)
        for (unsigned node : skin.jointNodes)
            if (node < skinnedNode.size()) skinnedNode[node] = true;
    // A surviving Bullet body and a spring must never drive the same bone.
    std::vector<bool> protectedNode(model.nodes.size(), false);
    for (size_t index = 0; index < count; ++index)
    {
        const auto& body = model.rigidBodies[index];
        if (protectedBody[index] && body.relatedBoneNode >= 0 &&
            static_cast<size_t>(body.relatedBoneNode) < protectedNode.size())
            protectedNode[body.relatedBoneNode] = true;
    }

    std::vector<bool> replaceBody(count, false);
    for (size_t index = 0; index < count; ++index)
    {
        const auto& body = model.rigidBodies[index];
        bool replace = body.mode != Engine::Model::RigidBodyMotion::FollowTransform &&
            body.relatedBoneNode >= 0 &&
            static_cast<size_t>(body.relatedBoneNode) < model.nodes.size() &&
            skinnedNode[body.relatedBoneNode] &&
            !protectedBody[index] && !protectedNode[body.relatedBoneNode];
        if (replace && options.physicsQuality == PmxPhysicsQuality::Medium)
            replace = IsHairOrEar(body.name) ||
                IsHairOrEar(model.nodes[body.relatedBoneNode].name);
        replaceBody[index] = replace;
        if (!replace && body.mode != Engine::Model::RigidBodyMotion::FollowTransform &&
            body.relatedBoneNode >= 0 &&
            static_cast<size_t>(body.relatedBoneNode) < protectedNode.size())
            protectedNode[body.relatedBoneNode] = true;
    }

    std::vector<int> remap(count, -1);
    std::vector<Engine::Model::ImportedRigidBody> retained;
    retained.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        const auto& body = model.rigidBodies[index];
        if (replaceBody[index] && !protectedNode[body.relatedBoneNode])
        {
            model.secondaryMotionNodes.push_back(
                static_cast<unsigned>(body.relatedBoneNode));
            continue;
        }
        remap[index] = static_cast<int>(retained.size());
        retained.push_back(body);
    }
    if (model.secondaryMotionNodes.empty()) return;
    std::sort(model.secondaryMotionNodes.begin(), model.secondaryMotionNodes.end());
    model.secondaryMotionNodes.erase(std::unique(model.secondaryMotionNodes.begin(),
        model.secondaryMotionNodes.end()), model.secondaryMotionNodes.end());
    model.rigidBodies = std::move(retained);

    std::vector<Engine::Model::ImportedPhysicsJoint> joints;
    joints.reserve(model.physicsJoints.size());
    for (auto joint : model.physicsJoints)
    {
        if ((joint.bodyA >= 0 && remap[joint.bodyA] < 0) ||
            (joint.bodyB >= 0 && remap[joint.bodyB] < 0)) continue;
        if (joint.bodyA >= 0) joint.bodyA = remap[joint.bodyA];
        if (joint.bodyB >= 0) joint.bodyB = remap[joint.bodyB];
        joints.push_back(std::move(joint));
    }
    model.physicsJoints = std::move(joints);
    for (auto& morph : model.impulseMorphs)
        for (auto& offset : morph.offsets)
            if (offset.bodyIndex >= 0) offset.bodyIndex = remap[offset.bodyIndex];
    for (auto& soft : model.softBodies)
        for (auto& anchor : soft.anchors)
            if (anchor.bodyIndex >= 0) anchor.bodyIndex = remap[anchor.bodyIndex];

    report.importedDynamicRigidBodies = static_cast<int>(std::count_if(
        model.rigidBodies.begin(), model.rigidBodies.end(), [](const auto& body)
        { return body.mode != Engine::Model::RigidBodyMotion::FollowTransform; }));
    report.importedPhysicsJoints = static_cast<int>(model.physicsJoints.size());
    report.springMotionBones = static_cast<int>(model.secondaryMotionNodes.size());
    report.warnings.push_back(std::to_string(report.springMotionBones) +
        " bones use pose-following spring motion without Bullet contacts");
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
        ApplyPmxPhysicsQuality(model, pmxOptions, report);
        model.unitScaleToMeters = pmxUnitScaleToMeters;
        auto result = ModelAssetWriter::Write(model, sourcePath, assetsDirectory,
            pmxOptions.weightFittedColliders);
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
                record->importSettings["pmxSpringMotionBones"] =
                    static_cast<double>(report.springMotionBones);
                record->importSettings["pmxPhysicsQuality"] =
                    !pmxOptions.dynamicPhysics ? "Disabled" :
                    pmxOptions.physicsQuality == PmxPhysicsQuality::High ? "High" :
                    pmxOptions.physicsQuality == PmxPhysicsQuality::Medium ? "Medium" : "Low";
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
                record->importSettings["pmxWeightFittedColliders"] =
                    pmxOptions.weightFittedColliders;
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
