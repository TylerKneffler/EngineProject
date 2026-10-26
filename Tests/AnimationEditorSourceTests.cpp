#include "Core/AnimationEditorSource.h"
#include "Core/AnimationTimelineEditing.h"
#include "Core/AnimationClipMapping.h"
#include "Core/Model/AnimationRigMapping.h"
#include <filesystem>
#include "Core/Serialization/SceneSerializer.h"
#include <iostream>
#include <stdexcept>

using namespace Engine;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
int main()
{
    try
    {
        Model::AnimationClipAsset asset;
        asset.clip.clipName = "Instance motion";
        asset.clip.duration = 1.f;
        Model::AnimationChannel channel;
        channel.nodeIndex = 1; channel.targetPath = "Hips/Hand";
        channel.path = Model::AnimationChannel::Path::Translation;
        channel.valueWidth = 3; channel.times = {0.f, 1.f};
        channel.values = {0.f, 1.f, 0.f, 1.f, 2.f, 3.f};
        asset.clip.channels.push_back(channel);
        Scene::Scene preview;
        Check(Serialization::SceneSerializer::LoadFromString(preview,
            Editor::AnimationEditorSource::ClipPreview(asset)), "Metadata-free clip did not open");
        auto* root = preview.GetObjects().front().get();
        auto* model = root->GetComponent<Components::Model>();
        Check(model && model->ResolveNode(1)->name == "Hand", "Preview lost channel node identity");
        Check(model->ResolveNode(1)->Parent->name == "Hips", "Preview did not construct parent paths");
        auto* manager = root->GetComponent<Components::AnimationManager>();
        Check(manager && manager->clips.size() == 1, "Preview lost animation data");
        manager->clips[0].channels[0].values[0] = 42.f;
        root->name = "Scene instance";
        root->transform.position = {2.f, 3.f, 4.f};
        root->SetPrefab("Original.prefab");
        Scene::Scene clone;
        Check(Serialization::SceneSerializer::LoadFromString(clone,
            Editor::AnimationEditorSource::Capture(*root)), "Live object did not open");
        auto* copied = clone.GetObjects().front().get();
        Check(!copied->Prefab && copied->name == root->name, "Object source reopened prefab data");
        Check(copied->transform.position == root->transform.position, "Object source lost instance placement");
        auto* copiedManager = copied->GetComponent<Components::AnimationManager>();
        Check(copiedManager->clips[0].channels[0].values[0] == 42.f, "Object source lost instance motion");
        copiedManager->clips[0].channels[0].values[0] = 13.f;
        Check(manager->clips[0].channels[0].values[0] == 42.f, "Preview modified the live object before save");
        Check(Editor::AnimationEditorSource::Rigs(copied).size() == 1, "Object source lost its skeleton");
        // A clip's saved skeleton defines the standalone preview even when
        // legacy metadata points at a prefab and a different rig ordinal.
        asset.previewPrefab = "Original.prefab";
        asset.previewRig = 7;
        asset.bindSkeleton.resize(2);
        asset.bindSkeleton[0].path = "Hips";
        asset.bindSkeleton[0].position = {0.f, 3.f, 0.f};
        asset.bindSkeleton[1].path = "Hips/Hand";
        asset.bindSkeleton[1].parent = "Hips";
        asset.bindSkeleton[1].position = {2.f, 1.f, 0.f};
        asset.baseSkeleton = asset.bindSkeleton;
        asset.baseSkeleton[1].position = {9.f, 9.f, 9.f};
        Scene::Scene standalone;
        Check(Serialization::SceneSerializer::LoadFromString(standalone,
            Editor::AnimationEditorSource::ClipPreview(asset)), "Clip with prefab metadata did not open");
        auto* standaloneRoot = standalone.GetObjects().front().get();
        auto* standaloneModel = standaloneRoot->GetComponent<Components::Model>();
        Check(!standaloneRoot->Prefab, "Standalone clip retained a prefab binding");
        Check(Editor::AnimationEditorSource::Rigs(standaloneRoot).size() == 1,
            "Preview rig hint changed standalone rig identity");
        Check(standaloneModel->ResolveNode(1)->Parent == standaloneModel->ResolveNode(0),
            "Standalone preview lost saved skeleton hierarchy");
        Check(standaloneModel->ResolveNode(1)->transform.position == asset.bindSkeleton[1].position,
            "Standalone preview replaced bind transforms with the animated base pose");
        Check(standaloneRoot->GetComponent<Components::AnimationManager>()->clips[0].channels[0].values ==
            asset.clip.channels[0].values, "Standalone preview changed the original animation channels");

        // A prefab's primary body clip and facial layer must both enter the editor.
        auto* timelineManager = standaloneRoot->GetComponent<Components::AnimationManager>();
        auto posedSource = asset;
        posedSource.clip.channels.clear();
        auto keyedRotation = channel;
        keyedRotation.path = Model::AnimationChannel::Path::Rotation;
        keyedRotation.valueWidth = 4;
        keyedRotation.times = {0.f};
        const glm::quat keyed = glm::angleAxis(.3f, glm::vec3(1,0,0));
        keyedRotation.values = {keyed.x, keyed.y, keyed.z, keyed.w};
        posedSource.clip.channels.push_back(keyedRotation);
        posedSource.baseSkeleton = posedSource.bindSkeleton;
        posedSource.baseSkeleton[0].rotation = {.7f, 0.f, 0.f};
        posedSource.baseSkeleton[1].rotation = {.6f, 0.f, 0.f};
        posedSource.previewPrefab = "Different source avatar.prefab";
        const auto converted = Model::AnimationRigMapping::Map(posedSource, *standaloneModel,
            Model::RigAsset::ForModel(*standaloneModel));
        const auto& rotationValues = converted.clip.channels.front().values;
        const glm::quat actualRotation(rotationValues[3], rotationValues[0], rotationValues[1], rotationValues[2]);
        Check(std::abs(glm::dot(actualRotation, keyed)) > .99999f,
            "Retargeting used the first animated pose instead of the source bind rotations");
        Model::AnimationClip facial;
        facial.clipName = "Facial";
        facial.duration = 1.f;
        Model::AnimationChannel face;
        face.nodeIndex = 0; face.targetPath = "Hips";
        face.path = Model::AnimationChannel::Path::Translation;
        face.times = {0.f, 1.f}; face.values = {0.f, 8.f, 0.f, 0.f, 10.f, 0.f};
        facial.channels.push_back(face);
        timelineManager->clips.push_back(facial);
        Model::AnimationLayer facialLayer;
        facialLayer.clip = facial.clipName;
        timelineManager->layers.push_back(facialLayer);
        Editor::AnimationTimelineEditing::Playback playback;
        Editor::AnimationTimelineEditing::Read(playback, *timelineManager);
        Check(playback.clipIndex == 0 && playback.stackedClips.size() == 1 &&
            playback.stackedClips[0].clipIndex == 1,
            "Opening a prefab discarded its facial layer");
        Editor::AnimationTimelineEditing::Remove(playback, 0);
        Check(playback.clipIndex == -1 && playback.stackedClips.size() == 1,
            "Primary animation could not be removed independently");
        playback.stackedClips[0].startSeconds = 2.f;
        playback.stackedClips[0].repeat = true;
        playback.stackedClips[0].repeatCount = 2;
        Editor::AnimationTimelineEditing::Write(playback, *timelineManager);
        timelineManager->Start();
        timelineManager->timelineTime = 0.f;
        timelineManager->Tick(0.f);
        Check(glm::distance(standaloneModel->ResolveNode(0)->transform.position,
            asset.bindSkeleton[0].position) < .001f, "Delayed clip played before its start");
        Check(glm::distance(standaloneModel->ResolveNode(1)->transform.position,
            asset.bindSkeleton[1].position) < .001f, "Removed primary animation still played");
        timelineManager->timelineTime = 2.5f;
        timelineManager->Tick(0.f);
        Check(std::abs(standaloneModel->ResolveNode(0)->transform.position.y - 9.f) < .001f,
            "Delayed facial layer sampled at the wrong local time");
        timelineManager->timelineTime = 3.5f;
        timelineManager->Tick(0.f);
        Check(std::abs(standaloneModel->ResolveNode(0)->transform.position.y - 9.f) < .001f,
            "Timeline repeat did not sample the second play");
        timelineManager->timelineTime = 4.1f;
        timelineManager->Tick(0.f);
        Check(glm::distance(standaloneModel->ResolveNode(0)->transform.position,
            asset.bindSkeleton[0].position) < .001f, "Finite repeat kept playing after its end");
        Editor::AnimationTimelineEditing::Playback restored;
        Components::AnimationManager serialized;
        serialized.Deserialize(timelineManager->Serialize());
        Editor::AnimationTimelineEditing::Read(restored, serialized);
        Check(restored.clipIndex == -1 && restored.stackedClips.size() == 1 &&
            restored.stackedClips[0].startSeconds == 2.f && restored.stackedClips[0].repeatCount == 2,
            "Model serialization lost removal, timing, or repeat settings");
        const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
        const auto outputDirectory = sourceRoot / ".local/test-output";
        std::filesystem::create_directories(outputDirectory);
        const std::string prefabPath = (outputDirectory / "animation-timeline-roundtrip.prefab").string();
        Check(Serialization::SceneSerializer::SavePrefab(*standaloneRoot, prefabPath),
            "Could not save edited animation prefab");
        Scene::Scene prefabReload;
        auto* prefabRoot = Serialization::SceneSerializer::InstantiatePrefab(prefabReload, prefabPath);
        Check(prefabRoot != nullptr, "Could not reopen edited animation prefab");
        auto* prefabManager = prefabRoot->GetComponent<Components::AnimationManager>();
        Editor::AnimationTimelineEditing::Playback prefabPlayback;
        Editor::AnimationTimelineEditing::Read(prefabPlayback, *prefabManager);
        Check(prefabPlayback.clipIndex == -1 && prefabPlayback.stackedClips.size() == 1 &&
            prefabPlayback.stackedClips[0].startSeconds == 2.f,
            "Prefab XML restored a removed primary or lost timeline timing");
        auto pack = Model::AnimationPackAsset::Capture(*timelineManager);
        pack.sourceTracks.push_back({"facial", "{}"});
        const std::string packPath = (outputDirectory / "animation-timeline-roundtrip.animpack").string();
        std::string packError;
        Check(pack.Save(packPath, packError), "Could not save timeline pack");
        auto savedPack = Model::AnimationPackAsset::Load(packPath, packError);
        Check(savedPack && savedPack->timelinePlayback && !savedPack->baseAnimationEnabled &&
            savedPack->layers.size() == 1 && savedPack->layers[0].startSeconds == 2.f &&
            savedPack->layers[0].repeatCount == 2 && savedPack->sourceTracks.size() == 1,
            "Pack roundtrip lost timeline playback or source tracks");
        Scene::Scene packPreview;
        Check(Serialization::SceneSerializer::LoadFromString(packPreview,
            Editor::AnimationEditorSource::PackPreview(*savedPack)),
            "Pack could not open as its own animation document");
        auto* packManager = packPreview.GetObjects().front()->GetComponent<Components::AnimationManager>();
        Check(packManager && packManager->clips.size() == 2 && packManager->layers.size() == 1,
            "Pack preview lost clips or layers");
        // A valid standalone .anim needs neither matching bones nor source files
        // to enter the rig. Preserve every unresolved row for manual assignment.
        Model::AnimationClipAsset foreign;
        foreign.clip.clipName = "Unmapped motion";
        foreign.clip.duration = 1.f;
        auto foreignTranslation = channel;
        foreignTranslation.targetPath = "OtherRig/UnknownJoint";
        foreignTranslation.nodeIndex = 900;
        foreign.clip.channels.push_back(foreignTranslation);
        auto foreignScale = foreignTranslation;
        foreignScale.path = Model::AnimationChannel::Path::Scale;
        foreignScale.values = {1.f, 1.f, 1.f, 2.f, 2.f, 2.f};
        foreign.clip.channels.push_back(foreignScale);
        const auto foreignPath = (outputDirectory / "unmapped-no-source.anim").string();
        Check(foreign.Save(foreignPath), "Could not save standalone unmapped clip");
        Check(timelineManager->AddAnimationAsset(foreignPath, packError),
            "Dropping an entirely unmapped animation was rejected");
        auto& imported = timelineManager->clips.back();
        const auto modelBones = Editor::AnimationClipMapping::ModelBones(*standaloneModel);
        auto mapping = Editor::AnimationClipMapping::Inspect(imported, modelBones, modelBones);
        Check(mapping.issues.size() == 2 && mapping.compatibleChannels.empty(),
            "Unmapped key data was discarded or applied to arbitrary bones");
        Check(imported.channels[0].values == foreignTranslation.values &&
            imported.channels[1].values == foreignScale.values, "Unmapped import changed key data");
        Check(Editor::AnimationClipMapping::AssignRow(imported, 0, modelBones[1]),
            "Unmapped timeline row could not be assigned");
        mapping = Editor::AnimationClipMapping::Inspect(imported, modelBones, modelBones);
        Check(mapping.issues.empty() && mapping.compatibleChannels.size() == 2,
            "Assigning a row failed to bind all of its transform properties");
        Check(imported.channels[0].times == foreignTranslation.times &&
            imported.channels[0].values == foreignTranslation.values &&
            imported.channels[1].values == foreignScale.values, "Bone assignment changed existing keys");
        Model::AnimationClipAsset assigned;
        assigned.clip = imported;
        const auto assignedPath = (outputDirectory / "assigned-animation.anim").string();
        Check(assigned.Save(assignedPath), "Assigned animation could not be saved");
        const auto assignedReload = Model::AnimationClipAsset::Load(assignedPath);
        Check(assignedReload && Editor::AnimationClipMapping::Inspect(assignedReload->clip,
            modelBones, modelBones).issues.empty(), "Saving lost manual bone assignments");
        foreign.clip.clipName = "Unnamed source rows";
        foreign.clip.channels[0].targetPath.clear();
        foreign.clip.channels[1].targetPath.clear();
        foreign.clip.channels[1].nodeIndex = 901;
        const auto unnamedPath = (outputDirectory / "unnamed-source-rows.anim").string();
        Check(foreign.Save(unnamedPath) && timelineManager->AddAnimationAsset(unnamedPath, packError),
            "Animation with numeric source rows required skeleton metadata");
        auto& unnamed = timelineManager->clips.back();
        Check(unnamed.channels[0].targetPath != unnamed.channels[1].targetPath,
            "Distinct unnamed source rows collapsed into one assignment");
        Check(Editor::AnimationClipMapping::AssignRow(unnamed, 0, modelBones[1]),
            "Unnamed source row could not be assigned");
        mapping = Editor::AnimationClipMapping::Inspect(unnamed, modelBones, modelBones);
        Check(mapping.issues.size() == 1 && mapping.compatibleChannels.size() == 1,
            "Assigning one unnamed row also assigned another source node");
        Editor::AnimationTimelineEditing::EraseClip(restored, 0);
        Check(restored.stackedClips[0].clipIndex == 0,
            "Removing a library clip did not remap the remaining timeline indices");
        Editor::AnimationTimelineEditing::EraseClip(restored, 0);
        Check(restored.stackedClips.empty() && restored.clipIndex == -1,
            "Removing the last library clip left dangling timeline references");
        const std::string mococoPack = (sourceRoot /
            "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/dancing.animpack").string();
        if (std::filesystem::exists(mococoPack))
        {
            auto dancing = Model::AnimationPackAsset::Load(mococoPack, packError);
            Check(dancing && dancing->clips.size() >= 2 && !dancing->layers.empty(),
                "Mococo fixture lost the body/facial playback setup");
            Components::AnimationManager mococo;
            mococo.InstallEmbeddedClips(std::move(dancing->clips));
            mococo.clip = dancing->activeClip; mococo.layers = dancing->layers;
            Editor::AnimationTimelineEditing::Playback mococoPlayback;
            Editor::AnimationTimelineEditing::Read(mococoPlayback, mococo);
            Check(mococoPlayback.clipIndex >= 0 && !mococoPlayback.stackedClips.empty(),
                "Mococo body and facial animations did not both enter the timeline");
        }
        asset.bindSkeleton.resize(2);
        asset.bindSkeleton[0].path = "A"; asset.bindSkeleton[0].parent = "B";
        asset.bindSkeleton[1].path = "B"; asset.bindSkeleton[1].parent = "A";
        bool rejected = false;
        try { Editor::AnimationEditorSource::ClipPreview(asset); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "Cyclic clip hierarchy was accepted");
        Model::AnimationClipAsset empty;
        empty.clip.clipName = "New animation";
        Scene::Scene emptyPreview;
        Check(Serialization::SceneSerializer::LoadFromString(emptyPreview,
            Editor::AnimationEditorSource::ClipPreview(empty)), "Empty animation did not open");
        Scene::Scene firstDocument;
        auto* firstCharacter = Serialization::SceneSerializer::InstantiatePrefab(firstDocument,
            (sourceRoot / "Engine/Core/Assets/Models/mmd/Mococo/Mococo Abyssgard/Mococo Abyssgard.low.prefab").string());
        Check(firstCharacter != nullptr, "Could not open the existing Mococo animation document");
        const auto firstObjectCount = firstDocument.GetObjects().size();
        auto* firstModel = firstCharacter->GetComponent<Components::Model>();
        Scene::Scene secondDocument;
        auto* danceCharacter = Serialization::SceneSerializer::InstantiatePrefab(secondDocument,
            (sourceRoot / "Engine/Core/Assets/Models/mmd/Mococo/Mococo Dance/Mococo Dance.prefab").string());
        Check(danceCharacter != nullptr, "Mococo Dance prefab could not open as a second animation document");
        const auto danceRigs = Editor::AnimationEditorSource::Rigs(danceCharacter);
        Check(!danceRigs.empty() && danceRigs.front()->ResolveModel() &&
            !danceRigs.front()->ResolveJoints().empty(), "Mococo Dance opened without a usable rig");
        Check(firstDocument.GetObjects().size() == firstObjectCount &&
            firstCharacter->GetComponent<Components::Model>() == firstModel &&
            firstModel != danceRigs.front()->ResolveModel(),
            "Opening another prefab replaced the current animation document");
        std::cout << "Animation editor source regressions passed\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
