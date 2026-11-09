#include "pch.h"
#include "Engine/Editor/State/EditorState.h"
#include "Core/Animation/AnimationDocumentAccess.h"
#include "Core/Animation/AnimationTimelineVisuals.h"
#include "Core/Animation/AnimationPropertySearch.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/Animation/AnimationClipEditing.h"
#include "Core/Animation/AnimationClipMapping.h"
#include "Core/Animation/AnimationRigSelection.h"
#include "Core/Animation/AnimationEditorSource.h"
#include "Core/Importers/MmdDanceAssetBinder.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/Views/AnimationView.h"
#include "Core/View/Views/GameView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Engine/Editor/UI/EditorIcons.h"
#include "Core/Animation/SkeletonBindPose.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include "Core/Compoonents/Animation/TwoBoneIK.h"
#include "Core/Compoonents/Animation/ChainIK.h"
#include "Core/Model/AnimationControlSpace.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/ComponentReference.h"
#include "Core/Model/RigAsset.h"
#include "Core/Model/PoseAsset.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Model/AnimationRigMapping.h"
#include "Core/Object.h"
#include "Core/Prefab/PrefabAsset.h"
#include "imgui.h"
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Editor
{
using namespace AnimationDocumentAccess;
using namespace AnimationTimelineVisuals;
using namespace AnimationPropertySearch;

bool EditorState::StepAnimationHistory(AnimationDocument& document, bool redo)
{
    auto& source = redo ? document.redo : document.undo;
    auto& target = redo ? document.undo : document.redo;
    if (source.empty()) return false;
    auto snapshot = std::move(source.back());
    source.pop_back();
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [&](const auto& item) { return item.index == snapshot.rigIndex; });
    if (track == document.tracks.end()) return false;
    target.push_back({track->index, track->sourceManager->clips, *track});
    track->sourceManager->InstallEmbeddedClips(std::move(snapshot.clips));
    static_cast<AnimationTimelineEditing::Playback&>(*track) = std::move(snapshot.playback);
    AnimationTimelineEditing::Write(*track, *track->sourceManager);
    FocusAnimationRig(document, track->index);
    document.clipIndex = track->clipIndex;
    document.selectedRows.clear();
    track->expandedClips.clear();
    document.selectedKeys.clear();
    ++document.clipRevision;
    document.clipCache.clear();
    document.previewLayouts.clear();
    document.dirty = document.sourceScene->SaveSnapshotToString() !=
        document.savedSnapshot || std::any_of(document.tracks.begin(),
            document.tracks.end(), [](const auto& track)
            { return track.controlsDirty || track.rigConstraintsDirty; });
    RefreshAnimationDocumentTitle(document);
    SampleAnimationDocument(document);
    return true;
}

bool EditorState::SaveAnimationDocument(AnimationDocument& document)
{
    const bool constraintsEdited = std::any_of(document.tracks.begin(), document.tracks.end(),
        [&](const auto& track) { return track.index == document.rigIndex && track.rigConstraintsDirty; });
    const auto copyConstraintSettings = [&](Engine::Components::Model* target)
    {
        auto* source = document.sourceManager ? ManagerModel(*document.sourceManager) : nullptr;
        auto* sourceConstraints = source && source->Owner
            ? source->Owner->GetComponent<Engine::Components::RigConstraints>() : nullptr;
        if (!constraintsEdited || !sourceConstraints || !target || !target->Owner) return;
        auto* destination = target->Owner->GetComponent<Engine::Components::RigConstraints>();
        if (!destination) destination = target->Owner->AddComponent<Engine::Components::RigConstraints>();
        destination->Deserialize(sourceConstraints->Serialize());
    };

    for (const auto& track : document.tracks)
        AnimationTimelineEditing::Write(track, *track.sourceManager);
    if (document.sourceObject)
    {
        const auto contains = [&](Engine::Scene::Scene* scene)
        {
            return scene && std::any_of(scene->GetObjects().begin(), scene->GetObjects().end(),
                [&](const auto& object) { return object.get() == document.sourceObject; });
        };
        Engine::Scene::Scene* destination = nullptr;
        if (contains(m_scene.get())) destination = m_scene.get();
        else if (contains(m_prefabScene.get())) destination = m_prefabScene.get();
        for (const auto& open : m_sceneAssetDocuments)
            if (contains(open->scene.get())) destination = open->scene.get();
        if (!destination)
        { document.error = "The source object was removed. Drop its replacement into the editor."; return false; }
        const auto rigs = AnimationEditorSource::Rigs(document.sourceObject);
        if (document.rigIndex >= rigs.size() || !document.sourceManager)
        { document.error = "The source skeleton changed. Reopen the object to edit its animations."; return false; }
        bool ambiguous = false;
        auto* manager = ManagerForRig(*destination, rigs[document.rigIndex], &ambiguous);
        if (ambiguous)
        { document.error = "Multiple Animation Managers target the source rig."; return false; }
        if (!manager)
        {
            manager = rigs[document.rigIndex]->Owner->AddComponent<Engine::Components::AnimationManager>();
            manager->modelReference = Engine::Core::CaptureComponentReference(
                rigs[document.rigIndex]->ResolveModel(), "Model");
            manager->clip = document.sourceManager->clip;
        }
        for (auto& track : document.tracks)
            if (track.index == document.rigIndex && track.controlsDirty)
            {
                auto rig = Engine::Model::RigAsset::Capture(*document.sourceScene,
                    *RigAt(*document.sourceScene, document.rigIndex));
                rig.controls = track.controls; rig.humanoid = track.humanoid;
                rig.poses = track.poses; rig.symmetryPairs = track.symmetryPairs;
                if (track.path.empty() || !rig.Save(track.path))
                { document.error = "Could not save the rig mapping."; return false; }
                track.controlsDirty = false;
            }
        manager->InstallEmbeddedClips(document.sourceManager->clips);
        manager->clip = document.sourceManager->clip;
        manager->baseAnimationEnabled = document.sourceManager->baseAnimationEnabled;
        manager->timelinePlayback = document.sourceManager->timelinePlayback;
        manager->timelineTime = 0.f;
        manager->layers = document.sourceManager->layers;
        copyConstraintSettings(ManagerModel(*manager));
        for (auto& track : document.tracks)
            if (track.index == document.rigIndex) track.rigConstraintsDirty = false;
        manager->MarkConfigurationDirty();
        document.sourceObject->InvalidatePrefabOverrideCache();
        if (destination == m_scene.get())
        { m_hasUnsavedChanges = true; MarkSceneEdited(); RefreshSceneDocumentTitle(); }
        else if (destination == m_prefabScene.get()) SetPrefabDirty(true);
        for (const auto& open : m_sceneAssetDocuments)
            if (open->scene.get() == destination)
            { open->dirty = true; RefreshSceneAssetDocumentTitle(*open); }
        document.savedSnapshot = document.sourceScene->SaveSnapshotToString();
        document.dirty = std::any_of(document.tracks.begin(), document.tracks.end(),
            [](const auto& track) { return track.controlsDirty || track.rigConstraintsDirty; });
        document.error.clear(); RefreshAnimationDocumentTitle(document);
        return true;
    }
    if (std::filesystem::path(document.standaloneAssetPath).extension() == ".animpack")
    {
        std::string error;
        auto previous = Engine::Model::AnimationPackAsset::Load(document.standaloneAssetPath, error);
        auto pack = Engine::Model::AnimationPackAsset::Capture(*document.sourceManager);
        if (previous) pack.sourceTracks = std::move(previous->sourceTracks);
        if (!pack.Save(document.standaloneAssetPath, document.error)) return false;
        document.savedSnapshot = document.sourceScene->SaveSnapshotToString();
        document.dirty = false;
        RefreshAnimationDocumentTitle(document);
        return true;
    }
    if (!document.standaloneAssetPath.empty() && document.sourceManager)
    {
        if (document.sourceManager->clips.empty())
        { document.error = "A standalone animation file must contain a clip. Remove its reference from the model instead."; return false; }
        for (const auto& clip : document.sourceManager->clips)
        {
            const auto known = document.clipAssetPaths.find(clip.clipName);
            std::string path = known != document.clipAssetPaths.end() ? known->second :
                document.sourceManager->clips.size() == 1 ? document.standaloneAssetPath : "";
            if (path.empty())
            {
                std::string name = clip.clipName;
                for (char& ch : name)
                    if (std::string("<>:\"/\\|?*").find(ch) != std::string::npos) ch = '_';
                if (name.empty()) name = "Animation";
                const auto directory = std::filesystem::path(document.standaloneAssetPath).parent_path();
                for (size_t suffix = 0;; ++suffix)
                {
                    const auto candidate = directory / (name + (suffix ? "_" + std::to_string(suffix) : "") + ".anim");
                    if (!std::filesystem::exists(candidate)) { path = candidate.string(); break; }
                }
            }
            auto asset = Engine::Model::AnimationClipAsset::Load(path).value_or(Engine::Model::AnimationClipAsset{});
            asset.clip = clip;
            for (const auto& track : document.tracks)
                if (track.index == document.rigIndex) asset.humanoid = track.humanoid;
            if (asset.baseSkeleton.empty())
                asset.baseSkeleton = Engine::Model::AnimationClipAsset::CaptureBaseSkeleton(
                    *RigAt(*document.sourceScene, document.rigIndex)->ResolveModel());
            if (asset.bindSkeleton.empty())
                asset.bindSkeleton = Engine::Model::AnimationClipAsset::CaptureBaseSkeleton(
                    *RigAt(*document.sourceScene, document.rigIndex)->ResolveModel());
            if (!asset.Save(path)) { document.error = "Could not save animation: " + path; return false; }
            document.clipAssetPaths[clip.clipName] = path;
        }
        document.savedSnapshot = document.sourceScene->SaveSnapshotToString();
        document.dirty = false; document.error.clear();
        RefreshAnimationDocumentTitle(document);
        return true;
    }
    if (document.path.empty()) return true;
    Engine::Scene::Scene latest;
    latest.Init(m_renderer->GetGraphicsProvider());
    auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        latest, document.path, latest.GetGraphicsProvider());
    auto* latestSkeleton = root ? RigAt(latest, document.rigIndex)
        : nullptr;
    auto* latestModel = latestSkeleton ? latestSkeleton->ResolveModel() : nullptr;
    bool latestManagerAmbiguous = false;
    auto* latestManager = ManagerForRig(latest, latestSkeleton,
        &latestManagerAmbiguous);
    if (latestManagerAmbiguous)
    {
        document.error = "Multiple Animation Managers target this rig.";
        return false;
    }
    if (!root || !latestModel || !document.sourceManager)
    {
        document.error = "Could not reload the prefab for animation save.";
        return false;
    }
    if (!latestManager)
    {
        latestManager = latestSkeleton->Owner->AddComponent<
            Engine::Components::AnimationManager>();
        latestManager->modelReference = Engine::Core::CaptureComponentReference(
            latestModel, "Model");
    }
    std::string rigPath = latestSkeleton->rigPath;
    if (rigPath.empty())
    {
        std::filesystem::path destination =
            std::filesystem::path(document.path).parent_path() /
            (std::filesystem::path(document.path).stem().string() +
             "_rig_" + std::to_string(document.rigIndex) + ".rig");
        const std::filesystem::path assets =
            m_projectSettings.assetsDirectory.empty()
                ? std::filesystem::path("Assets")
                : std::filesystem::path(m_projectSettings.assetsDirectory);
        std::error_code pathError;
        const auto assetsAbsolute = std::filesystem::weakly_canonical(
            assets, pathError);
        const auto rigAbsolute = std::filesystem::weakly_canonical(
            destination, pathError);
        if (!pathError)
        {
            const auto relative = rigAbsolute.lexically_relative(
                assetsAbsolute);
            if (!relative.empty() && *relative.begin() != "..")
                destination = assets / relative;
        }
        rigPath = destination.generic_string();
    }
    latestSkeleton->rigPath = rigPath;
    auto rig = Engine::Model::RigAsset::Capture(latest, *latestSkeleton);
    rig.prefabPath = document.path;
    const auto previousRig = Engine::Model::RigAsset::Load(
        rigPath, latestSkeleton->skinIndex);
    for (const auto& track : document.tracks)
        if (track.index == document.rigIndex)
        {
            rig.controls = track.controls;
            rig.humanoid = track.humanoid;
            rig.poses = track.poses;
            rig.symmetryPairs = track.symmetryPairs;
            break;
        }
    std::vector<std::string> animationPaths;
    for (size_t index = 0; index < document.sourceManager->clips.size(); ++index)
    {
        auto animation = document.sourceManager->clips[index];
        for (auto& channel : animation.channels)
            if (channel.targetPath.empty())
                channel.targetPath = AnimationClipMapping::NodePath(
                    *latestModel, channel.nodeIndex);
        std::string animationPath;
        if (const auto direct = document.clipAssetPaths.find(animation.clipName);
            direct != document.clipAssetPaths.end()) animationPath = direct->second;
        if (animationPath.empty() && previousRig)
            for (const auto& candidate : previousRig->animationPaths)
                if (auto existing = Engine::Model::AnimationClipAsset::Load(candidate);
                    existing && existing->clip.clipName == animation.clipName)
                { animationPath = candidate; break; }
        if (animationPath.empty())
        {
            std::string safeName = animation.clipName;
            for (char& character : safeName)
                if (!std::isalnum(static_cast<unsigned char>(character)) &&
                    character != '-' && character != '_') character = '_';
            if (safeName.empty()) safeName = "Animation";
            const std::string stem = std::filesystem::path(rigPath).stem()
                .string() + "_" + safeName + "_" +
                std::to_string(index + 1);
            for (size_t suffix = 0;; ++suffix)
            {
                const auto candidate = std::filesystem::path(rigPath)
                    .parent_path() / (stem + (suffix == 0 ? "" :
                        "_" + std::to_string(suffix)) + ".anim");
                if (!std::filesystem::exists(candidate))
                { animationPath = candidate.generic_string(); break; }
            }
        }
        Engine::Model::AnimationClipAsset asset;
        if (auto existing = Engine::Model::AnimationClipAsset::Load(animationPath))
            asset = std::move(*existing);
        asset.clip = std::move(animation);
        asset.humanoid = rig.humanoid;
        // These channels now address the edited avatar, even if the clip was
        // originally retargeted from another prefab.
        asset.previewPrefab = document.path;
        asset.previewRig = document.rigIndex;
        asset.baseSkeleton = Engine::Model::AnimationClipAsset::
            CaptureBaseSkeleton(*latestModel);
        asset.bindSkeleton = rig.bindNodes.empty() ? asset.baseSkeleton : rig.bindNodes;
        if (!asset.Save(animationPath))
        {
            document.error = "Could not save animation asset: " + animationPath;
            return false;
        }
        animationPaths.push_back(std::move(animationPath));
    }
    rig.animationPaths = std::move(animationPaths);
    if (!rig.Save(rigPath))
    {
        document.error = "Could not save rig animation references.";
        return false;
    }
    latestManager->InstallEmbeddedClips(document.sourceManager->clips);
    latestManager->rigPaths.push_back(rigPath);
    latestManager->clip = document.sourceManager->clip;
    latestManager->baseAnimationEnabled = document.sourceManager->baseAnimationEnabled;
    latestManager->timelinePlayback = document.sourceManager->timelinePlayback;
    latestManager->timelineTime = 0.f;
    latestManager->layers = document.sourceManager->layers;
    latestManager->RefreshRigClips();
    if (latestManager->baseAnimationEnabled && latestManager->clip.empty() && !latestManager->clips.empty())
        latestManager->clip = latestManager->clips.front().clipName;
    copyConstraintSettings(latestModel);
    root->Prefab.reset();
    if (!Engine::Serialization::SceneSerializer::SavePrefab(
            *root, document.path))
    {
        document.error = "Could not save the animation prefab.";
        return false;
    }
    if (auto* sourceSkeleton = RigAt(*document.sourceScene,
            document.rigIndex))
        sourceSkeleton->rigPath = rigPath;
    document.rigPath = rigPath;
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex)
        { track.path = rigPath; track.controlsDirty = false; track.rigConstraintsDirty = false; break; }
    document.savedSnapshot = document.sourceScene->SaveSnapshotToString();
    document.dirty = std::any_of(document.tracks.begin(), document.tracks.end(),
        [](const auto& track) { return track.controlsDirty || track.rigConstraintsDirty; });
    document.error.clear();
    RefreshAnimationDocumentTitle(document);
    // A model Asset Stage may still be open on this prefab. Keep its manager
    // in sync so a later bind-pose save cannot overwrite authored clips.
    for (const auto& open : m_sceneAssetDocuments)
    {
        if (!open || !open->scene) continue;
        const std::string stagePath = open->stageDataPath.empty()
            ? open->path : open->stageDataPath;
        if (AnimationIdentity(stagePath) != AnimationIdentity(document.path))
            continue;
        auto* skeleton = RigAt(*open->scene, document.rigIndex);
        auto* model = skeleton ? skeleton->ResolveModel() : nullptr;
        if (!model) continue;
        skeleton->rigPath = rigPath;
        bool managerAmbiguous = false;
        auto* manager = ManagerForRig(*open->scene, skeleton,
            &managerAmbiguous);
        if (managerAmbiguous) continue;
        if (!manager)
        {
            manager = skeleton->Owner->AddComponent<
                Engine::Components::AnimationManager>();
            manager->modelReference = Engine::Core::CaptureComponentReference(
                model, "Model");
        }
        manager->InstallEmbeddedClips(document.sourceManager->clips);
        manager->clip = document.sourceManager->clip;
        manager->baseAnimationEnabled = document.sourceManager->baseAnimationEnabled;
        manager->timelinePlayback = document.sourceManager->timelinePlayback;
        manager->timelineTime = 0.f;
        manager->layers = document.sourceManager->layers;
        copyConstraintSettings(model);
        if (manager->baseAnimationEnabled && manager->clip.empty() && !manager->clips.empty())
            manager->clip = manager->clips.front().clipName;
        if (!open->dirty)
        {
            open->savedSnapshot = CaptureSceneAssetDocumentSnapshot(*open);
            open->baseline = open->savedSnapshot;
        }
        RefreshSceneAssetDocumentTitle(*open);
    }
    if (m_prefabScene && !m_activePrefabPath.empty() &&
        AnimationIdentity(m_activePrefabPath) == AnimationIdentity(document.path))
    {
        auto* skeleton = RigAt(*m_prefabScene, document.rigIndex);
        auto* model = skeleton ? skeleton->ResolveModel() : nullptr;
        bool managerAmbiguous = false;
        auto* manager = ManagerForRig(*m_prefabScene, skeleton,
            &managerAmbiguous);
        if (skeleton) skeleton->rigPath = rigPath;
        if (managerAmbiguous) manager = nullptr;
        if (!manager && !managerAmbiguous)
        {
            if (model)
            {
                manager = skeleton->Owner->AddComponent<
                    Engine::Components::AnimationManager>();
                manager->modelReference = Engine::Core::CaptureComponentReference(
                    model, "Model");
            }
        }
        if (manager)
        {
            manager->clips = document.sourceManager->clips;
            manager->rigPaths = document.sourceManager->rigPaths;
        }
    }
    if (m_scene)
        Engine::Serialization::SceneSerializer::RefreshPrefabInstances(
            *m_scene, document.path, m_scene->GetGraphicsProvider());
    if (m_viewFactory && m_viewFactory->OnAssetContentsChanged)
        m_viewFactory->OnAssetContentsChanged(document.path);
    return true;
}

void EditorState::RefreshAnimationDocumentTitle(AnimationDocument& document)
{
    const std::string filename =
        document.sourceObject && document.sourceRoot ? document.sourceRoot->name :
        std::filesystem::path(document.standaloneAssetPath.empty() ? document.path :
            document.standaloneAssetPath).filename().string();
    if (document.view)
        document.view->SetTitle("Animation: " + filename + " / " +
            document.rigName +
            (document.dirty ? " *" : "") +
            "###AnimationDocument:" + document.identity);
}

bool EditorState::AssignAnimationChannels(AnimationDocument& document,
    size_t rigIndex, int clipIndex, size_t channelIndex, size_t boneIndex)
{
    const auto track = std::find_if(document.tracks.begin(),
        document.tracks.end(), [rigIndex](const auto& candidate)
        { return candidate.index == rigIndex; });
    if (track == document.tracks.end() || !track->sourceManager ||
        !track->poseSkeleton || clipIndex < 0 ||
        clipIndex >= static_cast<int>(track->sourceManager->clips.size()))
        return false;
    auto* model = track->poseSkeleton->ResolveModel();
    if (!model) return false;
    const auto bones = AnimationClipMapping::RigNodes(*track->poseSkeleton);
    if (boneIndex >= bones.size()) return false;
    auto& clips = track->sourceManager->clips;
    auto& clip = clips[clipIndex];
    if (channelIndex >= clip.channels.size()) return false;
    auto before = clips;
    if (!AnimationClipMapping::AssignRow(clip, channelIndex, bones[boneIndex])) return false;
    document.repairIssueChannel = -1;
    document.repairAwaitingViewportBone = false;
    document.error.clear();
    RecordAnimationEdit(document, std::move(before));
    return true;
}


}
