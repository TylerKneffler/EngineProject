#include "Editor/Core/Importers/FbxModelDecoder.h"
#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include <ufbx.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <memory>
#include <stdexcept>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

int main(int argc, char** argv)
{
    try
    {
    const std::string source = "Engine/Core/Assets/Models/mmd/Roku/Roku_V1_packed.fbx";
    ufbx_error loadError{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_file(source.c_str(), nullptr, &loadError), ufbx_free_scene);
    CHECK(scene);
    Engine::Model::ImportedModel model;
    std::string error;
    if (!Engine::Editor::FbxModelDecoder::Decode(source, model, error))
    { std::cerr << error << '\n'; return 1; }
    std::cout << "Decoded " << model.nodes.size() << " nodes, " << model.primitives.size()
        << " meshes, " << model.animations.size() << " animations\n";
    CHECK(model.primitives.size() == 31 && !model.skins.empty());
    for (const auto& material : model.materials) CHECK(!material.useVertexColors);
    CHECK(std::any_of(model.skins.begin(), model.skins.end(),
        [](const auto& skin) { return skin.jointNodes.size() > 256; }));
    if (argc > 1 && std::string(argv[1]) == "--inspect")
    {
        std::cout << "Embedded textures: " << model.textures.size() << '\n';
        std::vector<std::vector<unsigned>> usedBySkin(model.skins.size());
        for (const auto* texture : scene->textures)
            std::cout << "Texture " << texture->filename.data << " embedded " << texture->content.size << '\n';
        for (const auto& primitive : model.primitives)
        {
            std::vector<unsigned> used;
            for (const auto& vertex : primitive.vertices)
                for (int slot = 0; slot < 8; ++slot)
                    if ((slot < 4 ? vertex.weights0[slot] : vertex.weights1[slot-4]) > 0.f)
                        used.push_back(unsigned(slot < 4 ? vertex.joints0[slot] : vertex.joints1[slot-4]));
            std::sort(used.begin(), used.end()); used.erase(std::unique(used.begin(), used.end()), used.end());
            std::cout << primitive.name << " skin " << primitive.skinIndex << " used " << used.size() << '\n';
            if (primitive.skinIndex >= 0) usedBySkin[primitive.skinIndex].insert(usedBySkin[primitive.skinIndex].end(), used.begin(), used.end());
        }
        for (auto& used : usedBySkin)
        {
            std::sort(used.begin(), used.end()); used.erase(std::unique(used.begin(), used.end()), used.end());
            std::cout << "Union used joints: " << used.size() << '\n';
        }
        return 0;
    }
    const auto* stack = ufbx_find_anim_stack(scene.get(), "Base_Layer");
    CHECK(stack);
    unsigned verifiedBones = 0;
    for (unsigned index = 0; index < model.nodes.size(); ++index)
    {
        const auto& node = model.nodes[index];
        if (node.name != "Bone") continue;
        CHECK(node.parent >= 0);
        const auto& parent = model.nodes[node.parent].name;
        const ufbx_node* original = nullptr;
        for (const auto* candidate : scene->nodes)
            if (std::string(candidate->name.data, candidate->name.length) == "Bone" &&
                candidate->parent && parent == candidate->parent->name.data)
                original = candidate;
        CHECK(original);
        unsigned channels = 0;
        for (const auto& animation : model.animations)
        {
            if (animation.name != "Base_Layer") continue;
            for (const auto& channel : animation.channels)
            {
                if (channel.nodeIndex != index) continue;
                ++channels;
                CHECK(!channel.times.empty());
                for (size_t key = 0; key < channel.times.size(); ++key)
                {
                    const auto expected = ufbx_evaluate_transform(stack->anim, original,
                        stack->time_begin + channel.times[key]);
                    const float* value = channel.values.data() + key * channel.valueWidth;
                    if (channel.path == Engine::Model::AnimationChannel::Path::Rotation)
                    {
                        const glm::quat actual(value[3], value[0], value[1], value[2]);
                        const glm::quat rotation(static_cast<float>(expected.rotation.w),
                            static_cast<float>(expected.rotation.x), static_cast<float>(expected.rotation.y),
                            static_cast<float>(expected.rotation.z));
                        CHECK(std::abs(glm::dot(actual, rotation)) > .9999f);
                    }
                    else
                    {
                        const auto v = channel.path == Engine::Model::AnimationChannel::Path::Translation
                            ? expected.translation : expected.scale;
                        CHECK(glm::length(glm::vec3(value[0], value[1], value[2]) -
                            glm::vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z))) < .001f);
                    }
                }
            }
        }
        CHECK(channels == 3);
        ++verifiedBones;
    }
    CHECK(verifiedBones == 2);
    const auto output = std::filesystem::absolute(std::filesystem::path(".local/test-output") /
        ("roku-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    const auto result = Engine::Editor::ModelAssetWriter::Write(model, source, output.string());
    if (!result.success) throw std::runtime_error(result.message);
    CHECK(result.message.find("missing source texture files") != std::string::npos);
    Engine::Scene::Scene importedScene;
    auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(importedScene, result.prefabPath, nullptr);
    CHECK(root);
    auto* loadedModel = root->GetComponent<Engine::Components::Model>();
    auto* playback = root->GetComponent<Engine::Components::AnimationManager>();
    CHECK(loadedModel && playback && !playback->clips.empty());
    size_t completeLargePalettes = 0;
    for (const auto& object : importedScene.GetObjects())
        if (auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>())
            if (skin->BuildPalette().size() > 256) ++completeLargePalettes;
    CHECK(completeLargePalettes > 0);
    for (unsigned index = 0; index < model.nodes.size(); ++index)
        if (model.nodes[index].name == "Bone")
        {
            auto* bone = loadedModel->ResolveNode(index);
            CHECK(bone && bone->Parent);
            CHECK(bone->Parent->name == model.nodes[model.nodes[index].parent].name);
        }
    std::cout << "Both braid animations match source keys; Roku prefab imports into a scene\n";
    return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
