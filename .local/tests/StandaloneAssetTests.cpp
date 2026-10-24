#include "Core/Model/RigAsset.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Model/MeshPackAsset.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Engine/Editor/Core/Importers/AssetDataExtractor.h"
#include "Engine/Editor/Core/Importers/ModelImporter.h"
#include "Engine/Editor/Core/Importers/MmdDanceAssetBinder.h"
#include "InspectorTestUi.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

class InspectorAnimationManager : public Engine::Components::AnimationManager
{
public:
    mutable size_t serializationCalls=0;
    JsonValue Serialize() const override
    {
        ++serializationCalls;
        return AnimationManager::Serialize();
    }
};

int main()
{
    namespace fs = std::filesystem;
    using namespace Engine;
    const auto output = fs::absolute(fs::path(".local/test-output") /
        ("standalone-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    fs::create_directories(output);
    try
    {
        Model::RigAsset rig;
        rig.joints.push_back({ 0, "Hip", -1, glm::mat4(1.f) });
        rig.bindNodes.push_back({ "Hip", "", { 0.f, 1.f, 0.f }, {}, { 1.f, 1.f, 1.f } });
        const auto skeletonPath = output / "Hero.skeleton";
        CHECK(rig.Save(skeletonPath.string()));
        const auto loaded = Model::RigAsset::Load(skeletonPath.string());
        CHECK(loaded && loaded->bindNodes.size() == 1);
        Scene::Scene scene;
        auto* root = scene.AddObject("Other object");
        // Deliberately use a different node ordering to exercise path remapping.
        auto* model = root->AddComponent<Components::Model>();
        model->BindNode(0, root);
        std::string error;
        CHECK(loaded->ApplySkeleton(*root, skeletonPath.string(), error));
        auto* skeleton = root->GetComponent<Components::Skeleton>();
        CHECK(skeleton && skeleton->jointNodes.size() == 1);
        CHECK(skeleton->jointNodes[0] == 1);
        CHECK(model->ResolveNode(1)->name == "Hip");
        CHECK(model->ResolveNode(1)->transform.position.y == 1.f);
        const size_t objects = scene.GetObjects().size();
        CHECK(loaded->ApplySkeleton(*root, skeletonPath.string(), error));
        CHECK(scene.GetObjects().size() == objects);
        auto invalid = *loaded;
        invalid.bindNodes[0].parent = "Missing";
        CHECK(!invalid.ApplySkeleton(*root, skeletonPath.string(), error));
        CHECK(scene.GetObjects().size() == objects);

        Model::AnimationClipAsset animation;
        animation.clip.clipName = "Walk";
        animation.clip.duration = 1.f;
        Model::AnimationChannel channel;
        channel.nodeIndex = 0;
        channel.targetPath = "Hip";
        channel.path = Model::AnimationChannel::Path::Translation;
        channel.valueWidth = 3;
        channel.times = { 0.f, 1.f };
        channel.values = { 0.f, 1.f, 0.f, 0.f, 2.f, 0.f };
        animation.clip.channels.push_back(channel);
        animation.bindSkeleton = rig.bindNodes;
        animation.baseSkeleton = rig.bindNodes;
        const auto animationPath = output / "Walk.anim";
        CHECK(animation.Save(animationPath.string()));
        auto* manager = root->AddComponent<Components::AnimationManager>();
        CHECK(manager->AddAnimationAsset(animationPath.string(), error));
        CHECK(manager->animationPaths.size() == 1 && manager->clips.size() == 1);
        manager->BeginEditorPreview();
        CHECK(manager->IsEditorPreviewRunning());
        manager->Tick(.25f);
        CHECK(manager->time > 0.f);
        manager->Start();
        manager->time = .5f;
        manager->playing = false;
        manager->Tick(0.f);
        CHECK(model->ResolveNode(1)->transform.position.y > 1.4f);
        CHECK(manager->clips[0].channels[0].nodeIndex == 1);
        CHECK(manager->AddAnimationAsset(animationPath.string(), error));
        CHECK(manager->animationPaths.size() == 1 && manager->clips.size() == 1);
        const auto saved = manager->Serialize();
        CHECK(saved["animationPaths"].ArraySize() == 1);
        CHECK(saved["clips"].ArraySize() == 0);
        manager->Deserialize(saved);
        manager->RefreshRigClips();
        CHECK(manager->clips.size() == 1 && manager->clips[0].channels[0].nodeIndex == 1);
        CHECK(manager->clips[0].channels[0].values.back() == 0.f);
        CHECK(manager->clips[0].channels[0].values[4] == 2.f);
        std::ifstream originalFile(skeletonPath, std::ios::binary);
        const std::string original((std::istreambuf_iterator<char>(originalFile)), {});
        originalFile.close();
        CHECK(Serialization::SceneSerializer::SavePrefab(*root, (output / "Other.prefab").string()));
        std::ifstream afterFile(skeletonPath, std::ios::binary);
        CHECK(std::string((std::istreambuf_iterator<char>(afterFile)), {}) == original);
        Scene::Scene restoredScene;
        auto* restored = Serialization::SceneSerializer::InstantiatePrefab(
            restoredScene, (output / "Other.prefab").string(), nullptr);
        CHECK(restored);
        CHECK(restored->GetComponent<Components::Skeleton>()->jointNodes[0] == 1);
        CHECK(restored->GetComponent<Components::AnimationManager>()->animationPaths.size() == 1);

        Model::AnimationPackAsset pack;
        pack.clips.push_back(animation.clip);
        pack.activeClip = "Walk";
        pack.sourceTracks.push_back({ "camera", "{\"keys\":[]}" });
        const auto packPath = output / "Motion.animpack";
        CHECK(pack.Save(packPath.string(), error));
        CHECK(pack.Apply(*manager, error));
        manager->BeginEditorPreview();
        manager->Tick(.25f);
        CHECK(manager->IsEditorPreviewRunning() && manager->time > .2f);
        Scene::Scene bundleScene;
        auto* bundleObject = bundleScene.AddObject("Bundle target");
        auto* bundleModel = bundleObject->AddComponent<Components::Model>();
        bundleModel->BindNode(0, bundleObject);
        CHECK(loaded->ApplySkeleton(*bundleObject, skeletonPath.string(), error));
        auto* bundleManager = bundleObject->AddComponent<Components::AnimationManager>();
        CHECK(pack.Apply(*bundleManager, error));
        bundleManager->BeginEditorPreview();
        bundleManager->Tick(.25f);
        CHECK(bundleManager->IsEditorPreviewRunning() && bundleManager->time > .2f);
        CHECK(bundleModel->ResolveNode(1)->transform.position.y > 1.2f);
        auto extracted = Editor::AssetDataExtractor::Extract(packPath, output);
        CHECK(extracted.success);
        CHECK(Model::AnimationClipAsset::Load((fs::path(extracted.outputDirectory) / "Walk_1.anim").string()));
        CHECK(fs::exists(fs::path(extracted.outputDirectory) / "camera_1.vmdtrack"));
        CHECK(fs::exists(packPath));
        auto again = Editor::AssetDataExtractor::Extract(packPath, output);
        CHECK(again.success && again.outputDirectory != extracted.outputDirectory);
        rig.animationPaths.push_back(animationPath.string());
        const auto rigPath = output / "Hero.rig";
        CHECK(rig.Save(rigPath.string()));
        extracted = Editor::AssetDataExtractor::Extract(rigPath, output);
        CHECK(extracted.success);
        CHECK(Model::RigAsset::Load((fs::path(extracted.outputDirectory) / "Hero_1.skeleton").string()));
        CHECK(fs::exists(fs::path(extracted.outputDirectory) / "Walk_1.anim"));

        fs::create_directories(output / "Meshes");
        { std::ofstream mesh(output / "Meshes/Test.mesh", std::ios::binary); mesh << "mesh payload"; }
        CHECK(Model::MeshPackAsset::PackRig(rigPath, error));
        const auto meshPack = output / "Hero.meshpack";
        extracted = Editor::AssetDataExtractor::Extract(meshPack, output);
        CHECK(extracted.success && fs::exists(meshPack));
        CHECK(fs::exists(fs::path(extracted.outputDirectory) / "Meshes/Test.mesh"));

        // Importing and splitting must both accept fixed-byte truncated CP932
        // names, and preserve portable bone/morph tracks in the saved pack.
        const fs::path sadCat("Engine/Core/Assets/Models/mmd/sad cat dance/dancing.vmd");
        const std::string mococo="Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.low.prefab";
        const auto prefabBefore=fs::last_write_time(mococo);
        Model::AnimationPackAsset rawDance;
        if (!Editor::MmdDanceAssetBinder::BuildForPrefab(mococo,sadCat.string(),rawDance,error))
            throw std::runtime_error(error);
        CHECK(rawDance.activeClip=="dancing body");
        CHECK(rawDance.clips.size()==2 && rawDance.clips[0].channels.size()>100);
        CHECK(rawDance.layers.size()==1 && rawDance.layers[0].clip=="dancing facial");
        Scene::Scene characterScene;
        auto* character=Serialization::SceneSerializer::InstantiatePrefab(characterScene,mococo);
        CHECK(character);
        auto* characterModel=character->GetComponent<Components::Model>();
        CHECK(characterModel);
        if (auto* oldManager=character->GetComponent<Components::AnimationManager>())
        {
            character->Components.remove(oldManager);
            delete oldManager;
        }
        auto* danceManager=character->AddComponent<Components::AnimationManager>();
        if (!rawDance.Apply(*danceManager,error)) throw std::runtime_error(error);
        danceManager->BeginEditorPreview();
        CHECK(danceManager->clip=="dancing body" && danceManager->clips.size()==2);
        danceManager->time=7.f;danceManager->Tick(0.f);
        size_t movedBones=0;
        for (const auto& danceChannel : rawDance.clips[0].channels)
            if (danceChannel.path==Model::AnimationChannel::Path::Rotation)
                if (auto* bone=characterModel->ResolveNode(danceChannel.nodeIndex))
                    movedBones+=glm::length(bone->transform.rotation-rawDance.bindSkeleton[danceChannel.nodeIndex].rotation)>.01f;
        CHECK(movedBones>10);
        const auto sceneSnapshot=characterScene.SaveToString();
        Scene::Scene reopened;
        CHECK(Serialization::SceneSerializer::LoadFromString(reopened,sceneSnapshot));
        bool persistedDance=false;
        for (const auto& object : reopened.GetObjects())
            if (auto* playback=object->GetComponent<Components::AnimationManager>())
                persistedDance|=playback->clip=="dancing body" && playback->FindClip("dancing body") &&
                    playback->FindClip("dancing body")->channels.size()>100;
        CHECK(persistedDance);
        for (bool split : { false, true })
        {
            const auto imported = split ? Editor::AssetDataExtractor::Extract(sadCat, output) :
                Editor::ModelImporter::Import(sadCat.string(), output.string());
            if (!imported.success) throw std::runtime_error(imported.message);
            CHECK(fs::is_regular_file(imported.primaryAssetPath));
            auto motionPack = Model::AnimationPackAsset::Load(imported.primaryAssetPath, error);
            CHECK(motionPack && !motionPack->sourceTracks.empty());
            bool bones = false, morphs = false;
            for (const auto& track : motionPack->sourceTracks)
            {
                bones |= track.kind == "Body";
                morphs |= track.kind == "Facial";
            }
            CHECK(bones && morphs);
            // A portable source-only pack must bind from its embedded keys,
            // without relying on a source VMD beside it or .meta metadata.
            const auto portable=output/(split?"Split portable.animpack":"Portable.animpack");
            CHECK(motionPack->Save(portable.string(),error));
            Model::AnimationPackAsset baked;
            if (!Editor::MmdDanceAssetBinder::BuildForPrefab(mococo,portable.string(),baked,error))
                throw std::runtime_error(error);
            CHECK(baked.clips.size()==rawDance.clips.size());
            CHECK(baked.clips[0].channels.size()==rawDance.clips[0].channels.size());
            for (const auto& bakedChannel : baked.clips[0].channels)
            {
                const auto found=std::find_if(rawDance.clips[0].channels.begin(),rawDance.clips[0].channels.end(),
                    [&](const auto& raw) {return raw.nodeIndex==bakedChannel.nodeIndex && raw.path==bakedChannel.path;});
                CHECK(found!=rawDance.clips[0].channels.end());
                CHECK(found->times==bakedChannel.times);
                CHECK(found->values.size()==bakedChannel.values.size());
                // Native source-track JSON stores seven significant digits.
                for (size_t key=0;key<found->values.size();++key)
                    CHECK(std::abs(found->values[key]-bakedChannel.values[key])<1e-4f);
            }
            if (!baked.Apply(*danceManager,error)) throw std::runtime_error(error);
            CHECK(danceManager->FindClip(baked.activeClip)->channels.size()>100);
        }
        CHECK(fs::last_write_time(mococo)==prefabBefore);

        InspectorAnimationManager inspector;
        inspector.Owner=character;
        inspector.InstallEmbeddedClips(rawDance.clips);
        inspector.clip=rawDance.activeClip;
        Editor::InspectorTestUi inspectorUi;
        const auto drawStarted=std::chrono::steady_clock::now();
        for (int frame=0;frame<120;++frame) CHECK(!inspector.DrawProperties(inspectorUi));
        const auto drawMs=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-drawStarted).count();
        CHECK(inspector.serializationCalls==0);
        CHECK(!inspector.IsEditorPreviewRunning());
        inspectorUi.changePlaying=true;
        CHECK(inspector.DrawProperties(inspectorUi));
        CHECK(!inspector.IsEditorPreviewRunning());
        inspectorUi.changeClip=true;
        CHECK(inspector.DrawProperties(inspectorUi));
        CHECK(!inspector.IsEditorPreviewRunning());
        inspectorUi.clickButton="Play Editor Preview";
        inspector.DrawProperties(inspectorUi);
        CHECK(inspector.IsEditorPreviewRunning());
        inspectorUi.clickButton="Stop Editor Preview";
        inspector.DrawProperties(inspectorUi);
        CHECK(!inspector.IsEditorPreviewRunning() && inspector.playing);
        const float stoppedTime=inspector.time;
        inspector.Tick(.1f); // Scene Play can still advance the attached clip.
        CHECK(inspector.time>stoppedTime);
        inspector.BeginEditorPreview();
        inspector.InstallEmbeddedClips(rawDance.clips);
        CHECK(!inspector.IsEditorPreviewRunning());
        std::cout<<"Dense Sad Cat Dance inspector: 120 draws in "<<drawMs<<" ms; no clip serialization\n";

        const auto missingFbx = Editor::ModelImporter::Import(
            (output / "missing.fbx").string(), output.string());
        CHECK(!missingFbx.success && !missingFbx.message.empty());
        CHECK(missingFbx.outputDirectory.empty());

        const fs::path walking("Engine/Core/Assets/Models/Mixemo/Walking.fbx");
        if (fs::exists(walking))
        {
            extracted = Editor::AssetDataExtractor::Extract(walking, output);
            CHECK(extracted.success);
            fs::path skeletonFile, animationFile;
            for (const auto& entry : fs::directory_iterator(extracted.outputDirectory))
            {
                if (entry.path().extension() == ".skeleton") skeletonFile = entry.path();
                if (entry.path().extension() == ".anim") animationFile = entry.path();
            }
            CHECK(!skeletonFile.empty() && !animationFile.empty());
            auto walkingSkeleton = Model::RigAsset::Load(skeletonFile.string());
            CHECK(walkingSkeleton && walkingSkeleton->prefabPath.empty());
            Scene::Scene independent;
            auto* actor = independent.AddObject("Independent walker");
            CHECK(walkingSkeleton->ApplySkeleton(*actor, skeletonFile.string(), error));
            auto* playback = actor->AddComponent<Components::AnimationManager>();
            CHECK(playback->AddAnimationAsset(animationFile.string(), error));
            CHECK(!playback->clips.empty() && !playback->clips.front().channels.empty());
        }
        std::cout << "Standalone extraction, hierarchy binding, animation references, and prefab reload passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
