#include "AssetDataExtractor.h"
#include "ModelImporter.h"
#include "Core/AssetRecord.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Model/MeshPackAsset.h"
#include "Core/Model/PoseAsset.h"
#include "Core/Model/RigAsset.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace Engine::Editor
{
namespace fs = std::filesystem;
namespace
{
std::string Extension(const fs::path& path)
{
    auto value = path.extension().string();
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::string Name(std::string value)
{
    for (char& c : value)
        if (static_cast<unsigned char>(c) < 32 ||
            std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) value.pop_back();
    return value.empty() ? "Asset" : value;
}
}

bool AssetDataExtractor::Supports(const fs::path& path)
{
    const auto extension = Extension(path);
    return ModelImporter::SupportsExtension(extension) || extension == ".rig" ||
        extension == ".animpack" || extension == ".meshpack";
}

Engine::Model::ModelImportResult AssetDataExtractor::Extract(
    const fs::path& source, const fs::path& destination)
{
    Engine::Model::ModelImportResult result;
    fs::path output;
    try
    {
        if (!Supports(source) || !fs::is_regular_file(source))
            throw std::runtime_error("Select a supported model, rig, animation pack, or mesh pack.");
        const auto extension = Extension(source);
        if (ModelImporter::SupportsExtension(extension))
            return ModelImporter::Import(source.string(), destination.string());
        const auto base = Name(source.stem().string()) + " Assets";
        output = destination / base;
        for (unsigned suffix = 2; fs::exists(output); ++suffix)
            output = destination / (base + " " + std::to_string(suffix));
        fs::create_directories(output);
        const auto record = [&](const fs::path& path)
        {
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("package-extraction") } });
        };
        const auto writeClip = [&](const Engine::Model::AnimationClipAsset& asset, size_t index)
        {
            const auto path = output / (Name(asset.clip.clipName) + "_" +
                std::to_string(index + 1) + ".anim");
            auto standalone = asset;
            standalone.previewPrefab.clear();
            if (!standalone.Save(path.string()))
                throw std::runtime_error("Could not save animation: " + path.string());
            record(path);
        };
        std::string error;
        if (extension == ".meshpack")
        {
            if (!Engine::Model::MeshPackAsset::Extract(source, output, error))
                throw std::runtime_error(error);
            std::vector<fs::path> extractedFiles;
            for (const auto& entry : fs::recursive_directory_iterator(output))
                if (entry.is_regular_file()) extractedFiles.push_back(entry.path());
            for (const auto& path : extractedFiles) record(path);
        }
        else if (extension == ".animpack")
        {
            const auto pack = Engine::Model::AnimationPackAsset::Load(source.string(), error);
            if (!pack) throw std::runtime_error(error);
            for (size_t i = 0; i < pack->clips.size(); ++i)
            {
                Engine::Model::AnimationClipAsset asset;
                asset.clip = pack->clips[i];
                asset.bindSkeleton = pack->bindSkeleton;
                asset.baseSkeleton = pack->bindSkeleton;
                asset.humanoid = pack->humanoid;
                writeClip(asset, i);
            }
            for (size_t i = 0; i < pack->sourceTracks.size(); ++i)
            {
                const auto& track = pack->sourceTracks[i];
                const auto path = output / (Name(track.kind) + "_" + std::to_string(i + 1) + ".vmdtrack");
                if (!pack->ExtractSourceTrack(track.kind, path.string(), error))
                    throw std::runtime_error(error);
                record(path);
            }
        }
        else if (extension == ".rig")
        {
            const auto rigs = Engine::Model::RigAsset::LoadAll(source.string());
            if (rigs.empty()) throw std::runtime_error("Could not read rig package.");
            std::set<std::string> animations;
            for (size_t i = 0; i < rigs.size(); ++i)
            {
                auto skeleton = rigs[i];
                if (skeleton.bindNodes.empty())
                    throw std::runtime_error("Rig has no standalone hierarchy. Split its source model again.");
                skeleton.prefabPath.clear();
                skeleton.animationPaths.clear();
                skeleton.meshNames.clear();
                skeleton.poses.clear();
                const auto path = output / (Name(source.stem().string()) + "_" + std::to_string(i + 1) + ".skeleton");
                if (!skeleton.Save(path.string())) throw std::runtime_error("Could not save skeleton.");
                record(path);
                for (size_t p = 0; p < rigs[i].poses.size(); ++p)
                {
                    Engine::Model::PoseAsset pose;
                    pose.name = rigs[i].poses[p].name;
                    for (const auto& joint : rigs[i].poses[p].joints)
                    {
                        if (joint.nodeIndex >= rigs[i].bindNodes.size())
                            throw std::runtime_error("Rig pose contains an invalid node.");
                        pose.joints.push_back({ rigs[i].bindNodes[joint.nodeIndex].path,
                            joint.position, joint.rotation, joint.scale });
                    }
                    const auto posePath = output / (Name(pose.name) + "_" +
                        std::to_string(i + 1) + "_" + std::to_string(p + 1) + ".pose");
                    if (!pose.Save(posePath.string())) throw std::runtime_error("Could not save pose.");
                    record(posePath);
                }
                for (const auto& animation : rigs[i].animationPaths)
                    if (animations.insert(animation).second)
                    {
                        const auto asset = Engine::Model::AnimationClipAsset::Load(animation);
                        if (!asset) throw std::runtime_error("Missing animation: " + animation);
                        writeClip(*asset, animations.size() - 1);
                    }
            }
            auto meshPack = source;
            meshPack.replace_extension(".meshpack");
            if (fs::exists(meshPack) && !Engine::Model::MeshPackAsset::Extract(meshPack, output, error))
                throw std::runtime_error(error);
        }
        result.success = true;
        result.outputDirectory = output.generic_string();
        result.message = "Split " + source.filename().string();
    }
    catch (const std::exception& error)
    {
        if (!output.empty())
        {
            std::error_code ignored;
            fs::remove_all(output, ignored);
        }
        result.message = error.what();
    }
    return result;
}
}
