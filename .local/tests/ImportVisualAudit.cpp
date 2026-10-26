#include "Editor/Core/Importers/FbxModelDecoder.h"
#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Editor/Core/Importers/MmdDanceAssetBinder.h"
#include "Editor/Core/Importers/VmdMotionDecoder.h"
#include "Core/AssetRecord.h"
#include "Core/Model/RigAsset.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/SceneSerializerInternal.h"
#include <pugixml.hpp>
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Window.h"
#include <Windows.h>
#include <ufbx.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace Engine;
const fs::path output = fs::absolute(".local/test-output/import-visual-audit");

void SaveBmp(const std::vector<uint8_t>& pixels, const fs::path& path)
{
    constexpr unsigned w = 640, h = 800;
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + w*h*4;
    info.biSize = sizeof(info); info.biWidth = w; info.biHeight = -int(h);
    info.biPlanes = 1; info.biBitCount = 32;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    auto bgra = pixels;
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i+2]);
    out.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
}

std::vector<uint8_t> Capture(Renderers::DX11GameRenderer& renderer, const std::string& prefab,
    const std::string& prefix, glm::vec3 center, float height, float rootScale = 0.f,
    const std::vector<int>& frames = { -1, 0, 60, 100, 150, 210, 300, 450 },
    const std::string& motion = {}, bool disableIk = false)
{
    Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider());
    auto* root = Serialization::SceneSerializer::InstantiatePrefab(scene, prefab, renderer.GetGraphicsProvider());
    if (!root) throw std::runtime_error("Failed to load " + prefab);
    if (rootScale > 0) root->transform.scale = glm::vec3(rootScale);
    if (!motion.empty())
    {
        auto* animation = root->GetComponent<Components::AnimationManager>();
        if (!animation) animation = root->AddComponent<Components::AnimationManager>();
        std::string error;
        const bool added = fs::path(motion).extension() == ".anim"
            ? Editor::MmdDanceAssetBinder::AddClipForPrefab(prefab, motion, *animation, error)
            : Editor::MmdDanceAssetBinder::ApplyForPrefab(prefab, motion, *animation, error);
        if (!added) throw std::runtime_error(error);
        animation->layers.clear();
        animation->timelinePlayback = false;
        animation->baseAnimationEnabled = true;
    }
    if (disableIk)
        if (auto* constraints = root->GetComponent<Components::RigConstraints>())
            for (auto& chain : constraints->ikChains) chain.enabled = false;
    auto* light = scene.AddObject("Audit light")->AddComponent<Components::Light>();
    light->lightType = int(Components::Light::Type::Directional);
    light->intensity = 2.f; light->castsShadows = false;
    auto* cameraObject = scene.AddObject("Audit camera");
    auto* camera = cameraObject->AddComponent<Components::Camera>();
    camera->useTransformRotation = false; camera->target = center;
    camera->orthographic = true; camera->orthographicSize = height * 1.15f;
    cameraObject->transform.position = center + glm::vec3(0, height * .05f, height * 2.f);
    camera->farPlane = 10000.f;
    scene.Start();
    auto* manager = root->GetComponent<Components::AnimationManager>();
    const std::string selectedClip = manager ? manager->clip : std::string();
    std::vector<uint8_t> firstFrame;
    std::cout << prefix << " selected clip: " << selectedClip << '\n' << std::flush;
    for (int frame : frames)
    {
        if (manager)
        {
            manager->playing = false; manager->looping = false;
            if (frame < 0) manager->clip = "__audit_bind_pose__";
            else if (!manager->clips.empty())
            {
                manager->clip = selectedClip;
                manager->time = frame / 30.f;
                for (auto& layer : manager->layers) layer.time = manager->time;
            }
        }
        scene.Update(0.f);
        if (manager && frame >= 0)
        {
            const auto* clip = manager->FindClip(selectedClip);
            if (!clip) throw std::runtime_error("Capture clip is missing: " + selectedClip);
            std::cout << prefix << " frame " << frame << " channels " << clip->channels.size() << '\n';
        }
        scene.PrepareRenderFrame();
        renderer.BeginExportFrame(1); renderer.BeginFrame();
        auto context = renderer.CreateFrameGraphicsContext();
        scene.Render(context.get(), .8f, camera, false, 640, 800, true);
        renderer.Clear(.08f, .09f, .11f);
        scene.Render(context.get(), .8f, camera, false, 640, 800, false);
        renderer.EndFrame();
        std::vector<uint8_t> pixels;
        if (!renderer.ReadExportFrameRGBA(pixels, true)) throw std::runtime_error("Readback failed");
        const auto path = output / (prefix + "-" + std::to_string(frame) + ".bmp");
        SaveBmp(pixels, path);
        if (firstFrame.empty()) firstFrame = pixels;
        std::cout << "Captured " << path << " draws " << scene.GetLastOrdinaryDrawCount() << '\n' << std::flush;
    }
    return firstFrame;
}

Model::ImportedModel Reference(const Model::ImportedModel& imported, const ufbx_scene& scene,
    glm::vec3& low, glm::vec3& high)
{
    Model::ImportedModel model;
    model.name = "Roku Source Reference"; model.sourceFormat = "fbx";
    model.materials = imported.materials; model.textures = imported.textures;
    low = glm::vec3(1e20f); high = glm::vec3(-1e20f);
    for (const auto* node : scene.nodes)
    {
        const auto* mesh = node->mesh;
        if (!mesh) continue;
        const auto& positions = mesh->skinned_position.exists ? mesh->skinned_position : mesh->vertex_position;
        for (size_t material = 0; material < std::max(size_t(1), mesh->materials.count); ++material)
        {
            Model::ImportedPrimitive primitive;
            primitive.name = std::string(node->name.data, node->name.length) + " " + std::to_string(material);
            if (material < mesh->materials.count)
            {
                for (unsigned i = 0; i < model.materials.size(); ++i)
                    if (model.materials[i].name == mesh->materials.data[material]->name.data)
                        primitive.materialIndex = int(i);
                if (primitive.materialIndex < 0) throw std::runtime_error("Source reference material did not match");
            }
            for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
            {
                const auto face = mesh->faces.data[faceIndex];
                if (face.num_indices < 3 || (mesh->face_material.count && mesh->face_material.data[faceIndex] != material)) continue;
                std::vector<uint32_t> indices(face.num_indices * 3);
                const auto triangles = ufbx_triangulate_face(indices.data(), indices.size(), mesh, face);
                for (size_t i = 0; i < triangles * 3; ++i)
                {
                    const unsigned index = indices[i];
                    auto p = ufbx_get_vertex_vec3(&positions, index);
                    if (mesh->skinned_is_local) p = ufbx_transform_position(&node->geometry_to_world, p);
                    const glm::vec3 point(float(p.x), float(p.y), float(p.z));
                    low = glm::min(low, point); high = glm::max(high, point);
                    Model::AnimationVertex vertex{};
                    vertex.pos[0] = point.x; vertex.pos[1] = point.y; vertex.pos[2] = point.z;
                    if (mesh->vertex_normal.exists)
                    {
                        auto n = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
                        n = ufbx_transform_direction(&node->geometry_to_world, n);
                        const auto normal = glm::normalize(glm::vec3(float(n.x), float(n.y), float(n.z)));
                        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
                    }
                    if (mesh->vertex_uv.exists)
                    {
                        const auto uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                        vertex.uv[0] = float(uv.x); vertex.uv[1] = 1.f-float(uv.y);
                    }
                    vertex.tangent[0] = vertex.tangent[3] = 1.f;
                    if (mesh->vertex_color.exists)
                    {
                        const auto color = ufbx_get_vertex_vec4(&mesh->vertex_color, index);
                        vertex.color[0] = float(color.x); vertex.color[1] = float(color.y);
                        vertex.color[2] = float(color.z); vertex.color[3] = float(color.w);
                    }
                    primitive.vertices.push_back(vertex);
                }
            }
            if (primitive.vertices.empty()) continue;
            Model::ImportedNode object; object.name = primitive.name;
            object.primitives.push_back(unsigned(model.primitives.size()));
            model.nodes.push_back(std::move(object)); model.primitives.push_back(std::move(primitive));
        }
    }
    return model;
}

int main(int argc, char** argv)
{
    try
    {
        fs::create_directories(output);
        if (argc > 1 && std::string(argv[1]) == "repair-dance-rig")
        {
            const auto target = fs::path("Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/Mococo Dance.rig");
            const auto backup = fs::path(".local/backups") / ("Mococo-Dance-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".rig");
            fs::create_directories(backup.parent_path());
            fs::copy_file(target, backup);
            const auto bytes = Serialization::AssetWriteBinary(Serialization::JsonParseFile(".local/test-output/recovered-dance-source.json"));
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            file.write(bytes.data(), bytes.size());
            if (!file) throw std::runtime_error("Could not restore rig asset");
            std::cout << "Repaired rig paths. Backup: " << backup << '\n';
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "open-dance")
        {
            Scene::Scene scene;
            pugi::xml_document xml;
            if (!xml.load_file("Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/Mococo Dance.prefab"))
                throw std::runtime_error("Could not read dance prefab XML");
            const auto json = Serialization::Detail::SceneXmlBehavior::ReadNode(xml.child("Prefab"));
            std::unordered_map<std::string, Serialization::SceneSerializer::Factory> registry;
            for (const auto& name : Serialization::SceneSerializer::GetRegisteredComponentTypes())
                registry[name] = Serialization::SceneSerializer::GetRegisteredFactory(name);
            auto* object = Serialization::Detail::SceneObjectBehavior::DeserializeObjectTree(scene, json["object"], nullptr, registry);
            std::cout << "Opened " << (object ? object->name : "null") << '\n';
            return object ? 0 : 1;
        }
        if (argc > 1 && std::string(argv[1]) == "repair-dance")
        {
            const std::string bodyPath = "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/dance1_body.anim";
            const std::string sourcePrefab = "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/Mococo Dance.prefab";
            auto asset = Model::AnimationClipAsset::Load(".local/test-output/dance1-source-bind.anim");
            if (!asset) throw std::runtime_error("Recovered original animation is unreadable");
            if (asset->bindSkeleton.size() != 326) throw std::runtime_error("Recovered neutral FBX skeleton is incomplete");
            asset->previewPrefab = sourcePrefab;
            const auto backup = fs::path(".local/backups") / ("dance1_body-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".anim");
            fs::create_directories(backup.parent_path());
            fs::copy_file(bodyPath, backup);
            if (!asset->Save(bodyPath)) throw std::runtime_error("Could not save repaired animation");
            std::cout << "Restored self-contained source bind data; backup: " << backup << '\n';
            return 0;
        }
        Core::Window window(GetModuleHandleW(nullptr), L"Hidden import audit", 640, 800);
        Renderers::DX11GameRenderer renderer;
        if (!renderer.Init(window.GetHWND(), 640, 800) || !renderer.EnableOffscreenExport(640,800,1,2))
            throw std::runtime_error("DX11 audit initialization failed");
        std::string error;
        if (argc > 1 && std::string(argv[1]) == "dance1-bind")
        {
            const std::string prefab = "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.low.prefab";
            const std::string body = ".local/test-output/dance1-source-bind.anim";
            Capture(renderer, prefab, "dance1-bind", {0,.85f,0}, 1.9f, .08f, {0,60,150}, body);
            Capture(renderer, prefab, "dance1-bind-noik", {0,.85f,0}, 1.9f, .08f, {0,60,150}, body, true);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "dance1-fixed")
        {
            Capture(renderer, "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.low.prefab",
                "dance1-fixed", {0,.85f,0}, 1.9f, .08f, {0,60,150},
                "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/dance1_body.anim");
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "dance1")
        {
            const std::string prefab = "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.low.prefab";
            const std::string body = "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/dance1_body.anim";
            Capture(renderer, prefab, "dance1-anim", {0,.85f,0}, 1.9f, .08f, {-1,0,60,150}, body);
            Capture(renderer, prefab, "dance1-anim-noik", {0,.85f,0}, 1.9f, .08f, {0,60,150}, body, true);
            Capture(renderer, prefab, "dance1-native", {0,.85f,0}, 1.9f, .08f, {0,60,150}, "Engine/Core/Assets/Models/mmd/dance1.vmd");
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "saved")
        {
            Capture(renderer, "Engine/Core/Assets/Roku_V1_packed/Roku_V1_packed.prefab",
                "roku-saved", {-.00054f,.88035f,-.66456f}, 1.7589f, 0.f, {-1,60,210});
            Capture(renderer, "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.low.prefab",
                "mococo-saved", {0,.85f,0}, 1.9f, .08f, {150,210,300,450});
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "mococo")
        {
            const std::string original = "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.low.prefab";
            Capture(renderer, original, "mococo-before", {0,.85f,0}, 1.9f, .08f);
            Scene::Scene staged;
            auto* root = Serialization::SceneSerializer::InstantiatePrefab(staged, original);
            if (!root) throw std::runtime_error("No Mococo prefab");
            auto* skeleton = root->GetComponent<Components::Skeleton>();
            auto rig = Model::RigAsset::Load(skeleton->rigPath);
            if (!rig) throw std::runtime_error("No Mococo rig");
            const auto prefab = output / "Mococo Audit.prefab";
            const auto rigPath = output / "Mococo Audit.rig";
            rig->prefabPath = prefab.string(); rig->Save(rigPath.string());
            skeleton->rigPath = rigPath.string();
            auto* manager = root->GetComponent<Components::AnimationManager>();
            manager->rigPaths = { rigPath.string() };
            Serialization::SceneSerializer::SavePrefab(*root, prefab.string());
            Core::AssetRecord::Ensure(prefab, "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard.pmx");
            if (!Editor::MmdDanceAssetBinder::BindFromPrefab(prefab.string(),
                "Engine/Core/Assets/Models/mmd/sad cat dance/dancing.vmd", error)) throw std::runtime_error(error);
            auto body = Model::AnimationClipAsset::Load((output/"dancing_body.anim").string());
            if (!body || body->clip.channels.size() < 30) throw std::runtime_error("Sad Cat Dance body tracks did not bind");
            Editor::VmdMotion motion;
            if (!Editor::VmdMotionDecoder::DecodeFile("Engine/Core/Assets/Models/mmd/sad cat dance/dancing.vmd", motion, error))
                throw std::runtime_error(error);
            const auto profile = Serialization::JsonParseFile("Engine/Core/Assets/Models/mmd/Mococo/Mococo.mmdrig.json");
            size_t verifiedKeys = 0;
            const auto* targetModel = root->GetComponent<Components::Model>();
            for (const auto& frame : motion.bones)
                for (size_t bone = 0; bone < profile["bones"].ArraySize(); ++bone)
                    if (profile["bones"].ArrayAt(bone)["name"].AsString() == frame.name)
                    {
                        const auto node = skeleton->jointNodes.at(bone);
                        const auto* target = targetModel->ResolveNode(node);
                        for (const auto& channel : body->clip.channels)
                            if (channel.nodeIndex == node && channel.path == Model::AnimationChannel::Path::Rotation)
                            {
                                const auto key = std::lower_bound(channel.times.begin(), channel.times.end(), frame.timeSeconds - 1e-5f);
                                if (key == channel.times.end() || std::abs(*key-frame.timeSeconds) > 1e-4f)
                                    throw std::runtime_error("Missing authored VMD bone key");
                                const auto offset = (key-channel.times.begin())*4;
                                const auto& v = channel.values;
                                const glm::quat actual(v[offset+3],v[offset],v[offset+1],v[offset+2]);
                                const auto expected = glm::quat(target->transform.rotation) * glm::quat(frame.rotation[3],
                                    -frame.rotation[0],-frame.rotation[1],frame.rotation[2]);
                                if (std::abs(glm::dot(glm::normalize(actual),glm::normalize(expected))) < .9999f)
                                    throw std::runtime_error("VMD body rotation does not match source motion");
                                ++verifiedKeys;
                            }
                    }
            if (verifiedKeys < 500) throw std::runtime_error("Too few authored body keys verified");
            std::cout << "Verified Sad Cat Dance source bone keys: " << verifiedKeys << '\n' << std::flush;
            Capture(renderer, prefab.string(), "mococo-bound", {0,.85f,0}, 1.9f, .08f);
            return 0;
        }
        const std::string source = "Engine/Core/Assets/Models/mmd/Roku/Roku_V1_packed.fbx";
        Model::ImportedModel imported;
        if (!Editor::FbxModelDecoder::Decode(source, imported, error)) throw std::runtime_error(error);
        std::cout << "Imported unit scale " << imported.unitScaleToMeters << " skins";
        for (const auto& skin : imported.skins) std::cout << ' ' << skin.jointNodes.size();
        std::cout << '\n' << std::flush;
        ufbx_load_opts options{}; options.evaluate_skinning = true;
        options.target_axes = ufbx_axes_right_handed_y_up; options.target_unit_meters = 1.f;
        ufbx_error sourceError{};
        std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> sourceScene(
            ufbx_load_file(source.c_str(), &options, &sourceError), ufbx_free_scene);
        if (!sourceScene) throw std::runtime_error("Cannot load source reference");
        glm::vec3 low, high;
        auto reference = Reference(imported, *sourceScene, low, high);
        std::cout << "Source bounds " << low.x << ',' << low.y << ',' << low.z << " -> "
            << high.x << ',' << high.y << ',' << high.z << '\n' << std::flush;
        const glm::vec3 center = (low+high)*.5f;
        const float height = high.y-low.y;
        auto actual = Editor::ModelAssetWriter::Write(imported, source, (output/"engine").string());
        if (!actual.success) throw std::runtime_error(actual.message);
        const auto actualPixels = Capture(renderer, actual.prefabPath, "roku-engine", center, height);
        auto expected = Editor::ModelAssetWriter::Write(reference, source, (output/"reference").string());
        if (!expected.success) throw std::runtime_error(expected.message);
        const auto sourcePixels = Capture(renderer, expected.prefabPath, "roku-source", center, height, 0.f, {-1});
        size_t mismatched = 0;
        for (size_t i = 0; i < actualPixels.size(); i += 4)
            if (std::abs(int(actualPixels[i])-int(sourcePixels[i])) > 50 ||
                std::abs(int(actualPixels[i+1])-int(sourcePixels[i+1])) > 50 ||
                std::abs(int(actualPixels[i+2])-int(sourcePixels[i+2])) > 50) ++mismatched;
        const double fraction = double(mismatched)/(actualPixels.size()/4);
        std::cout << "Roku source/reference render pixel mismatch fraction: " << fraction << '\n' << std::flush;
        if (fraction > .02) throw std::runtime_error("Roku rendered import diverges from the FBX source reference");
        for (int frame : {60,210})
        {
            ufbx_evaluate_opts evaluate{}; evaluate.evaluate_skinning = true;
            auto* stack = ufbx_find_anim_stack(sourceScene.get(), "Base_Layer");
            std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> pose(
                ufbx_evaluate_scene(sourceScene.get(), stack->anim, frame/30.0, &evaluate, &sourceError), ufbx_free_scene);
            if (!pose) throw std::runtime_error("Cannot evaluate animated source reference");
            auto sampled = Reference(imported, *pose, low, high);
            sampled.name += " " + std::to_string(frame);
            auto saved = Editor::ModelAssetWriter::Write(sampled, source, (output/"reference").string());
            if (!saved.success) throw std::runtime_error(saved.message);
            Capture(renderer, saved.prefabPath, "roku-source-" + std::to_string(frame), center, height, 0.f, {-1});
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
