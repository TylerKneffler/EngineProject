#include "Editor/Core/Importers/FbxModelDecoder.h"
#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationRigMapping.h"
#include "Core/Model/RigAsset.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Window.h"

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
#include <Windows.h>
#include <ufbx.h>

namespace
{
struct WalkSamples
{
    std::map<std::string, std::array<glm::vec3, 5>> relative;
    float height = 0.f;
};

WalkSamples SampleWalk(Engine::Core::Object& root,
    Engine::Components::Model& model,
    const Engine::Model::RigAsset& rig,
    const Engine::Model::AnimationClip& clip)
{
    WalkSamples result;
    const std::array<std::string, 6> roles{
        "pelvis", "head", "lhand", "rhand", "lfoot", "rfoot"};
    std::map<std::string, unsigned> nodes;
    for (const auto& joint : rig.joints)
    {
        const auto path = Engine::Model::AnimationRigMapping::NodePath(
            model, joint.nodeIndex);
        const auto role = Engine::Model::AnimationRigMapping::Role(
            Engine::Model::AnimationRigMapping::Leaf(path));
        if (std::find(roles.begin(), roles.end(), role) != roles.end())
            nodes.try_emplace(role, joint.nodeIndex);
    }
    if (nodes.size() != roles.size()) return result;
    auto* manager = root.GetComponent<Engine::Components::AnimationManager>();
    if (!manager) manager = root.AddComponent<Engine::Components::AnimationManager>();
    manager->Start();
    manager->clips.clear();
    manager->clips.push_back(clip);
    manager->clip = clip.clipName;
    manager->playing = false;
    manager->looping = false;
    for (size_t frame = 0; frame < 5; ++frame)
    {
        manager->time = clip.duration * static_cast<float>(frame) / 5.f;
        manager->Tick(0.f);
        const glm::vec3 pelvis = model.ResolveNode(nodes.at("pelvis"))
            ->transform.GetWorldPosition();
        for (const auto& role : roles)
            result.relative[role][frame] =
                model.ResolveNode(nodes.at(role))->transform.GetWorldPosition() -
                pelvis;
    }
    result.height = glm::length(result.relative.at("head")[0]);
    return result;
}

WalkSamples SampleReferenceFbx(const std::filesystem::path& path)
{
    WalkSamples result;
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> source(
        ufbx_load_file(path.string().c_str(), nullptr, &error), ufbx_free_scene);
    if (!source || !source->anim) return result;
    const double duration = source->anim->time_end - source->anim->time_begin;
    for (size_t frame = 0; frame < 5; ++frame)
    {
        const double time = source->anim->time_begin +
            duration * static_cast<double>(frame) / 5.0;
        std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> pose(
            ufbx_evaluate_scene(source.get(), source->anim, time,
                nullptr, &error), ufbx_free_scene);
        if (!pose) return {};
        std::map<std::string, glm::vec3> points;
        for (size_t index = 0; index < pose->nodes.count; ++index)
        {
            const ufbx_node* node = pose->nodes.data[index];
            const std::string role = Engine::Model::AnimationRigMapping::Role(
                std::string(node->name.data, node->name.length));
            if (role == "pelvis" || role == "head" || role == "lhand" ||
                role == "rhand" || role == "lfoot" || role == "rfoot")
            {
                const auto point = node->node_to_world.cols[3];
                points.try_emplace(role, glm::vec3(point.x, point.y, point.z) *
                    static_cast<float>(source->settings.unit_meters));
            }
        }
        if (points.size() != 6) return {};
        for (const auto& [role, point] : points)
            result.relative[role][frame] = point - points.at("pelvis");
    }
    result.height = glm::length(result.relative.at("head")[0]);
    return result;
}

void SaveBmp(const std::vector<uint8_t>& rgba, unsigned width,
    unsigned height, const std::filesystem::path& path)
{
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
        for (unsigned x = 0; x < width; ++x)
        {
            const auto* pixel = rgba.data() +
                (static_cast<size_t>(y) * width + x) * 4;
            const uint8_t bgra[]{pixel[2], pixel[1], pixel[0], pixel[3]};
            out.write(reinterpret_cast<const char*>(bgra), 4);
        }
    if (!out) throw std::runtime_error("Could not write " + path.string());
}

void CaptureWalk(Engine::Renderers::DX11GameRenderer& renderer,
    const std::filesystem::path& prefab,
    const Engine::Model::AnimationClip& clip,
    const Engine::Model::RigAsset& rig,
    const std::filesystem::path& directory, const std::string& name)
{
    constexpr unsigned width = 480, height = 640;
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        scene, prefab.string(), renderer.GetGraphicsProvider());
    if (!root) throw std::runtime_error("Could not load " + prefab.string());
    auto* model = root->GetComponent<Engine::Components::Model>();
    if (!model) throw std::runtime_error("Capture model is missing");
    std::map<std::string, unsigned> nodes;
    for (const auto& joint : rig.joints)
    {
        const auto role = Engine::Model::AnimationRigMapping::Role(
            Engine::Model::AnimationRigMapping::Leaf(
                Engine::Model::AnimationRigMapping::NodePath(
                    *model, joint.nodeIndex)));
        if (role == "head" || role == "lfoot" || role == "rfoot")
            nodes.try_emplace(role, joint.nodeIndex);
    }
    if (nodes.size() != 3)
        throw std::runtime_error("Capture head or foot bones are missing");
    auto* manager = root->GetComponent<Engine::Components::AnimationManager>();
    if (!manager) manager = root->AddComponent<Engine::Components::AnimationManager>();
    auto* light = scene.AddObject("Walk capture light")
        ->AddComponent<Engine::Components::Light>();
    light->lightType = static_cast<int>(Engine::Components::Light::Type::Directional);
    light->intensity = 2.5f;
    light->castsShadows = false;
    auto* cameraObject = scene.AddObject("Walk capture camera");
    auto* camera = cameraObject->AddComponent<Engine::Components::Camera>();
    camera->useTransformRotation = false;
    camera->fov = 45.f;
    camera->farPlane = 10000.f;
    scene.Start();
    manager->clips.clear();
    manager->clips.push_back(clip);
    manager->clip = clip.clipName;
    manager->playing = false;
    manager->looping = false;
    float framingHeight = 0.f;
    for (int frame = -1; frame < 5; ++frame)
    {
        if (frame >= 0)
        {
            manager->time = clip.duration * static_cast<float>(frame) / 5.f;
            manager->Tick(0.f);
        }
        const glm::vec3 head = model->ResolveNode(nodes.at("head"))
            ->transform.GetWorldPosition();
        const glm::vec3 feet = (model->ResolveNode(nodes.at("lfoot"))
            ->transform.GetWorldPosition() +
            model->ResolveNode(nodes.at("rfoot"))
            ->transform.GetWorldPosition()) * .5f;
        if (frame == -1)
        {
            framingHeight = std::max(glm::length(head - feet), .5f);
            camera->target = (head + feet) * .5f;
            cameraObject->transform.position = camera->target +
                glm::vec3(0.f, framingHeight * .1f, framingHeight * 1.8f);
        }
        scene.PrepareRenderFrame();
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        auto context = renderer.CreateFrameGraphicsContext();
        if (!context) throw std::runtime_error("No graphics context");
        scene.Render(context.get(), float(width) / height, camera, false,
            width, height, true);
        renderer.Clear(.08f, .09f, .11f);
        scene.Render(context.get(), float(width) / height, camera, false,
            width, height, false);
        if (frame == -1)
            std::cout << name << " render draws " <<
                scene.GetLastOrdinaryDrawCount() << ", skinned " <<
                scene.GetLastSkinnedObjectCount() << '\n' << std::flush;
        renderer.EndFrame();
        std::vector<uint8_t> rgba;
        if (!renderer.ReadExportFrameRGBA(rgba, true) ||
            rgba.size() != static_cast<size_t>(width) * height * 4)
            throw std::runtime_error("Could not capture walk frame");
        SaveBmp(rgba, width, height,
            directory / (name + (frame < 0 ? "_rest" :
                "_" + std::to_string(frame)) + ".bmp"));
    }
}
}

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
    for (const fs::path meshFbx : {
            fs::path("Engine/Core/Assets/Models/Mixemo/X Bot.fbx"),
            fs::path("Engine/Core/Assets/Models/mmd/Mococo/Mococo (FBX No Outline).fbx") })
    {
        if (!fs::is_regular_file(meshFbx)) continue;
        Engine::Model::ImportedModel meshModel;
        if (!Engine::Editor::FbxModelDecoder::Decode(
                meshFbx.string(), meshModel, error) ||
            meshModel.primitives.empty() || meshModel.skins.empty() ||
            !std::isfinite(meshModel.unitScaleToMeters) ||
            meshModel.unitScaleToMeters <= 0.f)
        {
            std::cerr << "Mesh FBX import failed for " << meshFbx << ": " <<
                error << '\n';
            return 1;
        }
        for (size_t index = 0; index < meshModel.nodes.size(); ++index)
            if (meshModel.nodes[index].parent >= static_cast<int>(index))
                throw std::runtime_error("Mesh FBX node hierarchy is not ordered");
        for (const auto& skin : meshModel.skins)
        {
            if (skin.jointNodes.size() != skin.inverseBindMatrices.size())
                throw std::runtime_error("Mesh FBX skin has invalid bind matrices");
            for (unsigned joint : skin.jointNodes)
                if (joint >= meshModel.nodes.size())
                    throw std::runtime_error("Mesh FBX skin has invalid joint index");
        }
        std::cout << meshFbx.filename().string() << " imported " <<
            meshModel.primitives.size() << " meshes, " <<
            meshModel.skins.size() << " skins, " <<
            meshModel.animations.size() << " animations";
        for (const auto& animation : meshModel.animations)
            std::cout << " [" << animation.name << ": " <<
                animation.channels.size() << " channels, " <<
                animation.duration << "s]";
        std::cout << '\n';
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
        if (!motion || motion->bindSkeleton.empty() ||
            motion->baseSkeleton.empty() || !targetRig || !targetModel ||
            !fs::is_regular_file(fs::path(extracted.outputDirectory) /
                (fs::path(extracted.outputDirectory).filename().string() + "_Rest.pose")))
        { std::cerr << "Extracted motion assets are incomplete\n"; return 1; }
        Engine::Scene::Scene sourceScene;
        auto* sourceRoot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            sourceScene, extracted.prefabPath, nullptr);
        const auto sourceRig = Engine::Model::RigAsset::Load(
            (fs::path(extracted.outputDirectory) /
                (fs::path(extracted.outputDirectory).filename().string() +
                    ".rig")).string());
        auto* sourceModel = sourceRoot
            ? sourceRoot->GetComponent<Engine::Components::Model>() : nullptr;
        if (!sourceRig || !sourceModel)
        { std::cerr << "Walking source rig could not be loaded\n"; return 1; }
        const WalkSamples sourceWalk = SampleWalk(*sourceRoot, *sourceModel,
            *sourceRig, motion->clip);
        if (!(sourceWalk.height > 0.f))
        { std::cerr << "Walking source pose could not be sampled\n"; return 1; }
        {
            const WalkSamples reference = SampleReferenceFbx(walking);
            if (!(reference.height > 0.f))
                throw std::runtime_error("ufbx could not evaluate the source walk");
            for (const auto& role : { "head", "lhand", "rhand", "lfoot", "rfoot" })
            {
                for (size_t frame = 0; frame < 5; ++frame)
                {
                    const auto& actual = sourceWalk.relative.at(role)[frame];
                    const auto& expected = reference.relative.at(role)[frame];
                    if (glm::length(actual - expected) > .025f)
                        throw std::runtime_error("Walking source differs from FBX at " +
                            std::string(role) + " frame " + std::to_string(frame));
                }
            }
        }
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
        const WalkSamples crashWalk = SampleWalk(*target, *targetModel,
            *targetRig, mapping.clip);
        if (!(crashWalk.height > 0.f))
        { std::cerr << "Crash Dummy walking pose could not be sampled\n"; return 1; }
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
            const WalkSamples mococoWalk = SampleWalk(*mococo, *mococoModel,
                *mococoRig, mococoMapping.clip);
            if (!(mococoWalk.height > 0.f))
            { std::cerr << "Mococo walking pose could not be sampled\n"; return 1; }
            float squaredDifference = 0.f;
            float sourceToCrash = 0.f, sourceToMococo = 0.f;
            float crashFootMotion = 0.f, mococoFootMotion = 0.f;
            size_t compared = 0;
            for (const std::string role : {"lhand", "rhand", "lfoot", "rfoot"})
            {
                float crashMotion = 0.f, mococoMotion = 0.f;
                for (size_t frame = 1; frame < 5; ++frame)
                {
                    const glm::vec3 crashDelta =
                        (crashWalk.relative.at(role)[frame] -
                            crashWalk.relative.at(role)[0]) / crashWalk.height;
                    const glm::vec3 mococoDelta =
                        (mococoWalk.relative.at(role)[frame] -
                            mococoWalk.relative.at(role)[0]) / mococoWalk.height;
                    crashMotion = std::max(crashMotion, glm::length(crashDelta));
                    mococoMotion = std::max(mococoMotion, glm::length(mococoDelta));
                    squaredDifference += glm::dot(crashDelta - mococoDelta,
                        crashDelta - mococoDelta);
                    const glm::vec3 sourceDelta =
                        (sourceWalk.relative.at(role)[frame] -
                            sourceWalk.relative.at(role)[0]) / sourceWalk.height;
                    sourceToCrash += glm::dot(sourceDelta - crashDelta,
                        sourceDelta - crashDelta);
                    sourceToMococo += glm::dot(sourceDelta - mococoDelta,
                        sourceDelta - mococoDelta);
                    ++compared;
                }
                std::cout << role << " displacement / torso height: Crash " <<
                    crashMotion << ", Mococo " << mococoMotion << '\n';
                if (role.find("foot") != std::string::npos)
                {
                    crashFootMotion = std::max(crashFootMotion, crashMotion);
                    mococoFootMotion = std::max(mococoFootMotion, mococoMotion);
                }
            }
            if (!(crashFootMotion > .1f && mococoFootMotion > .1f) ||
                !std::isfinite(squaredDifference))
            { std::cerr << "Walking clip did not move both rigs\n"; return 1; }
            std::cout << "Walking channels: " << motion->clip.channels.size() <<
                ", Crash unmatched: " << mapping.unmatchedChannels <<
                ", Mococo unmatched: " << mococoMapping.unmatchedChannels << '\n';
            std::cout << "Walk normalized displacement RMS difference: " <<
                std::sqrt(squaredDifference / compared) << '\n';
            std::cout << "Walking source to Crash RMS: " <<
                std::sqrt(sourceToCrash / compared) <<
                ", source to Mococo RMS: " <<
                std::sqrt(sourceToMococo / compared) << '\n';
            if (GetEnvironmentVariableA("WALK_VISUAL_AUDIT", nullptr, 0) > 0)
            {
                const fs::path imageDirectory =
                    ".local/test-output/mixamo-walk-all-ufbx";
                fs::create_directories(imageDirectory);
                Engine::Core::Window window(GetModuleHandleW(nullptr),
                    L"Walk visual comparison", 480, 640);
                Engine::Renderers::DX11GameRenderer renderer;
                if (!renderer.Init(window.GetHWND(), 480, 640) ||
                    !renderer.EnableOffscreenExport(480, 640, 1, 2))
                    throw std::runtime_error("DX11 walk capture failed");
                const fs::path xbotPath = fs::path("Engine/Core/Assets/Models/Mixemo") /
                    "X Bot.fbx";
                if (fs::is_regular_file(xbotPath))
                {
                    Engine::Model::ImportedModel xbotModelData;
                    if (!Engine::Editor::FbxModelDecoder::Decode(
                            xbotPath.string(), xbotModelData, error))
                        throw std::runtime_error(error);
                    const auto xbotImport = Engine::Editor::ModelAssetWriter::Write(
                        xbotModelData, xbotPath.string(), output.string());
                    if (!xbotImport.success)
                        throw std::runtime_error(xbotImport.message);
                    Engine::Scene::Scene xbotScene;
                    auto* xbot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                        xbotScene, xbotImport.prefabPath, nullptr);
                    const auto xbotRig = Engine::Model::RigAsset::Load(
                        (fs::path(xbotImport.outputDirectory) /
                            (fs::path(xbotImport.outputDirectory).filename().string() +
                                ".rig")).string());
                    auto* xbotModel = xbot
                        ? xbot->GetComponent<Engine::Components::Model>() : nullptr;
                    if (!xbotRig || !xbotModel)
                        throw std::runtime_error("X Bot rig could not be loaded");
                    const auto xbotMapping = Engine::Model::AnimationRigMapping::Map(
                        *motion, *xbotModel, *xbotRig);
                    if (xbotMapping.unmatchedChannels)
                        throw std::runtime_error("X Bot has unmatched walk channels");
                    const auto xbotWalk = SampleWalk(*xbot, *xbotModel,
                        *xbotRig, xbotMapping.clip);
                    for (size_t frame = 0; frame < 5; ++frame)
                        if (!(xbotWalk.relative.at("head")[frame].y > 0.f &&
                            xbotWalk.relative.at("lfoot")[frame].y < 0.f &&
                            xbotWalk.relative.at("rfoot")[frame].y < 0.f))
                            throw std::runtime_error(
                                "X Bot walking skeleton inverted at frame " +
                                std::to_string(frame));
                    CaptureWalk(renderer, xbotImport.prefabPath,
                        xbotMapping.clip, *xbotRig, imageDirectory, "xbot");
                }
                CaptureWalk(renderer, imported.prefabPath,
                    mapping.clip, *targetRig, imageDirectory, "crash");
                CaptureWalk(renderer, mococoPrefab,
                    mococoMapping.clip, *mococoRig, imageDirectory, "mococo");
                std::cout << "Walk captures: " << imageDirectory << '\n';
            }
        }
    }
    std::cout << "Imported " << model.primitives.size() << " meshes and "
        << model.skins.size() << " skins\n";
    return 0;
}
