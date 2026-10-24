#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Object.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Serialization/SceneSerializerInternal.h"
#include "Core/Importers/MmdDanceAssetBinder.h"
#include <chrono>
#include <cmath>
#include <filesystem>
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
        std::cout << "Character performance regressions passed" << std::endl;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
