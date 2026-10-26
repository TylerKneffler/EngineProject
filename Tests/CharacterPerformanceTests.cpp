#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Object.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationRigMapping.h"
#include "Core/Serialization/SceneSerializerInternal.h"
#include "Core/Importers/MmdDanceAssetBinder.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
class CountedComponent : public Engine::Core::Component
{
public:
    CountedComponent() { SetTypeName("CountedComponent"); }
    int value = 0;
    int loads = 0;
    JsonValue Serialize() const override
    { return Component::Serialize().Set("value", JsonValue(value)); }
    void Deserialize(const JsonValue& source) override
    { Component::Deserialize(source); value = source["value"].AsInt(); ++loads; }
};
void CheckOverrideApplication()
{
    using Behavior = Engine::Serialization::Detail::SceneObjectBehavior;
    Engine::Core::Object object;
    auto* component = object.AddComponent<CountedComponent>();
    auto state = Behavior::SerializeObject(object, false);
    state.Set("enabled", Engine::Serialization::JsonValue(false));
    Check(Behavior::ApplyObjectState(object, state, nullptr), "Enabled override failed");
    Check(!object.enabled && component->loads == 0, "Enabled override reloaded unchanged component");
    state["components"].ArrayAt(0).Set("value", Engine::Serialization::JsonValue(42));
    Check(Behavior::ApplyObjectState(object, state, nullptr), "Component override failed");
    Check(component->value == 42 && component->loads == 1, "Changed component was not loaded");
}
void CheckMorphBlending()
{
    Engine::Scene::Scene scene;
    auto* root = scene.AddObject();
    auto* model = root->AddComponent<Engine::Components::Model>();
    auto* face = scene.AddObject();
    face->name = "Face"; face->Parent = root; root->Children.push_back(face);
    model->BindNode(0, root); model->BindNode(1, face);
    auto* mesh = face->AddComponent<Engine::Components::Mesh>();
    mesh->SetMorphData(1, { Engine::Model::MorphTarget{} }, { .2f });
    auto* manager = root->AddComponent<Engine::Components::AnimationManager>();
    Engine::Model::AnimationClip base;
    base.clipName = "base"; base.duration = 1;
    Engine::Model::AnimationChannel channel;
    channel.nodeIndex = 1; channel.targetMesh = "Face";
    channel.path = Engine::Model::AnimationChannel::Path::Weights;
    channel.valueWidth = 1; channel.times = {0.f, 1.f}; channel.values = {.8f, .8f};
    base.channels.push_back(channel);
    auto layer = base; layer.clipName = "layer"; layer.channels[0].values = {.6f, .6f};
    manager->InstallEmbeddedClips({base, layer}); manager->clip = "base";
    Engine::Model::AnimationLayer blend; blend.clip = "layer"; blend.weight = .5f;
    manager->layers.push_back(blend); manager->Start(); manager->Tick(0);
    Check(std::abs(mesh->GetMorphWeights()[0] - .7f) < 1e-6f, "Named morph blend changed");
    const auto revision = mesh->GetMorphWeightsRevision();
    manager->Tick(0);
    Check(mesh->GetMorphWeightsRevision() == revision, "Unchanged final morph uploaded again");
    manager->layers[0].enabled = false; manager->Tick(0);
    Check(std::abs(mesh->GetMorphWeights()[0] - .8f) < 1e-6f, "Disabled morph layer still applied");
}
void CheckClipCache()
{
    const auto path = std::filesystem::temp_directory_path() /
        ("character-cache-" + std::to_string(Clock::now().time_since_epoch().count()) + ".anim");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    Engine::Model::AnimationClipAsset asset;
    asset.clip.clipName = "original";
    Check(asset.Save(path.string()), "Clip save failed");
    auto first = Engine::Model::AnimationClipAsset::Load(path.string());
    Check(bool(first), "Clip load failed"); first->clip.clipName = "edited instance";
    auto second = Engine::Model::AnimationClipAsset::Load(path.string());
    Check(second && second->clip.clipName == "original", "Cache leaked edits between instances");
    const auto stamp = std::filesystem::last_write_time(path);
    asset.clip.clipName = "replaced";
    Check(asset.Save(path.string()), "Clip replacement failed");
    std::filesystem::last_write_time(path, stamp);
    auto replaced = Engine::Model::AnimationClipAsset::Load(path.string());
    Check(replaced && replaced->clip.clipName == "replaced", "Save did not invalidate cache");
    std::filesystem::remove(path);
    Check(!Engine::Model::AnimationClipAsset::Load(path.string()), "Deleted clip returned from cache");
}
template<class F> void Measure(const char* name, F action)
{
    const auto start = Clock::now();
    action();
    std::cout << name << ": " << std::chrono::duration<double, std::milli>(
        Clock::now() - start).count() << " ms" << std::endl;
}
int main(int argc, char** argv)
{
    try
    {
        CheckMorphBlending();
        CheckClipCache();
        CheckOverrideApplication();
        Engine::Scene::Scene scene;
        Engine::Core::Object* root = nullptr;
        const std::string path = argc > 1 ? argv[1] :
            "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.low.prefab";
        Measure("instantiate", [&] { root = Engine::Serialization::SceneSerializer::InstantiatePrefab(scene, path); });
        if (!root) throw std::runtime_error("Could not instantiate character");
        Measure("initial override check", [&] {
            if (Engine::Serialization::SceneSerializer::HasPrefabOverrides(*root, false))
                throw std::runtime_error("Fresh instance has overrides");
        });
        std::string snapshot;
        Measure("save", [&] { snapshot = scene.SaveSnapshotToString(); });
        Measure("start", [&] { scene.Start(); });
        Measure("120 animation frames", [&] { for (int i=0;i<120;++i) scene.Update(1.f/60); });
        Measure("disable and override check", [&] {
            root->Disabled(); root->InvalidatePrefabOverrideCache();
            if (!Engine::Serialization::SceneSerializer::HasPrefabOverrides(*root, false))
                throw std::runtime_error("Enabled override lost");
        });
        Measure("120 disabled frames", [&] { for (int i=0;i<120;++i) scene.Update(1.f/60); });
        root->Enabled();
        Measure("restore", [&] { if (!Engine::Serialization::SceneSerializer::LoadFromString(scene, snapshot)) throw std::runtime_error("Restore failed"); });
        Check(!scene.HasStarted(), "Restored scene retained runtime lifecycle state");
        Measure("second instantiate", [&] { if (!Engine::Serialization::SceneSerializer::InstantiatePrefab(scene, path)) throw std::runtime_error("Second instance failed"); });
        Engine::Model::AnimationPackAsset pack;
        std::string error;
        Measure("bind VMD", [&] {
            if (!Engine::Editor::MmdDanceAssetBinder::BuildForPrefab(path,
                "Engine/Core/Assets/Models/mmd/sad cat dance/dancing.vmd", pack, error))
                throw std::runtime_error(error);
        });
        root = scene.GetObjects().front().get();
        auto* manager = root->GetComponent<Engine::Components::AnimationManager>();
        Check(manager != nullptr, "Character lacks animation manager");
        Measure("apply VMD", [&] { if (!pack.Apply(*manager, error)) throw std::runtime_error(error); });
        const auto expectedManager = Engine::Serialization::AssetWriteBinary(manager->Serialize());
        root->InvalidatePrefabOverrideCache();
        Measure("save embedded VMD", [&] { snapshot = scene.SaveSnapshotToString(); });
        Measure("restore embedded VMD", [&] {
            Check(Engine::Serialization::SceneSerializer::LoadFromString(scene, snapshot), "Embedded VMD restore failed");
        });
        auto* restoredManager = scene.GetObjects().front()->GetComponent<Engine::Components::AnimationManager>();
        Check(restoredManager && Engine::Serialization::AssetWriteBinary(restoredManager->Serialize()) == expectedManager,
            "Embedded VMD changed during snapshot round trip");
        const std::string legacyPath = "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/dance1.animpack";
        const auto legacyBytes = Engine::Serialization::AssetWriteBinary(
            Engine::Serialization::AssetParseFile(legacyPath));
        auto legacy = Engine::Model::AnimationPackAsset::Load(legacyPath, error);
        Check(legacy && legacy->bindSkeleton.empty() && legacy->sourceTracks.empty(),
            "Legacy dance1 fixture no longer exercises FBX-only tracks");
        Check(!legacy->Apply(*restoredManager, error) && error.find("RootNode/HumanEar") != std::string::npos,
            "Legacy FBX pack unexpectedly matched the PMX rig");
        Check(Engine::Serialization::AssetWriteBinary(restoredManager->Serialize()) == expectedManager,
            "Failed pack application modified existing animation playback");
        Measure("rebind legacy dance1 pack", [&] {
            if (!Engine::Editor::MmdDanceAssetBinder::ApplyForPrefab(path, legacyPath, *restoredManager, error))
                throw std::runtime_error(error);
        });
        Check(error.empty() && restoredManager->clips.size() == 2 && !restoredManager->layers.empty(),
            "Rebinding dance1 lost body or facial playback");
        auto* restoredModel = restoredManager->Owner->GetComponent<Engine::Components::Model>();
        bool hasBody = false, hasFacial = false;
        for (const auto& clip : restoredManager->clips)
        {
            hasFacial |= !clip.componentMorphs.empty();
            for (const auto& channel : clip.channels)
            {
                Check(channel.nodeIndex < restoredModel->GetNodeCount() &&
                    channel.targetPath.find("RootNode/") != 0,
                    "Rebound dance1 retained an old FBX node");
                hasBody |= channel.path == Engine::Model::AnimationChannel::Path::Rotation;
                hasFacial |= channel.path == Engine::Model::AnimationChannel::Path::Weights;
            }
        }
        Check(hasBody && hasFacial, "Rebound dance1 has no body or facial keys");
        Check(Engine::Serialization::AssetWriteBinary(Engine::Serialization::AssetParseFile(legacyPath)) == legacyBytes,
            "Rebinding changed the source pack");
        const auto reboundClips = restoredManager->clips;
        const auto reboundPrimary = restoredManager->clip;
        auto compatible = Engine::Model::AnimationPackAsset::Capture(*restoredManager);
        const auto compatiblePath = std::filesystem::path(".local/test-output/dance1-compatible.animpack");
        std::filesystem::create_directories(compatiblePath.parent_path());
        Check(compatible.Save(compatiblePath.string(), error), "Could not save compatible pack");
        Check(Engine::Editor::MmdDanceAssetBinder::ApplyForPrefab({}, compatiblePath.string(), *restoredManager, error),
            "Compatible pack required prefab metadata or original VMD");
        Check(restoredManager->clip == reboundPrimary && restoredManager->clips.size() == reboundClips.size(),
            "Compatible pack application changed clip selection");
        for (size_t clipIndex = 0; clipIndex < reboundClips.size(); ++clipIndex)
        {
            const auto& before = reboundClips[clipIndex];
            const auto& after = restoredManager->clips[clipIndex];
            Check(before.clipName == after.clipName && before.channels.size() == after.channels.size(),
                "Compatible pack changed its clips or channel counts");
            for (size_t channelIndex = 0; channelIndex < before.channels.size(); ++channelIndex)
            {
                const auto& a = before.channels[channelIndex];
                const auto& b = after.channels[channelIndex];
                Check(a.nodeIndex == b.nodeIndex && a.targetPath == b.targetPath && a.values.size() == b.values.size(),
                    "Compatible pack remapped an existing target");
                // Pack clip strings use the existing JSON numeric precision.
                for (size_t key = 0; key < a.values.size(); ++key)
                    Check(std::abs(a.values[key] - b.values[key]) <= 2e-5f * std::max(1.f, std::abs(a.values[key])),
                        "Compatible pack retargeted existing local keys");
            }
        }
        const std::string legacyBodyPath = "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/dance1_body.anim";
        auto legacyBody = Engine::Model::AnimationClipAsset::Load(legacyBodyPath);
        const std::string bodyName = legacyBody ? legacyBody->clip.clipName : "dance1 body";
        const auto readBytes = [](const std::string& filePath) {
            std::ifstream file(filePath, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        const auto bodySourceBytes = readBytes(legacyBodyPath);
        Check(!bodySourceBytes.empty(), "Legacy body fixture is missing");
        const auto existingCount = restoredManager->clips.size();
        const auto existingLayers = restoredManager->layers.size();
        // Exercise the same add helper used for Properties and Scene drops.
        restoredManager->baseAnimationEnabled = false;
        Measure("add legacy body clip", [&] {
            if (!Engine::Editor::MmdDanceAssetBinder::AddClipForPrefab(
                path, legacyBodyPath, *restoredManager, error)) throw std::runtime_error(error);
        });
        Check(error.empty() && restoredManager->baseAnimationEnabled &&
            restoredManager->clip == bodyName && restoredManager->time == 0.f,
            "Dropped body clip was not selected for playback");
        Check(restoredManager->clips.size() == existingCount && restoredManager->layers.size() == existingLayers,
            "Adding body clip duplicated animations or discarded existing layers");
        const auto body = std::find_if(restoredManager->clips.begin(), restoredManager->clips.end(),
            [&](const auto& clip) { return clip.clipName == bodyName; });
        Check(body != restoredManager->clips.end() && !body->channels.empty() && body->componentMorphs.empty(),
            "Body-only drop added facial animation or lost bone motion");
        for (const auto& channel : body->channels)
            Check(channel.path != Engine::Model::AnimationChannel::Path::Weights,
                "Dropped body animation included facial channels");
        Check(body->channels.size() == legacy->clips.front().channels.size(),
            "Body recovery discarded unresolved key rows");
        const auto expectedBody = legacyBody
            ? Engine::Model::AnimationRigMapping::Map(*legacyBody, *restoredModel,
                Engine::Model::RigAsset::ForModel(*restoredModel)).clip : legacy->clips.front();
        for (size_t index = 0; index < body->channels.size(); ++index)
            Check(body->channels[index].values == expectedBody.channels[index].values &&
                body->channels[index].times == expectedBody.channels[index].times,
                "Body import did not use the saved source bind transforms");
        if (!legacyBody)
            Check(restoredManager->animationPaths.empty(), "Recovered body retained an unreadable external asset reference");
        Check(readBytes(legacyBodyPath) == bodySourceBytes,
            "Body drop changed the original animation asset");
        // Timeline imports use the preparation helper before their normal mapper.
        Engine::Model::AnimationClipAsset legacyFace;
        bool rebound = false;
        Check(Engine::Editor::MmdDanceAssetBinder::LoadClipForPrefab(path,
            "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/dance1_facial.anim", legacyFace, rebound, error) && rebound,
            "Facial-only animation could not be rebound");
        Check(!legacyFace.clip.channels.empty() || !legacyFace.clip.componentMorphs.empty(),
            "Facial-only rebinding lost its targets");
        for (const auto& channel : legacyFace.clip.channels)
            Check(channel.path == Engine::Model::AnimationChannel::Path::Weights,
                "Facial-only rebinding included body animation");
        const auto exportDirectory = std::filesystem::path(".local/test-output");
        const auto exportId = std::to_string(Clock::now().time_since_epoch().count());
        for (const std::string kind : {"Body", "Facial"})
        {
            const auto sourceTrack = std::filesystem::path(
                "Engine/Core/Assets/Models/mmd/sad cat dance/dancing VMD") / (kind + ".vmdtrack");
            const auto copiedTrack = exportDirectory / (kind + "-no-source-" + exportId + ".vmdtrack");
            std::filesystem::copy_file(sourceTrack, copiedTrack);
            const auto exported = exportDirectory / (kind + "-export-" + exportId + ".anim");
            if (!Engine::Editor::MmdDanceAssetBinder::ExportTrackAnimation(
                    copiedTrack.string(), path, exported.string(), error)) throw std::runtime_error(error);
            const auto loaded = Engine::Model::AnimationClipAsset::Load(exported.string());
            Check(loaded && !loaded->bindSkeleton.empty() && loaded->previewPrefab == path &&
                loaded->clip.clipName == exported.stem().string(), "Track export lost bind pose or clip identity");
            Check(!loaded->clip.channels.empty() || !loaded->clip.componentMorphs.empty(),
                "Track conversion produced an empty animation");
            Check(std::filesystem::is_regular_file(exported.string() + ".meta"),
                "Track export did not create an asset identity");
            for (const auto& channel : loaded->clip.channels)
                Check(channel.nodeIndex < restoredModel->GetNodeCount(),
                    "Track export contains unresolved target nodes");
            const auto originalBytes = readBytes(exported.string());
            Check(!Engine::Editor::MmdDanceAssetBinder::ExportTrackAnimation(
                copiedTrack.string(), path, exported.string(), error) &&
                readBytes(exported.string()) == originalBytes, "Track conversion overwrote an existing animation");
        }
        std::cout << "Character performance regressions passed" << std::endl;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
