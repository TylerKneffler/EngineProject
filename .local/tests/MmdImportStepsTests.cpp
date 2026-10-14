#include "Editor/Core/Importers/FbxModelDecoder.h"
#include "Editor/Core/Importers/VmdMotionDecoder.h"
#include "Editor/Core/Importers/VmdSceneImporter.h"
#include "Editor/Core/Importers/MmdPoseRetargeter.h"
#include "Editor/Core/Importers/MmdDanceAssetBinder.h"
#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include <chrono>
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Assets/Scripts/Animation/MmdDanceReplay.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Model/RigAsset.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")
#endif

namespace fs = std::filesystem;

static void Memory(const char* stage)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof(info);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info)))
        std::cout << "Memory " << stage << ": current " <<
            info.WorkingSetSize / (1024 * 1024) << " MiB, peak " <<
            info.PeakWorkingSetSize / (1024 * 1024) << " MiB\n";
#else
    (void)stage;
#endif
}

static bool SaveMmdDemo(const fs::path& prefab, const fs::path& scenePath)
{
    Engine::Scene::Scene demo;
    if (!Engine::Serialization::SceneSerializer::InstantiatePrefab(
            demo, prefab.string()))
        return false;
    auto* floor = demo.AddObject("Dance Floor");
    floor->transform.position = { 0.f, -.025f, 0.f };
    floor->transform.scale = { 6.f, .04f, 6.f };
    floor->AddComponent<Engine::Components::Mesh>()->LoadFromFile(
        "Assets/Mesh/diagnostic_cube.obj");
    auto* floorMaterial = floor->AddComponent<Engine::Components::Material>();
    floorMaterial->diffuseColor = { .16f, .19f, .24f };
    floorMaterial->metallicFactor = .05f;
    floorMaterial->roughnessFactor = .85f;
    auto* cameraObject = demo.AddObject("Dance Camera");
    cameraObject->transform.position = { 0.f, .95f, 2.7f };
    auto* camera = cameraObject->AddComponent<Engine::Components::Camera>();
    camera->useTransformRotation = false;
    camera->target = { 0.f, .75f, 0.f };
    camera->fov = 50.f;
    camera->farPlane = 100.f;
    auto* lightObject = demo.AddObject("Dance Key Light");
    lightObject->transform.position = { -2.f, 3.f, 2.f };
    lightObject->transform.rotation = { .65f, 2.6f, 0.f };
    auto* light = lightObject->AddComponent<Engine::Components::Light>();
    light->lightType = static_cast<int>(Engine::Components::Light::Type::Directional);
    light->intensity = 2.f;
    light->castsShadows = true;
    demo.AddObject("MMD Animation Replay")->AddComponent<MmdDanceReplay>();
    fs::create_directories(scenePath.parent_path());
    if (!Engine::Serialization::SceneSerializer::Save(demo, scenePath.string()))
        return false;
    Engine::Scene::Scene reloaded;
    if (!Engine::Serialization::SceneSerializer::Load(reloaded, scenePath.string()) ||
        reloaded.GetObjects().size() < 4)
        return false;
    return true;
}

static bool ConsolidateMococo(const Engine::Model::ImportedModel& imported)
{
    using namespace Engine::Components;
    const fs::path directory = fs::path(MMD_TEST_ASSET_ROOT) / "Mococo Dance";
    const fs::path prefab = directory / "Mococo Dance.prefab";
    const fs::path rigPath = directory / "Mococo Dance.rig";
    const fs::path stage = fs::path(".local/test-output") /
        ("mmd-consolidation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(stage / "Meshes");
    fs::create_directories(stage / "Before");
    Engine::Scene::Scene scene;
    auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(scene, prefab.string());
    auto* model = root ? root->GetComponent<Model>() : nullptr;
    auto* manager = root ? root->GetComponent<AnimationManager>() : nullptr;
    auto* skeleton = root ? root->GetComponent<Skeleton>() : nullptr;
    auto previousRig = Engine::Model::RigAsset::Load(rigPath.string());
    if (!model || !manager || !skeleton || !previousRig || imported.skins.size() != 1) return false;
    const auto& nodes = model->ResolveNodes();
    if (nodes.size() != imported.nodes.size()) return false;
    struct Rest { glm::vec3 position, rotation, scale; };
    std::vector<Rest> rest;
    for (auto* node : nodes) rest.push_back({node->transform.position, node->transform.rotation, node->transform.scale});
    const auto playback = manager->Serialize();
    struct Entry
    {
        Mesh* mesh;
        SkinnedMesh* skin;
        const Engine::Model::ImportedPrimitive* primitive;
        std::string path;
        std::vector<float> morphWeights;
    };
    std::vector<Entry> entries;
    for (size_t index = 0; index < nodes.size(); ++index)
        for (size_t slot = 0; slot < imported.nodes[index].primitives.size(); ++slot)
        {
            const auto& indices = imported.nodes[index].primitives;
            auto* object = indices.size() == 1 ? nodes[index] : nodes[index]->Children.at(slot);
            auto* mesh = object->GetComponent<Mesh>();
            auto* skin = object->GetComponent<SkinnedMesh>();
            const auto& primitive = imported.primitives[indices[slot]];
            if (!mesh || !skin || mesh->GetVertices().size() != primitive.vertices.size()) return false;
            const auto relative = fs::weakly_canonical(mesh->GetFilePath()).lexically_relative(
                fs::weakly_canonical(directory / "Meshes"));
            if (relative.empty() || *relative.begin() == "..") return false;
            entries.push_back({mesh, skin, &primitive, mesh->GetFilePath(), mesh->GetMorphWeights()});
        }
    if (entries.size() != imported.primitives.size()) return false;
    scene.Start();
    manager->looping = false;
    const auto capture = [&](float frame)
    {
        manager->time = frame / 30.f;
        for (auto& layer : manager->layers) layer.time = manager->time;
        manager->Tick(0.f);
        std::vector<glm::vec3> positions;
        for (const auto& entry : entries)
        {
            const auto vertices = entry.mesh->BuildPosedVertices(&entry.skin->BuildPalette());
            const auto world = entry.mesh->Owner->transform.GetWorldMatrix();
            for (const auto& vertex : vertices)
                positions.emplace_back(world * glm::vec4(vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
        }
        return positions;
    };
    std::vector<std::vector<glm::vec3>> reference;
    for (float frame : {0.f, 120.f, 322.f}) reference.push_back(capture(frame));

    // These are private test-scene components, with no editor panels retaining
    // pointers. Notify the scene after removing duplicate component identities.
    for (auto it = root->Components.begin(); it != root->Components.end();)
        if (auto* extra = dynamic_cast<Skeleton*>(*it); extra && extra != skeleton)
        { extra->OnDestroy(); delete extra; it = root->Components.erase(it); }
        else ++it;
    root->NotifyStructureChanged();
    skeleton->skinIndex = 0;
    skeleton->jointNodes = imported.skins[0].jointNodes;
    skeleton->inverseBindMatrices = imported.skins[0].inverseBindMatrices;
    skeleton->MarkConfigurationDirty();
    std::unordered_map<unsigned, int> palette;
    for (size_t index = 0; index < skeleton->jointNodes.size(); ++index)
        palette.emplace(skeleton->jointNodes[index], static_cast<int>(index));
    for (auto* node : nodes)
    {
        AnimationBone* retained = nullptr;
        for (auto it = node->Components.begin(); it != node->Components.end();)
            if (auto* bone = dynamic_cast<AnimationBone*>(*it))
            {
                if (!retained) { retained = bone; ++it; }
                else { bone->OnDestroy(); delete bone; it = node->Components.erase(it); }
            }
            else ++it;
        node->NotifyStructureChanged();
    }
    for (const auto& [node, index] : palette)
    {
        auto* bone = nodes[node]->GetComponent<AnimationBone>();
        if (!bone) bone = nodes[node]->AddComponent<AnimationBone>();
        int parent = imported.nodes[node].parent;
        while (parent >= 0 && !palette.count(static_cast<unsigned>(parent))) parent = imported.nodes[parent].parent;
        bone->skinIndex = 0;
        bone->nodeIndex = node;
        bone->paletteIndex = index;
        bone->parentPaletteIndex = parent < 0 ? -1 : palette.at(static_cast<unsigned>(parent));
        bone->hierarchyRoot = parent < 0;
        bone->MarkConfigurationDirty();
    }
    for (auto& entry : entries)
    {
        const fs::path candidate = stage / "Meshes" / fs::path(entry.path).filename();
        if (!Mesh::SaveNativeFile(candidate.string(), entry.primitive->vertices)) return false;
        entry.mesh->LoadFromFile(candidate.string());
        entry.skin->skinIndex = 0;
        entry.skin->bindMeshToModel = entry.primitive->bindMeshToModel;
        entry.skin->MarkConfigurationDirty();
    }
    float worst = 0.f;
    size_t sample = 0;
    for (float frame : {0.f, 120.f, 322.f})
    {
        const auto candidate = capture(frame);
        if (candidate.size() != reference[sample].size()) return false;
        for (size_t vertex = 0; vertex < candidate.size(); ++vertex)
            worst = std::max(worst, glm::distance(candidate[vertex], reference[sample][vertex]));
        ++sample;
    }
    std::cout << "Shared-skeleton old/new world-vertex error at frames 0/120/322: " << worst << " meters\n";
    if (worst > 1e-4f) return false;
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        nodes[index]->transform.position = rest[index].position;
        nodes[index]->transform.rotation = rest[index].rotation;
        nodes[index]->transform.scale = rest[index].scale;
    }
    for (auto& entry : entries) entry.mesh->SetMorphWeights(entry.morphWeights);
    manager->Deserialize(playback);
    auto rig = Engine::Model::RigAsset::Capture(scene, *skeleton);
    rig.animationPaths = previousRig->animationPaths;
    rig.controls = previousRig->controls;
    rig.poses = previousRig->poses;
    rig.symmetryPairs = previousRig->symmetryPairs;
    rig.prefabPath = (stage / "Mococo Dance.prefab").generic_string();
    skeleton->rigPath = (stage / "Mococo Dance.rig").generic_string();
    manager->rigPaths = {skeleton->rigPath};
    if (!rig.Save(skeleton->rigPath) || !Engine::Serialization::SceneSerializer::SavePrefab(
            *root, rig.prefabPath)) return false;
    {
        Engine::Scene::Scene reload;
        auto* candidate = Engine::Serialization::SceneSerializer::InstantiatePrefab(reload, rig.prefabPath);
        auto* loadedSkeleton = candidate ? candidate->GetComponent<Skeleton>() : nullptr;
        if (!loadedSkeleton || loadedSkeleton->jointNodes != skeleton->jointNodes) return false;
    }
    // Publish only after the full geometry comparison and candidate reload.
    fs::copy_file(prefab, stage / "Before" / prefab.filename());
    fs::copy_file(rigPath, stage / "Before" / rigPath.filename());
    for (auto& entry : entries)
    {
        fs::copy_file(entry.path, stage / "Before" / fs::path(entry.path).filename());
        if (!Mesh::SaveNativeFile(entry.path, entry.primitive->vertices)) return false;
        entry.mesh->LoadFromFile(entry.path);
    }
    skeleton->rigPath = rigPath.generic_string();
    manager->Deserialize(playback);
    rig.prefabPath = prefab.generic_string();
    if (!Engine::Model::RigAsset::SaveBundle(rigPath.string(), {rig}) ||
        !Engine::Serialization::SceneSerializer::SavePrefab(*root, prefab.string())) return false;
    auto metadata = Engine::Serialization::JsonParseFile(rigPath.string() + ".meta");
    metadata["importSettings"].Set("skinCount", Engine::Serialization::JsonValue(1));
    std::ofstream metadataFile(rigPath.string() + ".meta");
    metadataFile << Engine::Serialization::JsonWrite(metadata);
    if (!metadataFile) return false;
    std::cout << "Consolidated " << entries.size() << " meshes onto one skeleton with " <<
        skeleton->jointNodes.size() << " unique bones; node hierarchy, materials and clips retained.\n";
    return true;
}

int main()
{
    std::cout << std::unitbuf;
    const fs::path source = fs::path(MMD_TEST_ASSET_ROOT) /
        "Mococo/Mococo (FBX No Outline).fbx";
    Engine::Model::ImportedModel model;
    std::string error;
    if (!Engine::Editor::FbxModelDecoder::Decode(source.string(), model, error))
    {
        std::cerr << "MMD step 1: FBX decode failed: " << error << '\n';
        return 1;
    }
    if (std::getenv("MMD_REBUILD_DEMO"))
    {
        const fs::path assetRoot = MMD_TEST_ASSET_ROOT;
        const fs::path vmd = assetRoot / "dance1.vmd";
        const fs::path profile = source.parent_path() / "Mococo.mmdrig.json";
        const fs::path mapping = source.parent_path() / "Mococo.mmdmap.json";
        Engine::Editor::VmdMotion motion;
        Engine::Editor::MmdPoseRetargeter retargeter;
        if (!Engine::Editor::VmdMotionDecoder::DecodeFile(vmd.string(), motion, error) ||
            !retargeter.Initialize(model, Engine::Serialization::JsonParseFile(profile.string()),
                Engine::Serialization::JsonParseFile(mapping.string()), motion, error))
        { std::cerr << error << '\n'; return 1; }
        float worstDifference = 0.f;
        for (float frame : { 0.f, 30.f, 60.5f, 120.f, 240.5f, 322.f })
        {
            const auto incremental = retargeter.Sample(frame);
            const auto reference = retargeter.Sample(frame, true);
            for (size_t bone = 0; bone < reference.sourceWorld.size(); ++bone)
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        worstDifference = std::max(worstDifference, std::abs(
                            incremental.sourceWorld[bone][column][row] -
                            reference.sourceWorld[bone][column][row]));
        }
        std::cout << "IK dependency update/reference matrix difference: " << worstDifference << '\n';
        if (worstDifference > 1e-5f) return 1;
        std::cout << "Decoded corrected FBX and VMD; baking body motion at 120 Hz.\n";
        auto body = retargeter.BakeBody(motion.lastFrame, "dance1 body");
        auto face = retargeter.BakeFacial(motion.lastFrame, "dance1 facial");
        std::cout << "Baked body and facial animation; writing fresh character assets.\n";
        model.name = "Mococo Dance";
        const auto result = Engine::Editor::ModelAssetWriter::Write(model,
            source.string(), "Engine/Core/Assets/mmd");
        if (!result.success || !Engine::Editor::MmdDanceAssetBinder::BindBaked(
                result.prefabPath, vmd.string(), profile.string(), mapping.string(), body, face, error))
        { std::cerr << result.message << ' ' << error << '\n'; return 1; }
        if (!SaveMmdDemo(result.prefabPath, "Engine/Core/Assets/Scenes/Animation/mmd.scene"))
        { std::cerr << "Could not save rebuilt MMD scene\n"; return 1; }
        std::ofstream credits(fs::path(result.outputDirectory) / "CREDITS.md");
        credits << "# Mococo dance demo credits\n\n"
            "Model by Antro (@antro3dcg); MMD conversion and facial work by Shinigami3D "
            "(@Shinigami_3D). Original character and design belong to hololive. "
            "See [ReadMe.txt](../Mococo/ReadMe.txt).\n\n"
            "Source motion: ../dance1.vmd; its author is unidentified in the supplied files. "
            "The motion declares model black, so artistic compatibility needs visual review.\n\n"
            "Fresh FBX import: " << model.primitives.size() << " meshes and "
            << model.skins.size() << " skin palettes. Body and facial clips share the VMD timeline. "
            "Textures are referenced from ../Mococo/Textures.\n";
        if (!credits) return 1;
        std::cout << "Rebuilt " << result.prefabPath << " and mmd.scene.\n";
        return 0;
    }
    if (std::getenv("MMD_DUMP_FBX_NODES"))
    {
        std::vector<glm::mat4> world(model.nodes.size(), glm::mat4(1.f));
        for (size_t index = 0; index < model.nodes.size(); ++index)
        {
            const auto& node = model.nodes[index];
            const glm::mat4 local = glm::translate(glm::mat4(1.f), node.translation) *
                glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.f), node.scale);
            world[index] = node.parent < 0 ? local : world[node.parent] * local;
            std::cout << index << '\t' << model.nodes[index].parent << '\t'
                      << model.nodes[index].name << '\t'
                      << model.nodes[index].translation.x << '\t'
                      << model.nodes[index].translation.y << '\t'
                      << model.nodes[index].translation.z << '\t'
                      << world[index][3].x << '\t' << world[index][3].y << '\t'
                      << world[index][3].z << '\n';
        }
        return 0;
    }

    // Compare against the source reader, not inverse binds rebuilt from the
    // same (potentially damaged) nodes. Mococo's 100x scale and -90 degree
    // rotation are required to assemble meter/Z-up meshes onto its rig.
    {
        Assimp::Importer sourceReader;
        const aiScene* original = sourceReader.ReadFile(source.string(), aiProcess_Triangulate);
        if (!original || !original->mRootNode) return 1;
        size_t index = 0;
        bool intact = true;
        const auto inspect = [&](auto&& self, const aiNode* node) -> void
        {
            if (index >= model.nodes.size()) { intact = false; return; }
            const auto& imported = model.nodes[index++];
            const glm::mat4 local = glm::translate(glm::mat4(1.f), imported.translation) *
                glm::mat4_cast(imported.rotation) * glm::scale(glm::mat4(1.f), imported.scale);
            float difference = 0.f;
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    difference = std::max(difference, std::abs(local[column][row] - node->mTransformation[row][column]));
            if (imported.name != node->mName.C_Str() || difference > .001f)
            {
                std::cerr << "FBX authored transform changed: " << node->mName.C_Str()
                          << " (matrix error " << difference << ")\n";
                intact = false;
            }
            for (unsigned child = 0; child < node->mNumChildren; ++child)
                self(self, node->mChildren[child]);
        };
        inspect(inspect, original->mRootNode);
        if (!intact || index != model.nodes.size()) return 1;
        std::cout << "MMD step 1: authored FBX transforms preserved.\n";
        if (original->mNumMeshes != model.primitives.size()) return 1;
        size_t corners = 0;
        for (unsigned meshIndex = 0; meshIndex < original->mNumMeshes; ++meshIndex)
        {
            const auto* mesh = original->mMeshes[meshIndex];
            const auto& primitive = model.primitives[meshIndex];
            if (primitive.materialIndex != static_cast<int>(mesh->mMaterialIndex)) return 1;
            size_t cornerIndex = 0;
            for (unsigned face = 0; face < mesh->mNumFaces; ++face)
                if (mesh->mFaces[face].mNumIndices == 3)
                    for (unsigned corner = 0; corner < 3; ++corner)
                    {
                        if (cornerIndex >= primitive.vertices.size()) return 1;
                        const auto sourceIndex = mesh->mFaces[face].mIndices[corner];
                        const auto& vertex = primitive.vertices[cornerIndex++];
                        for (int axis = 0; axis < 3; ++axis)
                            if (std::abs(vertex.pos[axis] - mesh->mVertices[sourceIndex][axis]) > 1e-6f) return 1;
                        for (unsigned set = 0; set < 2; ++set)
                            if (mesh->HasTextureCoords(set))
                            {
                                const auto& uv = mesh->mTextureCoords[set][sourceIndex];
                                const auto* actual = set == 0 ? vertex.uv : vertex.uv1;
                                // Assimp's vertex joining merges UVs within 1e-5.
                                if (std::abs(actual[0] - uv.x) > 1e-5f ||
                                    std::abs(actual[1] - (1.f - uv.y)) > 1e-5f)
                                { std::cerr << "FBX texture orientation mismatch: " << primitive.name
                                    << " corner " << cornerIndex - 1 << " set " << set << " actual " << actual[0] << ',' << actual[1]
                                    << " expected " << uv.x << ',' << 1.f - uv.y << '\n'; return 1; }
                            }
                    }
            if (cornerIndex != primitive.vertices.size()) return 1;
            corners += cornerIndex;
        }
        std::cout << "MMD step 1: all " << corners / 3 << " triangles, material slots and top-left UVs verified.\n";
    }

    if (std::getenv("MMD_REFRESH_MESH_ASSETS"))
    {
        const fs::path directory = fs::path(MMD_TEST_ASSET_ROOT) / "Mococo Dance/Meshes";
        std::vector<fs::path> paths(model.primitives.size());
        for (const auto& entry : fs::directory_iterator(directory))
        {
            if (entry.path().extension() != ".mesh") continue;
            const auto metadata = Engine::Serialization::JsonParseFile(entry.path().string() + ".meta");
            const auto& settings = metadata["importSettings"];
            const int primitiveIndex = settings["primitiveIndex"].IsNumber()
                ? settings["primitiveIndex"].AsInt() : -1;
            if (settings["importer"].AsString() != "model-mesh" || primitiveIndex < 0 ||
                static_cast<size_t>(primitiveIndex) >= paths.size() || !paths[primitiveIndex].empty() ||
                fs::weakly_canonical(metadata["sourcePath"].AsString()) != fs::weakly_canonical(source)) return 1;
            Engine::Components::Mesh existing;
            existing.LoadFromFile(entry.path().string());
            const auto& stored = existing.GetVertices();
            const auto& decoded = model.primitives[primitiveIndex].vertices;
            if (stored.size() != decoded.size() || existing.GetIndexCount() != 0) return 1;
            for (size_t vertex = 0; vertex < stored.size(); ++vertex)
                for (int axis = 0; axis < 3; ++axis)
                    if (stored[vertex].pos[axis] != decoded[vertex].pos[axis]) return 1;
            paths[primitiveIndex] = entry.path();
        }
        for (const auto& path : paths) if (path.empty()) return 1;
        const fs::path backup = ".local/test-output/mmd-before-uv";
        fs::create_directories(backup);
        for (size_t index = 0; index < paths.size(); ++index)
        {
            fs::copy_file(paths[index], backup / paths[index].filename(), fs::copy_options::overwrite_existing);
            if (!Engine::Components::Mesh::SaveNativeFile(paths[index].string(), model.primitives[index].vertices)) return 1;
        }
        std::cout << "Refreshed " << paths.size() << " mesh assets in place; prefab, materials, morphs, rig, animation and asset IDs retained.\n";
        return 0;
    }

    if (model.primitives.size() < 55 || model.nodes.size() < 269 ||
        model.skins.size() != 1 || std::abs(model.unitScaleToMeters - .01f) > 1e-6f)
    {
        std::cerr << "MMD step 1: incomplete character import: "
                  << model.primitives.size() << " meshes, "
                  << model.nodes.size() << " nodes, "
                  << model.skins.size() << " skins\n";
        return 1;
    }
    std::cout << "MMD step 1: FBX decoded (" << model.primitives.size()
              << " meshes, " << model.nodes.size() << " nodes, "
              << model.skins.size() << " skins).\n";
    if (std::getenv("MMD_CONSOLIDATE_PREFAB")) return ConsolidateMococo(model) ? 0 : 1;

    // Step 2: every vertex in a skinned mesh must have a usable palette weight.
    // This checks the actual post-import vertices, including the eight-weight
    // reduction, rather than only the FBX source data.
    size_t skinnedVertices = 0;
    size_t unweightedVertices = 0;
    size_t invalidVertices = 0;
    size_t facialExtraVertices = 0;
    size_t facialExtraUnweighted = 0;
    for (const auto& primitive : model.primitives)
    {
        if (primitive.skinIndex < 0) continue;
        const auto& skin = model.skins.at(static_cast<size_t>(primitive.skinIndex));
        const bool facialExtra = primitive.name.find("FacialsExtra_") != std::string::npos;
        for (const auto& vertex : primitive.vertices)
        {
            ++skinnedVertices;
            if (facialExtra) ++facialExtraVertices;
            float sum = 0.f;
            for (int slot = 0; slot < 8; ++slot)
            {
                const float weight = slot < 4 ? vertex.weights0[slot] : vertex.weights1[slot - 4];
                const float joint = slot < 4 ? vertex.joints0[slot] : vertex.joints1[slot - 4];
                if (!std::isfinite(weight) || weight < 0.f ||
                    (weight > 0.f && (!std::isfinite(joint) || joint < 0.f ||
                     joint >= static_cast<float>(skin.jointNodes.size()))))
                    ++invalidVertices;
                sum += weight;
            }
            if (sum <= 0.f)
            {
                ++unweightedVertices;
                if (facialExtra) ++facialExtraUnweighted;
            }
            else if (std::abs(sum - 1.f) > 1e-4f)
                ++invalidVertices;
        }
    }
    if (skinnedVertices == 0 || unweightedVertices || invalidVertices ||
        facialExtraVertices == 0)
    {
        std::cerr << "MMD step 2: invalid skinning: " << skinnedVertices
                  << " skinned vertices, " << unweightedVertices
                  << " unweighted, " << invalidVertices << " invalid, "
                  << facialExtraUnweighted << '/' << facialExtraVertices
                  << " FacialsExtra_ unweighted. Later steps were not run.\n";
        for (const auto& primitive : model.primitives)
            if (primitive.name.find("FacialsExtra_") != std::string::npos &&
                primitive.skinIndex >= 0)
            {
                const auto& skin = model.skins.at(static_cast<size_t>(primitive.skinIndex));
                std::cerr << "FacialsExtra_ palette:";
                for (unsigned node : skin.jointNodes)
                    std::cerr << ' ' << model.nodes.at(node).name;
                std::cerr << '\n';
            }
        return 2;
    }
    std::cout << "MMD step 2 weight gate passed; prefab, joint deformation, "
                 "and later steps still require verification.\n";

    // Check the head attachment against the imported rest transforms. At the
    // bind pose the palette must leave FacialsExtra_ vertices in place; a head
    // rotation must then move them as a rigid facial attachment.
    std::vector<glm::mat4> world(model.nodes.size(), glm::mat4(1.f));
    for (size_t index = 0; index < model.nodes.size(); ++index)
    {
        const auto& node = model.nodes[index];
        const glm::mat4 local = glm::translate(glm::mat4(1.f), node.translation) *
            glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.f), node.scale);
        world[index] = node.parent < 0 ? local : world.at(static_cast<size_t>(node.parent)) * local;
    }
    bool checkedFacialBind = false;
    if (std::getenv("MMD_VALIDATE_PREFAB"))
    {
        const fs::path output = ".local/test-output/mococo-rest";
        fs::create_directories(output);
        const char* existingPrefab = std::getenv("MMD_PREFAB_PATH");
        const auto result = existingPrefab ? Engine::Model::ModelImportResult {
            true, existingPrefab, {}, {} } : Engine::Editor::ModelAssetWriter::Write(
            model, source.string(), output.string());
        if (!result.success) { std::cerr << result.message << '\n'; return 2; }
        Engine::Scene::Scene scene;
        auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            scene, result.prefabPath);
        auto* loaded = root ? root->GetComponent<Engine::Components::Model>() : nullptr;
        if (!loaded) return 2;
        const glm::mat4 modelWorld = root->transform.GetWorldMatrix();
        if (!existingPrefab && glm::distance(root->transform.scale,
                glm::vec3(model.unitScaleToMeters)) > 1e-6f) return 2;
        const bool repair = existingPrefab && std::getenv("MMD_REPAIR_PREFAB");
        if (repair)
        {
            // Preserve existing clips, materials, authored mesh files and rig
            // references. The old shared rig was bound against the stripped
            // mesh transforms; compensate per mesh before restoring the nodes.
            for (size_t index = 0; index < model.nodes.size(); ++index)
            {
                auto* object = loaded->ResolveNode(static_cast<unsigned>(index));
                if (!object) return 2;
                const glm::mat4 oldWorld = object->transform.GetWorldMatrix();
                for (size_t slot = 0; slot < model.nodes[index].primitives.size(); ++slot)
                {
                    auto* target = model.nodes[index].primitives.size() == 1 ? object : object->Children.at(slot);
                    if (auto* deformer = target->GetComponent<Engine::Components::SkinnedMesh>())
                        deformer->bindMeshToModel *= glm::inverse(oldWorld) * modelWorld * world[index];
                }
            }
            for (size_t index = 0; index < model.nodes.size(); ++index)
            {
                auto* object = loaded->ResolveNode(static_cast<unsigned>(index));
                const auto& imported = model.nodes[index];
                const glm::mat4 expectedLocal = glm::translate(glm::mat4(1.f), imported.translation) *
                    glm::mat4_cast(imported.rotation) * glm::scale(glm::mat4(1.f), imported.scale);
                const glm::mat4 existingLocal = object->transform.GetLocalMatrix();
                float difference = 0.f;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        difference = std::max(difference, std::abs(expectedLocal[c][r] - existingLocal[c][r]));
                if (difference <= .001f) continue;
                const glm::quat q = glm::normalize(imported.rotation);
                object->transform.position = imported.translation;
                object->transform.rotation = {
                    std::atan2(2.f * (q.w * q.x + q.y * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y)),
                    std::asin(std::clamp(2.f * (q.w * q.y - q.z * q.x), -1.f, 1.f)),
                    std::atan2(2.f * (q.w * q.z + q.x * q.y), 1.f - 2.f * (q.y * q.y + q.z * q.z)) };
                object->transform.scale = imported.scale;
            }
        }
        float worstTransform = 0.f, worstVertex = 0.f;
        unsigned failedNodes = 0;
        for (size_t index = 0; index < model.nodes.size(); ++index)
        {
            auto* object = loaded->ResolveNode(static_cast<unsigned>(index));
            if (!object) return 2;
            const glm::mat4 actual = object->transform.GetWorldMatrix();
            float difference = 0.f;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    difference = std::max(difference, std::abs(actual[c][r] - (modelWorld * world[index])[c][r]));
            worstTransform = std::max(worstTransform, difference);
            if (difference > .01f && failedNodes++ < 12)
                std::cerr << "Prefab transform mismatch " << index << ' ' << object->name
                          << ": " << difference << " rotation " << object->transform.rotation.x
                          << ',' << object->transform.rotation.y << ',' << object->transform.rotation.z << '\n';
            for (size_t slot = 0; slot < model.nodes[index].primitives.size(); ++slot)
            {
                const auto& primitive = model.primitives[model.nodes[index].primitives[slot]];
                auto* target = model.nodes[index].primitives.size() == 1 ? object : object->Children[slot];
                auto* deformer = target->GetComponent<Engine::Components::SkinnedMesh>();
                if (!deformer || primitive.skinIndex < 0) continue;
                const auto& palette = deformer->BuildPalette();
                const size_t stride = std::max<size_t>(1, primitive.vertices.size() / 128);
                for (size_t i = 0; i < primitive.vertices.size(); i += stride)
                {
                    const auto& vertex = primitive.vertices[i];
                    const glm::vec4 position(vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f);
                    glm::vec4 skinned(0.f);
                    for (int influence = 0; influence < 8; ++influence)
                    {
                        const float weight = influence < 4 ? vertex.weights0[influence] : vertex.weights1[influence - 4];
                        if (weight <= 0.f) continue;
                        const size_t joint = static_cast<size_t>(influence < 4 ? vertex.joints0[influence] : vertex.joints1[influence - 4]);
                        skinned += weight * palette.at(joint) * position;
                    }
                    worstVertex = std::max(worstVertex, glm::length(glm::vec3(
                        target->transform.GetWorldMatrix() * skinned - modelWorld * world[index] * position)));
                }
            }
        }
        std::cout << "Prefab rest validation: " << failedNodes << " changed nodes, worst transform "
                  << worstTransform << ", worst skinned vertex " << worstVertex << '\n';
        if (repair && worstTransform <= .01f && worstVertex <= .05f)
        {
            if (!Engine::Serialization::SceneSerializer::SavePrefab(*root, existingPrefab)) return 2;
            std::cout << "Repaired authored transforms and mesh bind corrections in " << existingPrefab << '\n';
        }
        return worstTransform > .01f || worstVertex > .05f ? 2 : 0;
    }
    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
        for (unsigned primitiveIndex : model.nodes[nodeIndex].primitives)
        {
            const auto& primitive = model.primitives.at(primitiveIndex);
            if (primitive.name.find("FacialsExtra_") == std::string::npos ||
                primitive.skinIndex < 0 || primitive.vertices.empty()) continue;
            const auto& skin = model.skins.at(static_cast<size_t>(primitive.skinIndex));
            for (size_t palette = 0; palette < skin.jointNodes.size(); ++palette)
            {
                if (model.nodes[skin.jointNodes[palette]].name != "head.x") continue;
                const glm::mat4 rest = world[skin.jointNodes[palette]] *
                    skin.inverseBindMatrices.at(palette) * primitive.bindMeshToModel;
                float maxBindError = 0.f;
                for (const auto& vertex : primitive.vertices)
                {
                    const glm::vec4 position(vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f);
                    const float bindError = glm::length(glm::vec3(
                        rest * position - world[nodeIndex] * position));
                    maxBindError = std::max(maxBindError, bindError);
                }
                if (maxBindError > 0.01f)
                {
                    std::cerr << "MMD step 2: FacialsExtra_ head bind-pose error "
                              << maxBindError << " units\n";
                    return 2;
                }
                checkedFacialBind = true;
            }
        }
    if (!checkedFacialBind)
    {
        std::cerr << "MMD step 2: FacialsExtra_ head bind pose was not checked\n";
        return 2;
    }
    std::cout << "MMD step 2: facial head bind pose verified.\n";

    float worstSkinBind = 0.f;
    std::string worstMesh;
    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
        for (unsigned primitiveIndex : model.nodes[nodeIndex].primitives)
        {
            const auto& primitive = model.primitives.at(primitiveIndex);
            if (primitive.skinIndex < 0) continue;
            const auto& skin = model.skins.at(static_cast<size_t>(primitive.skinIndex));
            const size_t stride = std::max<size_t>(1, primitive.vertices.size() / 64);
            for (size_t vertexIndex = 0; vertexIndex < primitive.vertices.size(); vertexIndex += stride)
            {
                const auto& vertex = primitive.vertices[vertexIndex];
                const glm::vec4 position(vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f);
                glm::vec4 skinned(0.f);
                for (int slot = 0; slot < 8; ++slot)
                {
                    const float weight = slot < 4 ? vertex.weights0[slot] : vertex.weights1[slot - 4];
                    if (weight <= 0.f) continue;
                    const size_t joint = static_cast<size_t>(slot < 4 ?
                        vertex.joints0[slot] : vertex.joints1[slot - 4]);
                    skinned += weight * world.at(skin.jointNodes.at(joint)) *
                        skin.inverseBindMatrices.at(joint) * primitive.bindMeshToModel * position;
                }
                const float bindError = glm::length(glm::vec3(skinned - world[nodeIndex] * position));
                if (bindError > worstSkinBind) { worstSkinBind = bindError; worstMesh = primitive.name; }
            }
        }
    if (worstSkinBind > 0.01f)
    {
        std::cerr << "MMD step 2: mesh bind mismatch in " << worstMesh << ": "
                  << worstSkinBind << " units\n";
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
            for (unsigned primitiveIndex : model.nodes[nodeIndex].primitives)
                if (model.primitives[primitiveIndex].name == worstMesh)
                {
                    const auto& primitive = model.primitives[primitiveIndex];
                    const auto& skin = model.skins[model.primitives[primitiveIndex].skinIndex];
                    const glm::mat4 firstRest = world[skin.jointNodes.front()] * skin.inverseBindMatrices.front() * primitive.bindMeshToModel;
                    std::cerr << "owner " << model.nodes[nodeIndex].name << " scale "
                              << glm::length(glm::vec3(world[nodeIndex][0])) << " first rest scale "
                              << glm::length(glm::vec3(firstRest[0])) << " parent "
                              << model.nodes[nodeIndex].parent << '\n';
                    float worstRest = 0.f;
                    std::string worstJoint;
                    for (size_t joint = 0; joint < skin.jointNodes.size(); ++joint)
                    {
                        const glm::mat4 rest = world[skin.jointNodes[joint]] * skin.inverseBindMatrices[joint] * primitive.bindMeshToModel;
                        const float delta = glm::length(glm::vec3(rest[3] - world[nodeIndex][3])) +
                            glm::length(glm::vec3(rest[0] - world[nodeIndex][0]));
                        if (delta > worstRest) { worstRest = delta; worstJoint = model.nodes[skin.jointNodes[joint]].name; }
                    }
                    const size_t sameName = std::count_if(model.nodes.begin(), model.nodes.end(),
                        [&](const Engine::Model::ImportedNode& node) { return node.name == worstJoint; });
                    std::cerr << "worst joint rest " << worstJoint << " delta "
                              << worstRest << ", name matches " << sameName << '\n';
                }
        return 2;
    }
    std::cout << "MMD step 2: sampled skin bind poses verified.\n";

    for (const char* jointName : { "head.x", "index1.l", "Tail" })
    {
        float maxMotion = 0.f;
        size_t influencedVertices = 0;
        for (const auto& primitive : model.primitives)
        {
            if (primitive.skinIndex < 0) continue;
            const auto& skin = model.skins[static_cast<size_t>(primitive.skinIndex)];
            for (size_t joint = 0; joint < skin.jointNodes.size(); ++joint)
            {
                if (model.nodes[skin.jointNodes[joint]].name != jointName) continue;
                const glm::mat4 rest = world[skin.jointNodes[joint]] *
                    skin.inverseBindMatrices[joint] * primitive.bindMeshToModel;
                const glm::mat4 rotated = world[skin.jointNodes[joint]] *
                    glm::rotate(glm::mat4(1.f), 0.25f, glm::vec3(0.f, 1.f, 0.f)) *
                    skin.inverseBindMatrices[joint] * primitive.bindMeshToModel;
                const size_t stride = std::max<size_t>(1, primitive.vertices.size() / 256);
                for (size_t vertexIndex = 0; vertexIndex < primitive.vertices.size(); vertexIndex += stride)
                {
                    const auto& vertex = primitive.vertices[vertexIndex];
                    for (int slot = 0; slot < 8; ++slot)
                    {
                        const float weight = slot < 4 ? vertex.weights0[slot] : vertex.weights1[slot - 4];
                        const size_t palette = static_cast<size_t>(slot < 4 ?
                            vertex.joints0[slot] : vertex.joints1[slot - 4]);
                        if (palette != joint || weight < 0.05f) continue;
                        const glm::vec4 position(vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f);
                        const float motion = weight * glm::length(glm::vec3(
                            rotated * position - rest * position));
                        if (std::isfinite(motion)) maxMotion = std::max(maxMotion, motion);
                        ++influencedVertices;
                    }
                }
            }
        }
        if (influencedVertices == 0 || maxMotion < 1e-4f)
        {
            std::cerr << "MMD step 2: joint motion failed for " << jointName
                      << " (" << influencedVertices << " influenced samples, "
                      << maxMotion << " units maximum motion)\n";
            return 2;
        }
        std::cout << "MMD step 2: " << jointName << " moves weighted vertices.\n";
    }

    // Step 3 begins with the material references produced by the decoder.
    size_t texturedMaterials = 0;
    for (const auto& material : model.materials)
        if (!material.baseColorTexture.empty()) ++texturedMaterials;
    if (model.materials.size() < 7 || texturedMaterials != model.materials.size())
    {
        std::cerr << "MMD step 3: material textures missing: "
                  << texturedMaterials << '/' << model.materials.size()
                  << " materials have a base-color texture. Later steps were not run.\n";
        for (size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex)
        {
            std::cerr << "  [" << materialIndex << "] " << model.materials[materialIndex].name
                      << " meshes:";
            for (const auto& primitive : model.primitives)
                if (primitive.materialIndex == static_cast<int>(materialIndex))
                    std::cerr << ' ' << primitive.name;
            std::cerr << '\n';
        }
        return 3;
    }
    std::cout << "MMD step 3: decoder material textures assigned.\n";

    const std::array<std::pair<const char*, const char*>, 7> expectedTextures{{
        { "MAT_Head", "Textures/Head.png" },
        { "MAT_Body", "Textures/Body.png" },
        { "Facials extra", "Textures/Facials.png" },
        { "MAT_HairTailEar", "Textures/Hair_Tail.png" },
        { "MAT_Shoes_ExtraAcc", "Textures/Shoes_Extra.png" },
        { "MAT_Outfit1", "Textures/Outfit1_Acc.png" },
        { "MAT_Jacket", "Textures/Jacket.png" },
    }};
    for (const auto& [name, expectedPath] : expectedTextures)
    {
        const auto found = std::find_if(model.materials.begin(), model.materials.end(),
            [&](const Engine::Model::ImportedMaterial& material)
            { return material.name == name; });
        if (found == model.materials.end() || found->baseColorTexture != expectedPath ||
            !fs::is_regular_file(source.parent_path() / expectedPath))
        {
            std::cerr << "MMD step 3: missing or incorrect portable texture for "
                      << name << '\n';
            return 3;
        }
        const bool facial = std::string(name) == "Facials extra";
        if (found->alphaMode != (facial ? "Blend" : "Opaque"))
        {
            std::cerr << "MMD step 3: incorrect alpha mode for " << name << '\n';
            return 3;
        }
    }
    std::cout << "MMD step 3: texture paths and facial alpha verified.\n";

    // Step 4 requires a reproducible PMX rig profile before retargeting.
    const fs::path pmx = source.parent_path() / "Mococo Abyssgard.pmx";
    const fs::path profile = source.parent_path() / "Mococo.mmdrig.json";
    if (!fs::is_regular_file(pmx) || !fs::is_regular_file(profile))
    {
        std::cerr << "MMD step 4: PMX rig profile is missing ("
                  << profile.filename().string() << "). Later steps were not run.\n";
        return 4;
    }
    const auto rig = Engine::Serialization::JsonParseFile(profile.string());
    const auto& bones = rig["bones"];
    if (rig["format"].AsString() != "mmd-rig-profile-v1" ||
        rig["boneCount"].AsInt() != 239 || bones.ArraySize() != 239 ||
        rig["ikChainCount"].AsInt() != 4 ||
        rig["sourceSha256"].AsString().size() != 64)
    {
        std::cerr << "MMD step 4: PMX rig profile header is incomplete\n";
        return 4;
    }
    size_t ikChains = 0, inherited = 0, fixedAxes = 0, localAxes = 0;
    for (size_t index = 0; index < bones.ArraySize(); ++index)
    {
        const auto& bone = bones.ArrayAt(index);
        const int parent = bone["parent"].AsInt();
        if (bone["index"].AsInt() != static_cast<int>(index) ||
            parent < -1 || parent >= static_cast<int>(bones.ArraySize()) ||
            !bone["position"].IsArray() || bone["position"].ArraySize() != 3 ||
            !bone["localTranslation"].IsArray() ||
            bone["localTranslation"].ArraySize() != 3)
        {
            std::cerr << "MMD step 4: invalid PMX bone " << index << '\n';
            return 4;
        }
        if (bone.Has("inherit")) ++inherited;
        if (bone.Has("fixedAxis")) ++fixedAxes;
        if (bone.Has("localAxes")) ++localAxes;
        if (bone.Has("ik"))
        {
            ++ikChains;
            const auto& ik = bone["ik"];
            if (ik["target"].AsInt() < 0 || !ik["links"].IsArray() ||
                ik["links"].ArraySize() == 0 ||
                ik["angleLimitRadians"].AsFloat() <= 0.f)
            {
                std::cerr << "MMD step 4: invalid IK chain on bone " << index << '\n';
                return 4;
            }
        }
    }
    if (ikChains != 4 || inherited == 0 || fixedAxes == 0 || localAxes == 0)
    {
        std::cerr << "MMD step 4: missing inherited transforms or bone axes\n";
        return 4;
    }
    std::cout << "MMD step 4: 239 bones, 4 IK chains, inherited transforms "
                 "and axes verified.\n";

    // Step 5 parses the supplied motion before any rig mapping is attempted.
    const fs::path vmd = source.parent_path().parent_path() / "dance1.vmd";
    Engine::Editor::VmdMotion dance;
    if (!Engine::Editor::VmdMotionDecoder::DecodeFile(vmd.string(), dance, error))
    {
        std::cerr << "MMD step 5: VMD decode failed: " << error << '\n';
        return 5;
    }
    std::cout << "MMD step 5: parsed " << dance.bones.size() << " bone, "
              << dance.morphs.size() << " morph and " << dance.properties.size()
              << " property keys through frame " << dance.lastFrame << ".\n";
    if (dance.bones.size() != 5704 || dance.morphs.size() != 213 ||
        dance.properties.size() != 1 || dance.lastFrame != 322 ||
        std::abs(dance.durationSeconds - 322.f / 30.f) > 1e-4f ||
        dance.modelName != "black")
    {
        std::cerr << "MMD step 5: dance track totals or 30 fps timing changed\n";
        return 5;
    }
    std::cout << "MMD step 5: source motion and 30 fps duration verified.\n";
    if (std::getenv("MMD_DUMP_FACE_NAMES"))
    {
        std::unordered_set<std::string> names;
        for (const auto& key : dance.morphs) names.insert(key.name);
        std::cout << "VMD morph names (" << names.size() << "):\n";
        for (const auto& name : names) std::cout << "  " << name << '\n';
        for (const auto& primitive : model.primitives)
            if (!primitive.morphTargets.empty())
            {
                size_t matched = 0;
                for (const auto& target : primitive.morphTargets)
                    if (names.count(target.name)) ++matched;
                std::cout << "FBX " << primitive.name << " ("
                    << primitive.morphTargets.size() << ", " << matched
                    << " exact names):\n";
                for (const auto& target : primitive.morphTargets)
                    std::cout << "  " << target.name << '\n';
            }
        for (size_t node = 0; node < model.nodes.size(); ++node)
            for (unsigned primitive : model.nodes[node].primitives)
                if (primitive < model.primitives.size() &&
                    !model.primitives[primitive].morphTargets.empty())
                    std::cout << "OWNER " << node << " "
                              << model.nodes[node].name << " primitive "
                              << primitive << " "
                              << model.primitives[primitive].name << '\n';
        return 0;
    }

    const fs::path mapping = source.parent_path() / "Mococo.mmdmap.json";
    if (!fs::is_regular_file(mapping))
    {
        std::cerr << "MMD step 6: PMX-to-FBX retarget mapping is missing ("
                  << mapping.filename().string() << "). Later steps were not run.\n";
        return 6;
    }
    const auto map = Engine::Serialization::JsonParseFile(mapping.string());
    const auto& entries = map["bones"];
    const auto& conversion = map["coordinates"];
    const auto& aliases = map["motionAliases"];
    const auto& report = map["trackReport"];
    if (map["format"].AsString() != "mmd-retarget-map-v1" ||
        map["motion"]["declaredModelName"].AsString() != dance.modelName ||
        map["motion"]["sourceRigIdentity"].AsString().empty() ||
        map["targetRig"]["nodeCount"].AsInt() != static_cast<int>(model.nodes.size()) ||
        conversion["pmxToFbxWorldScale"].AsFloat() != 8.f ||
        conversion["axisSigns"].ArraySize() != 3 ||
        conversion["axisSigns"].ArrayAt(2).AsInt() != -1 ||
        map["rootMotion"]["composePmxIndicesInOrder"].ArraySize() != 5 ||
        entries.ArraySize() != bones.ArraySize() || aliases.ArraySize() != 9)
    {
        std::cerr << "MMD step 6: retarget map metadata is incomplete\n";
        return 6;
    }
    std::vector<glm::mat4> targetWorld(model.nodes.size(), glm::mat4(1.f));
    for (size_t index = 0; index < model.nodes.size(); ++index)
    {
        const auto& node = model.nodes[index];
        const glm::mat4 local = glm::translate(glm::mat4(1.f), node.translation) *
            glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.f), node.scale);
        targetWorld[index] = node.parent < 0 ? local : targetWorld[node.parent] * local;
    }
    size_t spatial = 0, manual = 0;
    for (size_t index = 0; index < entries.ArraySize(); ++index)
    {
        const auto& entry = entries.ArrayAt(index);
        const int target = entry["fbxNode"].AsInt();
        if (entry["pmxIndex"].AsInt() != static_cast<int>(index) ||
            entry["pmxName"].AsString() != bones.ArrayAt(index)["name"].AsString() ||
            target < 57 || target >= static_cast<int>(model.nodes.size()) ||
            entry["fbxName"].AsString() != model.nodes[target].name ||
            entry["role"].AsString().empty())
        {
            std::cerr << "MMD step 6: stale or invalid bone mapping " << index << '\n';
            return 6;
        }
        if (entry["method"].AsString() == "rest-position")
        {
            ++spatial;
            const auto& position = bones.ArrayAt(index)["position"];
            const glm::vec3 expected(position.ArrayAt(0).AsFloat() * 8.f,
                position.ArrayAt(1).AsFloat() * 8.f,
                position.ArrayAt(2).AsFloat() * -8.f);
            if (glm::distance(expected, glm::vec3(targetWorld[target][3])) > 0.01f)
            {
                std::cerr << "MMD step 6: rest-position mismatch for PMX bone " << index << '\n';
                return 6;
            }
        }
        else if (entry["method"].AsString() == "manual") ++manual;
        else { std::cerr << "MMD step 6: unknown mapping method\n"; return 6; }
    }
    std::unordered_set<std::string> aliasesSeen;
    for (size_t index = 0; index < aliases.ArraySize(); ++index)
    {
        const auto& alias = aliases.ArrayAt(index);
        const int pmxIndex = alias["pmxIndex"].AsInt();
        if (pmxIndex < 0 || pmxIndex >= static_cast<int>(bones.ArraySize()) ||
            !aliasesSeen.insert(alias["vmdName"].AsString()).second)
        { std::cerr << "MMD step 6: invalid motion alias\n"; return 6; }
    }
    std::unordered_map<std::string, size_t> danceTrackCounts;
    for (const auto& key : dance.bones) ++danceTrackCounts[key.name];
    size_t reportedKeys = 0;
    const auto& unmatched = report["unmatchedBoneTracks"];
    for (size_t index = 0; index < unmatched.ArraySize(); ++index)
    {
        const auto& item = unmatched.ArrayAt(index);
        const auto found = danceTrackCounts.find(item["name"].AsString());
        if (found == danceTrackCounts.end() ||
            static_cast<int>(found->second) != item["keyCount"].AsInt())
        { std::cerr << "MMD step 6: unmatched-track report is stale\n"; return 6; }
        reportedKeys += found->second;
    }
    if (spatial != 214 || manual != 25 ||
        report["totalBoneTracks"].AsInt() != static_cast<int>(danceTrackCounts.size()) ||
        unmatched.ArraySize() != 705 || report["unmatchedAnimatedTracks"].ArraySize() != 0 ||
        reportedKeys == 0)
    { std::cerr << "MMD step 6: retarget mapping coverage changed\n"; return 6; }
    std::cout << "MMD step 6: validated 214 spatial and 25 manual PMX mappings, "
                 "9 motion aliases and 705 reported unmatched tracks.\n";
    Engine::Editor::MmdPoseRetargeter retargeter;
    if (!retargeter.Initialize(model, rig, map, dance, error))
    { std::cerr << "MMD step 6: pose converter failed: " << error << '\n'; return 6; }
    for (float frame : { 0.f, 30.f, 60.5f, 120.f, 240.5f, 322.f })
    {
        const auto incremental = retargeter.Sample(frame);
        const auto reference = retargeter.Sample(frame, true);
        for (size_t bone = 0; bone < reference.sourceWorld.size(); ++bone)
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    if (std::abs(incremental.sourceWorld[bone][column][row] -
                            reference.sourceWorld[bone][column][row]) > 1e-5f)
                    { std::cerr << "IK dependency update differs from full solver\n"; return 6; }
    }
    Engine::Editor::VmdMotion neutral;
    Engine::Editor::MmdPoseRetargeter restConverter;
    if (!restConverter.Initialize(model, rig, map, neutral, error))
    { std::cerr << "MMD step 6: neutral converter failed: " << error << '\n'; return 6; }
    const auto rest = restConverter.Sample(0.f);
    if (rest.local.size() != model.nodes.size())
    { std::cerr << "MMD step 6: incomplete neutral pose\n"; return 6; }
    for (size_t index = 0; index < rest.world.size(); ++index)
    {
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                if (std::abs(rest.world[index][column][row] -
                    targetWorld[index][column][row]) > 0.01f)
                { std::cerr << "MMD step 6: neutral pose changed node " << index << '\n'; return 6; }
    }
    Engine::Editor::VmdMotion synthetic;
    for (size_t sourceIndex = 0; sourceIndex < 5; ++sourceIndex)
    {
        Engine::Editor::VmdBoneFrame key;
        key.name = bones.ArrayAt(sourceIndex)["name"].AsString();
        key.translation = { 1.f, 0.f, 0.f };
        key.rotation = { 0.f, 0.f, 0.f, 1.f };
        synthetic.bones.push_back(key);
    }
    Engine::Editor::MmdPoseRetargeter rootConverter;
    if (!rootConverter.Initialize(model, rig, map, synthetic, error))
    { std::cerr << "MMD step 6: root composition failed: " << error << '\n'; return 6; }
    const auto rootPose = rootConverter.Sample(0.f);
    const int rootNode = map["rootMotion"]["targetNode"].AsInt();
    if (std::abs(rootPose.world[rootNode][3].x - targetWorld[rootNode][3].x - 40.f) > 0.01f)
    { std::cerr << "MMD step 6: five root translations did not compose\n"; return 6; }
    auto rotationKey = [&](int sourceIndex, float radians, const glm::vec3& axis)
    {
        Engine::Editor::VmdBoneFrame key;
        key.name = bones.ArrayAt(sourceIndex)["name"].AsString();
        const glm::quat rotation = glm::angleAxis(radians, axis);
        key.rotation = { rotation.x, rotation.y, rotation.z, rotation.w };
        return key;
    };
    Engine::Editor::VmdMotion helperMotion;
    helperMotion.bones.push_back(rotationKey(150, 0.4f, { 0.f, 0.f, 1.f }));
    helperMotion.bones.push_back(rotationKey(44, 0.8f, { 1.f, 0.f, 0.f }));
    Engine::Editor::MmdPoseRetargeter helperConverter;
    if (!helperConverter.Initialize(model, rig, map, helperMotion, error))
    { std::cerr << "MMD step 6: inherited rotations failed: " << error << '\n'; return 6; }
    const auto helperPose = helperConverter.Sample(0.f);
    const glm::vec3 shoulderAxis = glm::vec3(helperPose.sourceWorld[152] *
        glm::vec4(1.f, 0.f, 0.f, 0.f));
    if (glm::distance(shoulderAxis, glm::vec3(1.f, 0.f, 0.f)) > 0.001f)
    { std::cerr << "MMD step 6: shoulder child did not cancel parent rotation\n"; return 6; }
    const glm::vec3 eyeAxis = glm::vec3(helperPose.sourceWorld[45] *
        glm::vec4(0.f, 1.f, 0.f, 0.f));
    if (glm::distance(eyeAxis, glm::vec3(0.f, std::cos(0.24f), std::sin(0.24f))) > 0.001f)
    { std::cerr << "MMD step 6: fractional eye inheritance was lost\n"; return 6; }
    const int shoulderNode = entries.ArrayAt(151)["fbxNode"].AsInt();
    const int shoulderChildNode = entries.ArrayAt(154)["fbxNode"].AsInt();
    const glm::mat4 shoulderDelta = helperPose.world[shoulderNode] *
        glm::inverse(targetWorld[shoulderNode]);
    const glm::mat4 shoulderChildDelta = helperPose.world[shoulderChildNode] *
        glm::inverse(targetWorld[shoulderChildNode]);
    const glm::vec3 shoulderDirection = glm::vec3(shoulderDelta *
        glm::vec4(1.f, 0.f, 0.f, 0.f));
    const glm::vec3 childDirection = glm::vec3(shoulderChildDelta *
        glm::vec4(1.f, 0.f, 0.f, 0.f));
    if (glm::distance(shoulderDirection,
            glm::vec3(std::cos(0.4f), std::sin(0.4f), 0.f)) > 0.002f ||
        glm::distance(childDirection, glm::vec3(1.f, 0.f, 0.f)) > 0.002f)
    { std::cerr << "MMD step 6: FBX shoulder pose lost parent/cancel roles\n"; return 6; }
    Engine::Editor::VmdMotion waistMotion;
    waistMotion.bones.push_back(rotationKey(3, 0.5f, { 0.f, 1.f, 0.f }));
    Engine::Editor::MmdPoseRetargeter waistConverter;
    if (!waistConverter.Initialize(model, rig, map, waistMotion, error))
    { std::cerr << "MMD step 6: waist cancellation failed: " << error << '\n'; return 6; }
    const auto waistPose = waistConverter.Sample(0.f);
    for (const int sourceIndex : { 228, 229 })
    {
        const glm::vec3 cancelledAxis = glm::vec3(waistPose.sourceWorld[sourceIndex] *
            glm::vec4(1.f, 0.f, 0.f, 0.f));
        if (glm::distance(cancelledAxis, glm::vec3(1.f, 0.f, 0.f)) > 0.001f)
        { std::cerr << "MMD step 6: waist rotation was not cancelled\n"; return 6; }
    }
    Engine::Editor::VmdMotion unsupportedTwist;
    unsupportedTwist.bones.push_back(rotationKey(155, 0.2f, { 1.f, 0.f, 0.f }));
    Engine::Editor::MmdPoseRetargeter rejectedConverter;
    if (rejectedConverter.Initialize(model, rig, map, unsupportedTwist, error) ||
        error.find("twist") == std::string::npos)
    { std::cerr << "MMD step 6: unmappable animated twist was accepted\n"; return 6; }
    std::unordered_set<int> directlyMapped { rootNode };
    for (size_t index = 0; index < entries.ArraySize(); ++index)
    {
        const auto& entry = entries.ArrayAt(index);
        if (entry["role"].AsString() == "joint" ||
            entry["role"].AsString() == "shoulder-parent")
            directlyMapped.insert(entry["fbxNode"].AsInt());
    }
    for (size_t index = 0; index < model.nodes.size(); ++index)
        if (directlyMapped.find(static_cast<int>(index)) == directlyMapped.end())
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    if (std::abs(rootPose.local[index][column][row] -
                        rest.local[index][column][row]) > 0.0001f)
                    { std::cerr << "MMD step 6: untouched FBX joint changed\n"; return 6; }
    const auto pose0 = retargeter.Sample(0.f);
    const auto pose60 = retargeter.Sample(60.f);
    const auto pose120 = retargeter.Sample(120.f);
    const auto pose240 = retargeter.Sample(240.f);
    const float rootTravel = glm::distance(glm::vec3(pose0.world[rootNode][3]),
        glm::vec3(pose120.world[rootNode][3]));
    if (!std::isfinite(rootTravel) || rootTravel < 0.1f)
    { std::cerr << "MMD step 6: source root movement was lost\n"; return 6; }
    for (const int sourceIndex : { 18, 22, 154 })
    {
        const int target = entries.ArrayAt(sourceIndex)["fbxNode"].AsInt();
        const glm::mat4 relative0 = glm::inverse(pose0.world[rootNode]) *
            pose0.world[target];
        float movement = 0.f;
        for (const auto* sample : { &pose60, &pose120, &pose240 })
        {
            const glm::mat4 relative = glm::inverse(sample->world[rootNode]) *
                sample->world[target];
            movement = std::max(movement,
                glm::distance(glm::vec3(relative0[3]), glm::vec3(relative[3])) +
                glm::distance(glm::vec3(relative0 * glm::vec4(0.f, 1.f, 0.f, 0.f)),
                    glm::vec3(relative * glm::vec4(0.f, 1.f, 0.f, 0.f))));
        }
        if (!std::isfinite(movement) || movement < 0.02f)
        { std::cerr << "MMD step 6: real limb/torso pose did not move: "
                    << sourceIndex << '\n'; return 6; }
    }
    for (size_t index = 0; index < model.nodes.size(); ++index)
    {
        if (!std::isfinite(pose120.world[index][3].x) ||
            !std::isfinite(pose120.world[index][3].y) ||
            !std::isfinite(pose120.world[index][3].z))
        { std::cerr << "MMD step 6: nonfinite pose node " << index << '\n'; return 6; }
    }
    std::cout << "MMD step 6: neutral bind, root composition, inherited helper "
                 "rotations, FBX shoulder pose and frame-120 scene travel ("
              << rootTravel << " world units) verified.\n";
    Engine::Editor::VmdMotion curvedMotion;
    Engine::Editor::VmdBoneFrame center0;
    center0.name = bones.ArrayAt(1)["name"].AsString();
    center0.rotation = { 0.f, 0.f, 0.f, 1.f };
    auto center10 = center0;
    center10.frame = 10;
    center10.translation = { 10.f, 0.f, 0.f };
    center10.interpolation[8] = 127; // X x2; Y controls stay at zero.
    curvedMotion.bones.push_back(center0);
    curvedMotion.bones.push_back(center10);
    auto shoulder0 = rotationKey(150, 0.f, { 0.f, 0.f, 1.f });
    auto shoulder10 = rotationKey(150, 0.8f, { 0.f, 0.f, 1.f });
    shoulder10.frame = 10;
    shoulder10.interpolation[11] = 127; // Rotation x2.
    curvedMotion.bones.push_back(shoulder0);
    curvedMotion.bones.push_back(shoulder10);
    Engine::Editor::MmdPoseRetargeter curveConverter;
    if (!curveConverter.Initialize(model, rig, map, curvedMotion, error))
    { std::cerr << "MMD step 7: Bezier setup failed: " << error << '\n'; return 7; }
    const auto curvePose = curveConverter.Sample(5.f);
    if (std::abs(curvePose.sourceWorld[1][3].x - 1.25f) > 0.002f)
    { std::cerr << "MMD step 7: X Bezier position sample is incorrect\n"; return 7; }
    const glm::vec3 curvedShoulder = glm::vec3(curvePose.sourceWorld[150] *
        glm::vec4(1.f, 0.f, 0.f, 0.f));
    if (glm::distance(curvedShoulder,
            glm::vec3(std::cos(0.1f), std::sin(0.1f), 0.f)) > 0.002f)
    { std::cerr << "MMD step 7: rotation Bezier sample is incorrect\n"; return 7; }
    Engine::Editor::VmdMotion ikMotion;
    Engine::Editor::VmdBoneFrame legGoal;
    legGoal.name = bones.ArrayAt(223)["name"].AsString();
    legGoal.translation = { 0.5f, 0.f, 0.f };
    legGoal.rotation = { 0.f, 0.f, 0.f, 1.f };
    ikMotion.bones.push_back(legGoal);
    Engine::Editor::MmdPoseRetargeter ikConverter;
    if (!ikConverter.Initialize(model, rig, map, ikMotion, error))
    { std::cerr << "MMD step 7: IK setup failed: " << error << '\n'; return 7; }
    const auto ikPose = ikConverter.Sample(0.f);
    const float solvedDistance = glm::distance(
        glm::vec3(ikPose.sourceWorld[20][3]),
        glm::vec3(ikPose.sourceWorld[223][3]));
    if (ikPose.ikError.size() != 4 || solvedDistance > 0.15f)
    { std::cerr << "MMD step 7: left ankle IK missed target by "
                << solvedDistance << '\n'; return 7; }
    Engine::Editor::VmdPropertyFrame ikOff;
    ikOff.visible = false;
    ikOff.ik.push_back({ legGoal.name, false });
    ikMotion.properties.push_back(ikOff);
    Engine::Editor::MmdPoseRetargeter disabledIkConverter;
    if (!disabledIkConverter.Initialize(model, rig, map, ikMotion, error))
    { std::cerr << "MMD step 7: IK switch setup failed: " << error << '\n'; return 7; }
    const auto disabledIkPose = disabledIkConverter.Sample(0.f);
    const float disabledDistance = glm::distance(
        glm::vec3(disabledIkPose.sourceWorld[20][3]),
        glm::vec3(disabledIkPose.sourceWorld[223][3]));
    if (disabledDistance < solvedDistance + 0.15f)
    { std::cerr << "MMD step 7: IK off switch was ignored\n"; return 7; }
    const auto hiddenClip = disabledIkConverter.BakeBody(0, "hidden");
    if (hiddenClip.visibility.size() != 1 ||
        hiddenClip.visibility.front().visible ||
        hiddenClip.Serialize()["visibility"].ArraySize() != 1)
    { std::cerr << "MMD step 7: visibility key was lost\n"; return 7; }
    std::cout << "MMD step 7: bone Bezier and synthetic IK switch/target checks passed.\n";
    std::unordered_set<std::string> mappedIk;
    for (size_t index = 0; index < bones.ArraySize(); ++index)
        if (bones.ArrayAt(index).Has("ik"))
            mappedIk.insert(bones.ArrayAt(index)["name"].AsString());
    size_t unsupportedIk = 0;
    for (const auto& setting : dance.properties.front().ik)
        if (mappedIk.find(setting.name) == mappedIk.end()) ++unsupportedIk;
    if (mappedIk.size() != 4 || unsupportedIk != 5)
    { std::cerr << "MMD step 7: IK switch coverage changed\n"; return 7; }
    std::cout << "MMD step 7: 4 mapped IK chains; " << unsupportedIk
              << " source switches have no PMX control.\n";
    std::array<float, 4> worstIk {};
    std::array<int, 4> worstIkFrame {};
    std::array<float, 4> worstDrift {};
    std::array<int, 4> worstDriftFrame {};
    std::array<float, 4> worstOriginal {};
    const std::array<std::pair<int, int>, 6> legSegments {{
        {18, 19}, {19, 20}, {20, 21},
        {218, 219}, {219, 220}, {220, 221}
    }};
    for (int frame = 0; frame <= 322; ++frame)
    {
        const auto sampled = retargeter.Sample(static_cast<float>(frame));
        for (const auto& [upper, lower] : legSegments)
        {
            const float restLength = glm::distance(
                glm::vec3(rest.sourceWorld[upper][3]),
                glm::vec3(rest.sourceWorld[lower][3]));
            const float posedLength = glm::distance(
                glm::vec3(sampled.sourceWorld[upper][3]),
                glm::vec3(sampled.sourceWorld[lower][3]));
            if (std::abs(restLength - posedLength) > 0.001f)
            { std::cerr << "MMD step 7: IK changed leg/toe length at frame "
                        << frame << '\n'; return 7; }
        }
        for (const auto& [thigh, knee] :
            { std::pair<int, int>{18, 19}, {218, 219} })
        {
            const glm::mat4 local = glm::inverse(sampled.sourceWorld[thigh]) *
                sampled.sourceWorld[knee];
            const float kneeX = glm::eulerAngles(glm::quat_cast(glm::mat3(local))).x;
            if (kneeX > 0.05f || kneeX < -3.2f)
            { std::cerr << "MMD step 7: knee exceeded hinge limit at frame "
                        << frame << " (" << kneeX << ")\n"; return 7; }
        }
        for (size_t chain = 0; chain < worstIk.size(); ++chain)
        {
            if (sampled.ikError[chain] > worstIk[chain])
            { worstIk[chain] = sampled.ikError[chain]; worstIkFrame[chain] = frame; }
            if (sampled.ikTargetDrift[chain] > worstDrift[chain])
            { worstDrift[chain] = sampled.ikTargetDrift[chain]; worstDriftFrame[chain] = frame; }
            worstOriginal[chain] = std::max(worstOriginal[chain],
                sampled.ikOriginalError[chain]);
        }
    }
    const auto worstLeft = retargeter.Sample(static_cast<float>(worstDriftFrame[0]));
    const float leftLegReach = glm::distance(
        glm::vec3(worstLeft.sourceWorld[18][3]),
        glm::vec3(worstLeft.sourceWorld[19][3])) +
        glm::distance(glm::vec3(worstLeft.sourceWorld[19][3]),
            glm::vec3(worstLeft.sourceWorld[20][3]));
    const float leftGoalDistance = glm::distance(
        glm::vec3(worstLeft.sourceWorld[18][3]),
        glm::vec3(worstLeft.sourceWorld[223][3]));
    if (*std::max_element(worstIk.begin(), worstIk.end()) > 0.15f ||
        *std::max_element(worstDrift.begin(), worstDrift.end()) > 0.75f ||
        *std::max_element(worstOriginal.begin(), worstOriginal.end()) > 0.75f)
    {
        std::cerr << "MMD step 7: reachable-target IK error exceeds 0.15 "
                     "PMX units or original-goal error/drift exceeds 0.75. Worst errors:";
        for (size_t chain = 0; chain < worstIk.size(); ++chain)
            std::cerr << ' ' << worstIk[chain] << '@' << worstIkFrame[chain];
        std::cerr << ". Worst target drifts:";
        for (size_t chain = 0; chain < worstDrift.size(); ++chain)
            std::cerr << ' ' << worstDrift[chain] << '@' << worstDriftFrame[chain];
        std::cerr << ". Worst original-goal errors:";
        for (float residual : worstOriginal) std::cerr << ' ' << residual;
        std::cerr << ". Left leg at frame " << worstDriftFrame[0]
                  << " needs " << leftGoalDistance << " units of reach but has "
                  << leftLegReach << ".\n";
        return 7;
    }
    std::cout << "MMD step 7: reachable IK error under 0.15 PMX units; "
                 "maximum target drift "
              << *std::max_element(worstDrift.begin(), worstDrift.end())
              << ", original-goal error "
              << *std::max_element(worstOriginal.begin(), worstOriginal.end()) << ".\n";
    const auto baked = retargeter.BakeBody(dance.lastFrame, "dance1");
    Memory("after body bake");
    if (baked.channels.empty() || baked.duration < 10.f ||
        baked.duration > 11.f)
    { std::cerr << "MMD step 7: body clip is empty or has wrong duration\n";
        return 7; }
    if (baked.visibility.size() != dance.properties.size() ||
        baked.visibility.front().visible != dance.properties.front().visible)
    { std::cerr << "MMD step 7: dance visibility mismatch\n"; return 7; }
    float worstPosition = 0.f, worstAngle = 0.f;
    for (const auto& channel : baked.channels)
    {
        if (channel.targetPath.empty() ||
            channel.times.size() * channel.valueWidth != channel.values.size())
        { std::cerr << "MMD step 7: malformed FK channel\n"; return 7; }
        if (channel.path == Engine::Model::AnimationChannel::Path::Rotation)
            for (size_t key = 1; key < channel.times.size(); ++key)
            {
                const size_t a = (key - 1) * 4, b = key * 4;
                const float dot = channel.values[a] * channel.values[b] +
                    channel.values[a + 1] * channel.values[b + 1] +
                    channel.values[a + 2] * channel.values[b + 2] +
                    channel.values[a + 3] * channel.values[b + 3];
                if (dot < 0.f)
                { std::cerr << "MMD step 7: quaternion sign discontinuity\n";
                    return 7; }
            }
    }
    // Check a spread of exact baked samples before interpolation is verified.
    for (int quarter = 1; quarter < static_cast<int>(dance.lastFrame) * 4;
        quarter += 8)
    {
        const float frame = static_cast<float>(quarter) / 4.f;
        const auto expected = retargeter.Sample(frame);
        const size_t before = static_cast<size_t>(quarter);
        const size_t after = before + 1;
        for (const auto& channel : baked.channels)
        {
            const size_t node = channel.nodeIndex;
            if (before >= channel.times.size() || after >= channel.times.size())
                continue;
            if (channel.path == Engine::Model::AnimationChannel::Path::Translation)
            {
                const glm::vec3 value(
                    channel.values[before * 3],
                    channel.values[before * 3 + 1],
                    channel.values[before * 3 + 2]);
                worstPosition = std::max(worstPosition,
                    glm::distance(value, glm::vec3(expected.local[node][3])));
            }
            if (channel.path == Engine::Model::AnimationChannel::Path::Rotation)
            {
                const size_t offset = before * 4;
                const glm::quat value(channel.values[offset + 3],
                    channel.values[offset], channel.values[offset + 1],
                    channel.values[offset + 2]);
                glm::vec3 scale, position, skew;
                glm::vec4 perspective;
                glm::quat target;
                if (!glm::decompose(expected.local[node], scale, target,
                        position, skew, perspective)) return 7;
                worstAngle = std::max(worstAngle,
                    2.f * std::acos(std::clamp(
                        std::abs(glm::dot(value, glm::normalize(target))),
                        0.f, 1.f)));
            }
        }
    }
    if (worstPosition > 0.001f || worstAngle > 0.005f)
    { std::cerr << "MMD step 7: baked pose reconstruction exceeded "
                << worstPosition << " units, " << worstAngle << " radians\n";
        return 7; }
    float worstInterpolatedPosition = 0.f;
    float worstInterpolatedAngle = 0.f;
    for (int quarter = 0; quarter < static_cast<int>(dance.lastFrame) * 4;
        quarter += 32)
    {
        const float frame = (static_cast<float>(quarter) + .5f) / 4.f;
        const auto expected = retargeter.Sample(frame);
        for (const auto& channel : baked.channels)
        {
            const size_t node = channel.nodeIndex;
            const size_t before = static_cast<size_t>(quarter) * channel.valueWidth;
            const size_t after = before + channel.valueWidth;
            if (after + channel.valueWidth > channel.values.size()) continue;
            if (channel.path == Engine::Model::AnimationChannel::Path::Translation)
            {
                const glm::vec3 a(channel.values[before],
                    channel.values[before + 1], channel.values[before + 2]);
                const glm::vec3 b(channel.values[after],
                    channel.values[after + 1], channel.values[after + 2]);
                worstInterpolatedPosition = std::max(worstInterpolatedPosition,
                    glm::distance(glm::mix(a, b, .5f),
                        glm::vec3(expected.local[node][3])));
            }
            else if (channel.path == Engine::Model::AnimationChannel::Path::Rotation)
            {
                const glm::quat a(channel.values[before + 3],
                    channel.values[before], channel.values[before + 1],
                    channel.values[before + 2]);
                const glm::quat b(channel.values[after + 3],
                    channel.values[after], channel.values[after + 1],
                    channel.values[after + 2]);
                glm::vec3 scale, position, skew;
                glm::vec4 perspective;
                glm::quat target;
                if (!glm::decompose(expected.local[node], scale, target,
                        position, skew, perspective)) return 7;
                worstInterpolatedAngle = std::max(worstInterpolatedAngle,
                    2.f * std::acos(std::clamp(std::abs(glm::dot(
                        glm::normalize(glm::slerp(a, b, .5f)),
                        glm::normalize(target))), 0.f, 1.f)));
            }
        }
    }
    if (worstInterpolatedPosition > .1f || worstInterpolatedAngle > .1f)
    { std::cerr << "MMD step 7: between-sample error exceeded "
                << worstInterpolatedPosition << " units, "
                << worstInterpolatedAngle << " radians\n"; return 7; }
    std::cout << "MMD step 7: baked " << baked.channels.size()
              << " FK channels; between-sample error "
              << worstInterpolatedPosition << " units / "
              << worstInterpolatedAngle << " radians.\n";
    const auto facial = retargeter.BakeFacial(dance.lastFrame);
    Memory("after facial bake");
    std::unordered_set<std::string> faceMeshes;
    for (const auto& channel : facial.clip.channels)
    {
        const bool duplicate = !faceMeshes.insert(channel.targetMesh).second;
        if (channel.path != Engine::Model::AnimationChannel::Path::Weights ||
            channel.targetMesh.empty() || channel.targetPath.empty() ||
            channel.valueWidth != 86 ||
            channel.times.size() * channel.valueWidth != channel.values.size() ||
            duplicate)
        { std::cerr << "MMD step 8: facial channel lost mesh ownership: "
                    << channel.targetMesh << " width=" << channel.valueWidth
                    << " keys=" << channel.times.size() << " duplicate="
                    << duplicate << '\n';
            return 8; }
    }
    std::cout << "MMD step 8: " << facial.matchedExpressions
              << " matching expressions on " << facial.matchedTargets
              << " targets across " << faceMeshes.size() << " meshes; "
              << facial.unmatchedExpressions.size() << " source expressions "
                 "unsupported.\n";
    Engine::Model::AnimationClip restoredFacial;
    restoredFacial.Deserialize(facial.clip.Serialize());
    bool animated = false;
    for (const auto& channel : restoredFacial.channels)
        for (size_t offset = channel.valueWidth;
            offset < channel.values.size(); ++offset)
            if (std::abs(channel.values[offset] -
                    channel.values[offset % channel.valueWidth]) > 1e-4f)
                animated = true;
    if (faceMeshes.size() != 4 || facial.matchedExpressions != 40 ||
        facial.matchedTargets != 160 ||
        facial.unmatchedExpressions.size() != 94 ||
        restoredFacial.channels.size() != 4 || !animated)
    { std::cerr << "MMD step 8: facial coverage is incomplete\n";
        return 8; }
    std::cout << "MMD step 8: morph names, per-mesh channels and animated "
                 "weights survive clip serialization.\n";
    const bool exportDemo = std::getenv("MMD_EXPORT_DEMO") != nullptr;
    const fs::path output = exportDemo ? fs::path("Engine/Core/Assets/mmd")
        : fs::path(".local/test-output/mmd-step9");
    fs::create_directories(output);
    // Exercise the complete FBX package, including its morph geometry.
    auto bindingModel = std::move(model);
    if (exportDemo) bindingModel.name = "Mococo Dance";
    const auto imported = Engine::Editor::ModelAssetWriter::Write(
        bindingModel, source.string(), output.string());
    if (!imported.success)
    { std::cerr << "MMD step 9: prefab import failed: " << imported.message << '\n';
        return 9; }
    const fs::path prefab = imported.prefabPath;
    const fs::path bodyPath = prefab.parent_path() / "dance1_body.anim";
    const fs::path facePath = prefab.parent_path() / "dance1_facial.anim";
    const fs::path reportPath = prefab.parent_path() / "dance1_mmd.json";
    const fs::path packPath = prefab.parent_path() / "dance1.animpack";
    const auto bind = [&]()
    {
        return Engine::Editor::MmdDanceAssetBinder::BindBaked(
            prefab.string(), vmd.string(), profile.string(), mapping.string(),
            baked, facial, error);
    };
    if (!bind() || !bind())
    { std::cerr << "MMD step 9: binding failed: " << error << '\n'; return 9; }
    {
    const auto savedBody = Engine::Model::AnimationClipAsset::Load(bodyPath.string());
    const auto savedFace = Engine::Model::AnimationClipAsset::Load(facePath.string());
    const auto bindingReport = Engine::Serialization::JsonParseFile(reportPath.string());
    Engine::Scene::Scene boundScene;
    auto* boundRoot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        boundScene, prefab.string());
    auto* manager = boundRoot ? boundRoot->GetComponent<Engine::Components::AnimationManager>() : nullptr;
    size_t meshCount = 0, morphMeshCount = 0, boundSkinnedMeshes = 0;
    for (const auto& object : boundScene.GetObjects())
    {
        if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
        { ++meshCount; if (mesh->HasMorphTargets()) ++morphMeshCount; }
        if (auto* skinned = object->GetComponent<Engine::Components::SkinnedMesh>())
            boundSkinnedMeshes += skinned->ResolveSkeleton() != nullptr;
    }
    size_t skeletonCount = 0;
    for (auto* component : boundRoot->Components)
        skeletonCount += dynamic_cast<Engine::Components::Skeleton*>(component) != nullptr;
    if (!savedBody || !savedFace || !manager ||
        meshCount != 56 || morphMeshCount != 4 || skeletonCount != bindingModel.skins.size() ||
        boundSkinnedMeshes != 56 ||
        savedBody->clip.channels.size() != baked.channels.size() ||
        savedFace->clip.channels.size() != facial.clip.channels.size() ||
        savedBody->previewPrefab != prefab.generic_string() ||
        bindingReport["unmatchedExpressions"].ArraySize() != 94 ||
        bindingReport["retargetMap"].AsString() != mapping.string())
    { std::cerr << "MMD step 9: saved assets or report incomplete\n"; return 9; }
    manager->RefreshRigClips();
    if (manager->clip != baked.clipName ||
        manager->layers.size() != 1 ||
        manager->layers.front().clip != facial.clip.clipName ||
        !manager->FindClip(baked.clipName) ||
        !manager->FindClip(facial.clip.clipName))
    { std::cerr << "MMD step 9: prefab playback binding incomplete\n"; return 9; }
    for (auto* component : boundRoot->Components)
        if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
        {
            const auto rigAsset = Engine::Model::RigAsset::Load(
                skeleton->rigPath, skeleton->skinIndex);
            if (!rigAsset || std::count(rigAsset->animationPaths.begin(),
                    rigAsset->animationPaths.end(), bodyPath.generic_string()) != 1 ||
                std::count(rigAsset->animationPaths.begin(),
                    rigAsset->animationPaths.end(), facePath.generic_string()) != 1)
            { std::cerr << "MMD step 9: rig clips duplicated or missing\n"; return 9; }
        }
    std::cout << "MMD step 9: body and facial assets saved, bound and "
                 "rebound without duplicates.\n";
    }
    std::string sourcePackPath;
    std::string sourceImportDirectory;
    if (!exportDemo)
    {
        const auto sourceImport = Engine::Editor::VmdSceneImporter::Import(
            vmd.string(), output.string());
        if (!sourceImport.success)
        { std::cerr << "MMD step 9: source VMD pack import failed: "
                    << sourceImport.message << '\n'; return 9; }
        sourceImportDirectory = sourceImport.outputDirectory;
        sourcePackPath = (fs::path(sourceImportDirectory) / "dance1.animpack").string();
    }
    if (!Engine::Editor::MmdDanceAssetBinder::BindFromPrefab(
            prefab.string(), vmd.string(), error, sourcePackPath))
    { std::cerr << "MMD step 9: metadata-driven VMD import failed: "
                << error << '\n'; return 9; }
    Memory("after metadata bind");
    {
    const auto autoBody = Engine::Model::AnimationClipAsset::Load(bodyPath.string());
    const auto autoFace = Engine::Model::AnimationClipAsset::Load(facePath.string());
    const auto pack = Engine::Model::AnimationPackAsset::Load(packPath.string(), error);
    Memory("after pack load");
    if (!autoBody || !autoFace || autoBody->clip.channels.empty() ||
        autoFace->clip.channels.empty() || !pack || pack->clips.size() != 2 ||
        pack->layers.size() != 1 ||
        (!exportDemo && pack->sourceTracks.size() != 3))
    { std::cerr << "MMD step 9: automatic import did not create separate .anim files\n";
        return 9; }
    Engine::Scene::Scene packScene;
    auto* packRoot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        packScene, prefab.string());
    auto* packManager = packRoot
        ? packRoot->GetComponent<Engine::Components::AnimationManager>() : nullptr;
    if (!packManager || !pack->Apply(*packManager, error) ||
        packManager->clips.size() != 2 || packManager->layers.size() != 1 ||
        !packManager->rigPaths.empty() ||
        packManager->Serialize()["clips"].ArraySize() != 2)
    { std::cerr << "MMD step 9: animation pack round-trip failed: "
                << error << '\n'; return 9; }
    const fs::path reexportedPack = prefab.parent_path() / "roundtrip.animpack";
    if (!Engine::Model::AnimationPackAsset::Capture(*packManager).Save(
            reexportedPack.string(), error) ||
        !Engine::Model::AnimationPackAsset::Load(
            reexportedPack.string(), error))
    { std::cerr << "MMD step 9: animation stack export failed: "
                << error << '\n'; return 9; }
    Memory("after pack round-trip");
    }
    std::cout << "MMD step 9: selecting imported FBX resolves the retarget map "
                 "and writes separate .anim files plus a stacked .animpack.\n";
    const fs::path scenePath = exportDemo
        ? fs::path("Engine/Core/Assets/Scenes/Animation/mmd.scene")
        : output / "mmd.scene";
    {
    if (!SaveMmdDemo(prefab, scenePath))
    { std::cerr << "MMD step 10: scene setup failed\n"; return 10; }
    std::cout << "MMD step 10: saved and reloaded scene with character, "
                 "floor, camera and light.\n";
    }
    Memory("before single-track packs");
    if (!exportDemo)
    {
        Engine::Model::AnimationClip emptyBody;
        Engine::Editor::MmdPoseRetargeter::FacialBake emptyFace;
        const fs::path bodyOnlyVmd = output / "body-only.vmd";
        const fs::path faceOnlyVmd = output / "face-only.vmd";
        if (!Engine::Editor::MmdDanceAssetBinder::BindBaked(
                prefab.string(), bodyOnlyVmd.string(), profile.string(),
                mapping.string(), baked, emptyFace, error) ||
            !fs::exists(prefab.parent_path() / "body-only_body.anim") ||
            fs::exists(prefab.parent_path() / "body-only_facial.anim") ||
            !Engine::Editor::MmdDanceAssetBinder::BindBaked(
                prefab.string(), faceOnlyVmd.string(), profile.string(),
                mapping.string(), emptyBody, facial, error) ||
            !fs::exists(prefab.parent_path() / "face-only_facial.anim") ||
            fs::exists(prefab.parent_path() / "face-only_body.anim"))
        { std::cerr << "MMD step 9: single-track VMD binding failed: "
                    << error << '\n'; return 9; }
        const auto bodyPack = Engine::Model::AnimationPackAsset::Load(
            (prefab.parent_path() / "body-only.animpack").string(), error);
        const auto facePack = Engine::Model::AnimationPackAsset::Load(
            (prefab.parent_path() / "face-only.animpack").string(), error);
        if (!bodyPack || !facePack || bodyPack->clips.size() != 1 ||
            facePack->clips.size() != 1 || !facePack->layers.empty())
        { std::cerr << "MMD step 9: single-track animation packs are invalid: "
                    << error << '\n'; return 9; }
        std::cout << "MMD step 9: body-only and face-only motions save only "
                     "their matching .anim asset.\n";
    }
    if (!exportDemo &&
        fs::path(imported.outputDirectory).parent_path().lexically_normal() ==
            output.lexically_normal())
    {
        fs::remove_all(imported.outputDirectory);
        if (!sourceImportDirectory.empty() &&
            fs::path(sourceImportDirectory).parent_path().lexically_normal() ==
                output.lexically_normal())
            fs::remove_all(sourceImportDirectory);
        fs::remove(scenePath);
    }
    std::cout << "MMD import steps 1-10 passed.\n";
    return 0;
}
