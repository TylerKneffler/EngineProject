#include "Editor/Core/Importers/FbxModelDecoder.h"
#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationRigMapping.h"
#include "Core/Model/RigAsset.h"
#include "Core/Compoonents/Animation/Model.h"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

int main()
{
    namespace fs = std::filesystem;
    const fs::path source = fs::path("Engine/Core/Assets/Models/crash-dummy/source") /
        "crush_dummy_UE4_skinned.fbx";
    if (!fs::is_regular_file(source))
    {
        std::cout << "Legacy FBX fixture is not present; skipping\n";
        return 0;
    }
    Engine::Model::ImportedModel model;
    std::string error;
    if (!Engine::Editor::FbxModelDecoder::Decode(source.string(), model, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    if (model.nodes.size() < 70 || model.primitives.empty() ||
        model.skins.empty() || model.primitives.front().vertices.empty())
    {
        std::cerr << "Legacy FBX lost its mesh or skeleton\n";
        return 1;
    }
    bool hasWeights = false;
    for (const auto& primitive : model.primitives)
        for (const auto& vertex : primitive.vertices)
            hasWeights |= vertex.weights0[0] > 0.f;
    if (!hasWeights || model.materials.empty() ||
        model.materials.front().baseColorTexture.empty())
    {
        std::cerr << "Legacy FBX lost weights or its nearby texture\n";
        return 1;
    }
    const fs::path output = fs::temp_directory_path() / "engine-legacy-fbx-test";
    fs::create_directories(output);
    const auto imported = Engine::Editor::ModelAssetWriter::Write(
        model, source.string(), output.string());
    if (!imported.success || !fs::is_regular_file(imported.prefabPath))
    {
        std::cerr << "Legacy FBX asset writing failed: " << imported.message << '\n';
        return 1;
    }
    std::ifstream material(fs::path(imported.outputDirectory) /
        "Materials/default 1.material");
    const std::string content(std::istreambuf_iterator<char>{material}, {});
    if (content.find("crush_dummy_default_BaseColor.tga.png") == std::string::npos)
    {
        std::cerr << "Generated material lost the resolved texture\n";
        return 1;
    }
    Engine::Scene::Scene scene;
    auto* target = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        scene, imported.prefabPath, nullptr);
    if (!target)
    {
        std::cerr << "Generated legacy prefab could not be instantiated\n";
        return 1;
    }
    const fs::path walking = fs::path("Engine/Core/Assets/Models/Mixemo") /
        "Walking.fbx";
    if (fs::is_regular_file(walking))
    {
        Engine::Model::ImportedModel walkingModel;
        if (!Engine::Editor::FbxModelDecoder::Decode(
                walking.string(), walkingModel, error) ||
            walkingModel.skins.size() != 1 || !walkingModel.primitives.empty())
        {
            std::cerr << "Animation-only FBX lost its pose-only rig: " << error << '\n';
            return 1;
        }
        const auto extracted = Engine::Editor::ModelAssetWriter::Write(
            walkingModel, walking.string(), output.string());
        if (!extracted.success)
        { std::cerr << extracted.message << '\n'; return 1; }
        std::optional<Engine::Model::AnimationClipAsset> motion;
        for (const auto& entry : fs::directory_iterator(extracted.outputDirectory))
            if (entry.path().extension() == ".anim")
                motion = Engine::Model::AnimationClipAsset::Load(
                    entry.path().string());
        const auto targetRig = Engine::Model::RigAsset::Load(
            (fs::path(imported.outputDirectory) /
                (fs::path(imported.outputDirectory).filename().string() + ".rig")).string());
        auto* targetModel = target->GetComponent<Engine::Components::Model>();
        if (!motion || !targetRig || !targetModel ||
            !fs::is_regular_file(fs::path(extracted.outputDirectory) /
                (fs::path(extracted.outputDirectory).filename().string() + "_Rest.pose")))
        { std::cerr << "Extracted motion assets are incomplete\n"; return 1; }
        const auto mapping = Engine::Model::AnimationRigMapping::Map(
            *motion, *targetModel, *targetRig);
        if (mapping.unmatchedChannels != 0 || mapping.inferredChannels == 0)
        {
            std::cerr << "Mixamo motion did not map to Crash Dummy: " <<
                mapping.unmatchedChannels << " unmatched channels\n";
            for (const auto& path : mapping.unmatchedPaths)
                std::cerr << "  " << path << '\n';
            return 1;
        }
        const fs::path mococoPrefab = fs::path("Engine/Core/Assets/Models/mmd") /
            "Mococo Abyssgard/Mococo Abyssgard.prefab";
        if (fs::is_regular_file(mococoPrefab))
        {
            Engine::Scene::Scene mococoScene;
            auto* mococo = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                mococoScene, mococoPrefab.string(), nullptr);
            const auto mococoRig = Engine::Model::RigAsset::Load(
                (mococoPrefab.parent_path() / "Mococo Abyssgard.rig").string());
            auto* mococoModel = mococo
                ? mococo->GetComponent<Engine::Components::Model>() : nullptr;
            if (!mococoRig || !mococoModel)
            { std::cerr << "Mococo rig could not be loaded\n"; return 1; }
            const auto mococoMapping = Engine::Model::AnimationRigMapping::Map(
                *motion, *mococoModel, *mococoRig);
            if (mococoMapping.unmatchedChannels > 3 ||
                mococoMapping.clip.channels.size() != motion->clip.channels.size() ||
                std::count_if(mococoMapping.clip.channels.begin(),
                    mococoMapping.clip.channels.end(), [](const auto& channel)
                    { return channel.nodeIndex ==
                        std::numeric_limits<unsigned>::max(); }) !=
                    static_cast<std::ptrdiff_t>(mococoMapping.unmatchedChannels) ||
                mococoMapping.inferredChannels == 0 ||
                std::any_of(mococoMapping.unmatchedPaths.begin(),
                    mococoMapping.unmatchedPaths.end(),
                    [](const std::string& path)
                    { return Engine::Model::AnimationRigMapping::Role(
                        Engine::Model::AnimationRigMapping::Leaf(path)) != "spine3"; }))
            {
                std::cerr << "Mococo bone translation changed unexpectedly: " <<
                    mococoMapping.unmatchedChannels << " channels unmatched\n";
                for (const auto& path : mococoMapping.unmatchedPaths)
                    std::cerr << "  " << Engine::Model::AnimationRigMapping::Leaf(path) << '\n';
                return 1;
            }
        }
    }
    std::cout << "Imported " << model.primitives.size() << " meshes and "
        << model.skins.size() << " skins\n";
    return 0;
}
