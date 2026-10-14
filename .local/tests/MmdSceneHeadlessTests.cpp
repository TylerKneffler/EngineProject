#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Assets/Scripts/Animation/MmdDanceReplay.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Editor/Core/Importers/FbxModelDecoder.h"
#include "Editor/Core/Importers/MmdPoseRetargeter.h"
#include "Editor/Core/Importers/MmdDanceAssetBinder.h"
#include "Editor/Core/Importers/VmdMotionDecoder.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")
#endif

namespace fs = std::filesystem;
using Engine::Components::AnimationManager;
using Engine::Components::Mesh;
using Engine::Components::Model;
using Engine::Editor::MmdPoseRetargeter;
using Engine::Editor::VmdMotion;

namespace
{
void Check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void Memory(const char* stage)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof(info);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info)))
        std::cout << "Memory " << stage << ": current " <<
            info.WorkingSetSize / (1024 * 1024) << " MiB, peak " <<
            info.PeakWorkingSetSize / (1024 * 1024) << " MiB, private " <<
            info.PrivateUsage / (1024 * 1024) << " MiB\n";
#else
    (void)stage;
#endif
}

float SampleWeight(const Engine::Model::AnimationChannel& channel, float time,
    size_t target)
{
    const auto next = std::lower_bound(channel.times.begin(),
        channel.times.end(), time);
    const size_t after = next == channel.times.end()
        ? channel.times.size() - 1 : static_cast<size_t>(next - channel.times.begin());
    const size_t before = after == 0 ? 0 : after - 1;
    const float span = channel.times[after] - channel.times[before];
    const float amount = span > 0.f
        ? std::clamp((time - channel.times[before]) / span, 0.f, 1.f) : 0.f;
    const size_t width = channel.valueWidth;
    return channel.values[before * width + target] * (1.f - amount) +
        channel.values[after * width + target] * amount;
}

void FindMeshes(Engine::Core::Object* object, const std::string& path,
    std::unordered_map<std::string, Mesh*>& meshes)
{
    if (auto* mesh = object->GetComponent<Mesh>()) meshes.emplace(path, mesh);
    for (auto* child : object->Children)
        FindMeshes(child, path.empty() ? child->name : path + "/" + child->name,
            meshes);
}
}

int main(int argc, char** argv)
{
    try
    {
        const fs::path root = MMD_TEST_ASSET_ROOT;
        const fs::path scenePath = root.parent_path() /
            "Scenes/Animation/mmd_fbx.scene";
        const fs::path fbxPath = root / "Mococo/Mococo (FBX No Outline).fbx";
        const fs::path profilePath = root / "Mococo/Mococo.mmdrig.json";
        const fs::path mapPath = root / "Mococo/Mococo.mmdmap.json";
        const fs::path vmdPath = root / "dance1.vmd";
        const fs::path packPath = root / "Mococo Dance/dance1.animpack";
        const bool repairAssets = argc == 2 && std::string(argv[1]) == "--repair-assets";
        Memory("baseline");
        Engine::Scene::Scene scene;
        Check(Engine::Serialization::SceneSerializer::Load(scene,
            scenePath.string()), "Saved MMD scene did not load headlessly");
        Engine::Core::Object* character = nullptr;
        for (const auto& object : scene.GetObjects())
            if (object->GetComponent<Model>())
            { Check(!character, "Scene has multiple model roots"); character = object.get(); }
        Check(character, "MMD character is absent from saved scene");
        auto* model = character->GetComponent<Model>();
        auto* manager = character->GetComponent<AnimationManager>();
        Check(manager, "Saved character has no AnimationManager");
        scene.Start();
        const auto* body = manager->FindClip("dance1 body");
        const auto* face = manager->FindClip("dance1 facial");
        Check(body && face && manager->clip == body->clipName,
            "Scene did not bind the imported body and facial assets");
        Check(manager->layers.size() == 1 && manager->layers[0].clip == face->clipName &&
            manager->layers[0].enabled, "Facial layer is missing or disabled");
        Memory("scene load and start");

        VmdMotion raw;
        std::string error;
        Check(Engine::Editor::VmdMotionDecoder::DecodeFile(
            vmdPath.string(), raw, error), error);
        Check(raw.cameras.empty() && raw.lights.empty(),
            "Dance VMD unexpectedly contains camera or light keys");
        Engine::Model::ImportedModel imported;
        Check(Engine::Editor::FbxModelDecoder::Decode(
            fbxPath.string(), imported, error), error);
        MmdPoseRetargeter retargeter;
        Check(retargeter.Initialize(imported,
            Engine::Serialization::JsonParseFile(profilePath.string()),
            Engine::Serialization::JsonParseFile(mapPath.string()), raw, error),
            error);
        const auto rawFace = retargeter.BakeFacial(raw.lastFrame);
        if (repairAssets)
        {
            const fs::path prefabPath = root / "Mococo Dance/Mococo Dance.prefab";
            const fs::path bodyPath = root / "Mococo Dance/dance1_body.anim";
            for (size_t primitiveIndex = 0;
                primitiveIndex < imported.primitives.size(); ++primitiveIndex)
            {
                const auto& primitive = imported.primitives[primitiveIndex];
                if (primitive.morphTargets.empty()) continue;
                const fs::path morphPath = root / "Mococo Dance/Meshes" /
                    (primitive.name + " " + std::to_string(primitiveIndex + 1) +
                        ".morph");
                Check(fs::is_regular_file(morphPath),
                    "Missing generated morph asset: " + morphPath.string());
                Check(Mesh::SaveMorphFile(morphPath.string(),
                    primitive.morphTargets),
                    "Could not refresh sorted morph asset: " + morphPath.string());
            }
            const auto bodyAsset = Engine::Model::AnimationClipAsset::Load(bodyPath.string());
            Check(bodyAsset.has_value(), "Could not load saved body clip for repair");
            Engine::Scene::Scene prefabScene;
            auto* prefabRoot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                prefabScene, prefabPath.string());
            Check(prefabRoot, "Could not load character prefab for repair");
            std::unordered_map<std::string, Mesh*> prefabMeshes;
            FindMeshes(prefabRoot, "", prefabMeshes);
            for (const auto& [path, mesh] : prefabMeshes)
                if (mesh->HasMorphTargets())
                    mesh->SetMorphWeights(std::vector<float>(
                        mesh->GetMorphTargets().size(), 0.f));
            Check(Engine::Serialization::SceneSerializer::SavePrefab(
                *prefabRoot, prefabPath.string()), "Could not save neutral morph defaults");
            const bool rebound = Engine::Editor::MmdDanceAssetBinder::BindBaked(
                prefabPath.string(), vmdPath.string(), profilePath.string(),
                mapPath.string(), bodyAsset->clip, rawFace, error,
                packPath.string());
            Check(rebound, "Could not refresh MMD animation assets: " + error);
            const auto refreshedFace = Engine::Model::AnimationClipAsset::Load(
                (root / "Mococo Dance/dance1_facial.anim").string());
            Check(refreshedFace && refreshedFace->clip.channels.size() ==
                rawFace.clip.channels.size(), "Refreshed facial asset did not reload");
            for (size_t i = 0; i < rawFace.clip.channels.size(); ++i)
                Check(refreshedFace->clip.channels[i].values ==
                    rawFace.clip.channels[i].values,
                    "Facial asset round-trip changed raw VMD weights in channel " +
                        std::to_string(i));
            std::cout << "Refreshed character prefab and facial clip from neutral FBX defaults.\n";
            return 0;
        }
        const auto& nodes = model->ResolveNodes();
        auto* sharedSkeleton = character->GetComponent<Engine::Components::Skeleton>();
        size_t skeletonCount = 0;
        for (auto* component : character->Components)
            skeletonCount += dynamic_cast<Engine::Components::Skeleton*>(component) != nullptr;
        Check(skeletonCount == 1 && sharedSkeleton &&
            imported.skins.size() == 1 && sharedSkeleton->jointNodes == imported.skins[0].jointNodes,
            "Mococo does not use one shared skeleton");
        Check(Engine::Model::RigAsset::LoadAll(sharedSkeleton->rigPath).size() == 1,
            "Mococo rig asset still contains duplicate skeletons");
        for (unsigned node : sharedSkeleton->jointNodes)
        {
            size_t bones = 0;
            for (auto* component : nodes.at(node)->Components)
                bones += dynamic_cast<Engine::Components::AnimationBone*>(component) != nullptr;
            Check(bones == 1, "A Mococo joint has duplicate bone components");
        }
        Check(nodes.size() == imported.nodes.size(),
            "Saved model node count differs from source FBX");
        size_t checkedMeshes = 0;
        for (size_t nodeIndex = 0; nodeIndex < imported.nodes.size(); ++nodeIndex)
        {
            const auto& primitiveIndices = imported.nodes[nodeIndex].primitives;
            for (size_t slot = 0; slot < primitiveIndices.size(); ++slot)
            {
                const auto& primitive = imported.primitives[primitiveIndices[slot]];
                auto* object = primitiveIndices.size() == 1 ? nodes[nodeIndex] : nodes[nodeIndex]->Children.at(slot);
                auto* mesh = object->GetComponent<Mesh>();
                auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>();
                Check(skin && skin->ResolveSkeleton() == sharedSkeleton,
                    "Mococo mesh does not resolve its shared skeleton");
                Check(mesh && mesh->GetVertices().size() == primitive.vertices.size(), "Saved mesh geometry is incomplete");
                const auto& vertices = mesh->GetVertices();
                for (size_t index = 0; index < vertices.size(); ++index)
                    for (int axis = 0; axis < 2; ++axis)
                        Check(vertices[index].uv[axis] == primitive.vertices[index].uv[axis] &&
                            vertices[index].uv1[axis] == primitive.vertices[index].uv1[axis] &&
                            vertices[index].tangent[3] == primitive.vertices[index].tangent[3],
                            "Saved mesh has stale texture coordinates: " + object->name);
                for (size_t index = 0; index < vertices.size(); ++index)
                    for (int influence = 0; influence < 4; ++influence)
                        Check(vertices[index].joints0[influence] == primitive.vertices[index].joints0[influence] &&
                            vertices[index].joints1[influence] == primitive.vertices[index].joints1[influence] &&
                            vertices[index].weights0[influence] == primitive.vertices[index].weights0[influence] &&
                            vertices[index].weights1[influence] == primitive.vertices[index].weights1[influence],
                            "Saved mesh uses an obsolete joint palette: " + object->name);
                auto* material = object->GetComponent<Engine::Components::Material>();
                const auto& expected = imported.materials.at(primitive.materialIndex);
                Check(material && material->baseColorTexture &&
                    fs::weakly_canonical(material->baseColorTexture->GetFilePath()) ==
                        fs::weakly_canonical(fbxPath.parent_path() / expected.baseColorTexture) &&
                    material->alphaMode == expected.alphaMode,
                    "Saved mesh has an incorrect material/texture: " + object->name);
                ++checkedMeshes;
            }
        }
        Check(checkedMeshes == imported.primitives.size(), "Saved scene is missing an FBX mesh");
        std::cout << "All " << checkedMeshes << " saved meshes have the corrected UVs and source material/texture bindings.\n";
        std::unordered_map<std::string, Mesh*> meshes;
        FindMeshes(character, "", meshes);
        size_t morphOrderMismatches = 0;
        size_t faceChannel = 0;
        for (const auto& node : imported.nodes)
            for (unsigned primitiveIndex : node.primitives)
            {
                const auto& primitive = imported.primitives[primitiveIndex];
                if (primitive.morphTargets.empty()) continue;
                Check(faceChannel < rawFace.clip.channels.size(),
                    "Raw facial primitive count changed");
                const auto found = meshes.find(rawFace.clip.channels[faceChannel].targetMesh);
                Check(found != meshes.end(), "Saved facial mesh missing");
                const auto& stored = found->second->GetMorphTargets();
                Check(stored.size() == primitive.morphTargets.size(),
                    "Saved facial target count differs from fresh FBX decode");
                for (size_t target = 0; target < primitive.morphTargets.size(); ++target)
                    if (primitive.morphTargets[target].name != stored[target].name)
                    {
                        if (morphOrderMismatches == 0)
                            std::cout << "First morph order mismatch: " <<
                                rawFace.clip.channels[faceChannel].targetMesh <<
                                " target " << target << " raw=" <<
                                primitive.morphTargets[target].name << " stored=" <<
                                stored[target].name << '\n';
                        ++morphOrderMismatches;
                    }
                ++faceChannel;
            }
        std::cout << "Morph target order mismatches: " << morphOrderMismatches << '\n';
        Check(morphOrderMismatches == 0,
            "Saved morph target order differs from a fresh FBX decode");
        Memory("raw FBX and VMD decode");

        float worstPosition = 0.f, worstAngle = 0.f, worstWeight = 0.f;
        float worstNodeMatrix = 0.f;
        std::string worstWeightTarget;
        size_t checkedBones = 0, checkedWeights = 0, checkedVisibility = 0;
        const auto verify = [&](float frame)
        {
            const float time = frame / 30.f;
            manager->time = time;
            manager->layers[0].time = time;
            manager->Tick(0.f);
            const auto pose = retargeter.Sample(frame);
            // Static mesh nodes are absent from reduced animation channels.
            // Check them too: missing import scale/rotation can leave all
            // animated bones correct while the rendered character falls apart.
            for (size_t node = 0; node < nodes.size(); ++node)
            {
                Check(nodes[node] && node < pose.local.size(),
                    "Imported node missing during playback");
                const glm::mat4 actual = nodes[node]->transform.GetLocalMatrix();
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        worstNodeMatrix = std::max(worstNodeMatrix,
                            std::abs(actual[column][row] - pose.local[node][column][row]));
            }
            for (const auto& channel : body->channels)
            {
                if (channel.nodeIndex >= nodes.size() || !nodes[channel.nodeIndex])
                    continue;
                const glm::mat4 actual = nodes[channel.nodeIndex]->transform.GetLocalMatrix();
                const glm::mat4 expected = pose.local[channel.nodeIndex];
                if (channel.path == Engine::Model::AnimationChannel::Path::Translation)
                {
                    worstPosition = std::max(worstPosition, glm::distance(
                        glm::vec3(actual[3]), glm::vec3(expected[3])));
                    ++checkedBones;
                }
                else if (channel.path == Engine::Model::AnimationChannel::Path::Rotation)
                {
                    glm::vec3 scale, translation, skew;
                    glm::vec4 perspective;
                    glm::quat a, b;
                    Check(glm::decompose(actual, scale, a, translation, skew, perspective) &&
                        glm::decompose(expected, scale, b, translation, skew, perspective),
                        "Could not decompose a runtime or raw pose");
                    worstAngle = std::max(worstAngle, 2.f * std::acos(std::clamp(
                        std::abs(glm::dot(glm::normalize(a), glm::normalize(b))),
                        0.f, 1.f)));
                    ++checkedBones;
                }
            }
            for (const auto& channel : rawFace.clip.channels)
            {
                const auto found = meshes.find(channel.targetMesh);
                Check(found != meshes.end(), "Raw VMD facial mesh is absent: " +
                    channel.targetMesh);
                const auto& weights = found->second->GetMorphWeights();
                Check(weights.size() == channel.valueWidth,
                    "Saved facial target width differs from raw VMD mapping");
                for (size_t target = 0; target < weights.size(); ++target)
                {
                    const float expected = SampleWeight(channel, time, target);
                    const float difference = std::abs(weights[target] - expected);
                    if (difference > worstWeight)
                    {
                        worstWeight = difference;
                        worstWeightTarget = "frame=" + std::to_string(frame) +
                            " mesh=" + channel.targetMesh + " target=" +
                            std::to_string(target) + " actual=" +
                            std::to_string(weights[target]) + " expected=" +
                            std::to_string(expected);
                    }
                    ++checkedWeights;
                }
            }
            bool visible = true;
            for (const auto& key : raw.properties)
                if (key.frame <= frame) visible = key.visible;
            for (const auto& [path, mesh] : meshes)
                if (mesh->GetVertexCount() > 0)
                { Check(mesh->IsAnimationVisible() == visible,
                    "Runtime visibility differs from raw VMD at " + path);
                    ++checkedVisibility; }
        };
        for (float frame : { 0.f, 30.f, 60.5f, 120.f, 240.5f, 322.f })
            verify(frame);
        Check(checkedBones > 100 && checkedWeights > 500 &&
            checkedVisibility > 100, "Headless comparison covered too few targets");
        std::cout << "Raw VMD comparison: " << checkedBones << " bone values, " <<
            checkedWeights << " facial weights, " << checkedVisibility <<
            " mesh visibility checks; max errors " << worstPosition <<
            " units, " << worstAngle << " radians, " << worstWeight <<
            " weight (" << worstWeightTarget << ")\n" << std::flush;
        Check(worstPosition < 0.08f && worstAngle < 0.12f &&
            worstWeight < 0.01f, "Saved scene differs from raw VMD retargeted pose");
        std::cout << "All-node playback matrix error: " << worstNodeMatrix << '\n';
        Check(worstNodeMatrix < 0.01f,
            "Static mesh or animated node transforms differ from the corrected FBX/VMD pose");

        for (const float step : { 1.f / 30.f, 1.f / 60.f })
        {
            auto* replayObject = scene.FindObjectByName("MMD Animation Replay");
            Check(replayObject && replayObject->GetComponent<MmdDanceReplay>(),
                "Saved MMD scene has no replay script");
            manager->time = 0.f;
            manager->layers[0].time = 0.f;
            unsigned restarts = 0;
            for (float elapsed = 0.f; elapsed < raw.durationSeconds * 3.f + step * 4.f; elapsed += step)
            {
                const float before = manager->time;
                scene.Update(step);
                if (manager->time < before) ++restarts;
                Check(manager->playing &&
                    std::abs(manager->time - manager->layers[0].time) < 0.002f,
                    "Replay stopped or desynchronized body/facial animation");
            }
            Check(restarts == 3, "MMD script did not replay the dance three times");
            manager->playing = false;
            scene.Update(step);
            Check(manager->playing && manager->time == 0.f && manager->layers[0].time == 0.f,
                "MMD script did not restart stopped playback");
            std::cout << "Headless scene playback replayed the dance three times at " <<
                static_cast<int>(std::round(1.f / step)) << " fps\n";
        }
        Memory("after scene playback");
        const auto pack = Engine::Model::AnimationPackAsset::Load(
            packPath.string(), error);
        Check(pack && pack->clips.size() == 2, "Imported animation pack failed to load: " + error);
        Memory("after pack load");
        const fs::path output = fs::path(".local/test-output/mmd-headless/roundtrip.animpack");
        Check(pack->Save(output.string(), error), "Pack round-trip save failed: " + error);
        Memory("after pack save");
        Check(Engine::Model::AnimationPackAsset::Load(output.string(), error).has_value(),
            "Pack round-trip reload failed: " + error);
        fs::remove(output);
        std::cout << "Saved MMD scene and imported assets match the raw dance headlessly.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
