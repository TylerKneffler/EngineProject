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

void EditorState::DrawAnimationTimelinePanel(IEditorUi& ui)
{
    HandleAnimationEditorDrop(ui, m_timelineAnimationDocument);
    if (!m_animationDocumentOpenError.empty())
    {
        ui.ColoredLabel(m_animationDocumentOpenError.c_str(), {1.f, .5f, .35f, 1.f});
        if (ui.Button("Dismiss open error")) m_animationDocumentOpenError.clear();
    }
    if (!m_timelineAnimationDocument)
    {
        ui.DisabledLabel("Open an animation in the Animation Editor to view its timeline.");
        return;
    }
    if (ui.IsWindowFocused())
        SetActiveAnimationDocument(m_timelineAnimationDocument);
    ui.ValueLabel("Animation", m_timelineAnimationDocument->view->GetTitle().c_str());
    ImGui::PushID(m_timelineAnimationDocument);
    DrawAnimationTimeline(ui, *m_timelineAnimationDocument);
    ImGui::PopID();
}

void EditorState::UpdateAnimationPlayback(float deltaTime)
{
    for (const auto& open : m_animationDocuments)
    {
        auto& document = *open;
        if (!document.playing || !document.sourceManager) continue;
        document.playheadSeconds += std::clamp(deltaTime, 0.f, .1f);
        float duration = .01f;
        bool anyLooping = false;
        for (const auto& track : document.tracks)
            if (track.selected)
            {
                const auto includeClip = [&](int index, float start,
                    bool repeat, int count)
                {
                    if (index >= 0 && index < static_cast<int>(
                            track.sourceManager->clips.size()))
                    {
                        const auto& clip = track.sourceManager->clips[index];
                        duration = std::max(duration, start + clip.duration *
                            (repeat && count > 0 ? count : 1));
                        anyLooping |= repeat && count == 0;
                    }
                };
                includeClip(track.clipIndex, track.startSeconds,
                    track.repeat, track.repeatCount);
                for (const auto& layer : track.stackedClips)
                    includeClip(layer.clipIndex, layer.startSeconds,
                        layer.repeat, layer.repeatCount);
            }
        if (document.playheadSeconds >= duration)
        {
            if (!anyLooping)
            {
                document.playheadSeconds = duration;
                document.playing = false;
            }
        }
        SampleAnimationDocument(document);
    }
}

void EditorState::DrawAnimationKeyTools(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.sourceManager) return;
    if (!document.selectedRows.empty() && ui.Button("Select Row Keys"))
    {
        document.selectedKeys.clear();
        for (const auto& row : document.selectedRows)
        {
            const auto track = std::find_if(document.tracks.begin(),
                document.tracks.end(), [&](const auto& item)
                { return item.index == row.rigIndex; });
            if (track == document.tracks.end() || !track->sourceManager ||
                row.clipIndex < 0 || row.clipIndex >= static_cast<int>(
                    track->sourceManager->clips.size())) continue;
            for (float time : AnimationClipEditing::KeyTimes(
                    track->sourceManager->clips[row.clipIndex], row.nodeIndex))
                document.selectedKeys.push_back({row.rigIndex,
                    row.clipIndex, row.nodeIndex, time});
        }
    }
    ImGui::BeginDisabled(document.selectedKeys.empty());
    if (ui.Button("Copy Keys")) CopyAnimationKeys(document);
    ImGui::EndDisabled();
    if (!document.copiedKeys.empty())
    {
        const auto targetTrack = std::find_if(document.tracks.begin(),
            document.tracks.end(), [&](const auto& item)
            { return item.index == document.copiedKeysRig; });
        if (targetTrack != document.tracks.end() && targetTrack->sourceManager)
        {
            const auto& available = targetTrack->sourceManager->clips;
            const char* destination = document.pasteClipIndex >= 0 &&
                document.pasteClipIndex < static_cast<int>(available.size())
                    ? available[document.pasteClipIndex].clipName.c_str()
                    : "Choose animation";
            ImGui::SetNextItemWidth(180.f);
            ImGui::SetNextWindowSizeConstraints({ 180.f, 0.f },
                { 420.f, FLT_MAX });
            if (ImGui::BeginCombo("Paste into animation",
                    AnimationDropdownLabel(destination).c_str()))
            {
                for (int index = 0; index < static_cast<int>(available.size());
                    ++index)
                {
                    ImGui::PushID(index);
                    if (AnimationDropdownItem(available[index].clipName,
                            index == document.pasteClipIndex))
                        document.pasteClipIndex = index;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (ui.Button("Paste Keys")) PasteAnimationKeys(document);
        }
    }
}

void EditorState::AnimationPlaybackChanged(AnimationDocument& document, bool recordHistory)
{
    if (recordHistory)
    {
        RecordAnimationEdit(document, document.sourceManager->clips);
        return;
    }
    for (const auto& track : document.tracks)
        AnimationTimelineEditing::Write(track, *track.sourceManager);
    document.dirty = true;
    RefreshAnimationDocumentTitle(document);
    SampleAnimationDocument(document);
}

bool EditorState::AddAnimationToDocument(AnimationDocument& document,
    const std::string& path)
{
    if (!document.sourceManager || !document.poseSkeleton) return false;
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [&](const auto& item) { return item.index == document.rigIndex; });
    if (track == document.tracks.end() || !track->sourceSkeleton) return false;
    auto* model = track->sourceSkeleton->ResolveModel();
    if (!model) return false;
    std::vector<Engine::Model::AnimationClip> incoming;
    std::vector<Engine::Model::AnimationLayer> layers;
    std::string incomingPrimary;
    const bool importingPack = std::filesystem::path(path).extension() == ".animpack";
    if (importingPack)
    {
        Engine::Components::AnimationManager mapped;
        mapped.Owner = document.sourceManager->Owner;
        mapped.modelReference = document.sourceManager->modelReference;
        if (!MmdDanceAssetBinder::ApplyForPrefab(document.path, path, mapped, document.error)) return false;
        incoming = std::move(mapped.clips);
        layers = std::move(mapped.layers);
        if (mapped.baseAnimationEnabled || mapped.timelinePlayback) incomingPrimary = mapped.clip;
    }
    else
    {
        Engine::Model::AnimationClipAsset prepared;
        bool rebound = false;
        if (!MmdDanceAssetBinder::LoadClipForPrefab(document.path, path, prepared, rebound, document.error))
            return false;
        const auto* asset = &prepared;
        auto rig = Engine::Model::RigAsset::Capture(*document.sourceScene, *track->sourceSkeleton);
        rig.humanoid = track->humanoid;
        if (auto stored = Engine::Model::RigAsset::Load(track->path, track->sourceSkeleton->skinIndex);
            stored && !stored->bindNodes.empty()) rig.bindNodes = std::move(stored->bindNodes);
        const auto mapped = Engine::Model::AnimationRigMapping::Map(*asset, *model, rig);
        incoming.push_back(mapped.clip);
    }
    auto before = document.sourceManager->clips;
    auto& clips = document.sourceManager->clips;
    bool added = false;
    for (auto& clip : incoming)
    {
        if (std::any_of(clips.begin(), clips.end(), [&](const auto& item)
                { return item.clipName == clip.clipName; })) continue;
        const int index = static_cast<int>(clips.size());
        clips.push_back(std::move(clip));
        if (std::any_of(clips.back().channels.begin(), clips.back().channels.end(),
                [&](const auto& channel) { return channel.nodeIndex >= model->GetNodeCount(); }))
            track->expandedClips[index] = true;
        const auto found = std::find_if(layers.begin(), layers.end(),
            [&](const auto& item) { return item.clip == clips.back().clipName; });
        const bool incomingActive = !importingPack || clips.back().clipName == incomingPrimary || found != layers.end();
        if (incomingActive)
        {
            if (track->clipIndex < 0 && (!importingPack || clips.back().clipName == incomingPrimary))
            {
                track->clipIndex = index;
                if (found != layers.end())
                {
                    track->baseSettings = *found;
                    track->weight = found->weight;
                    track->startSeconds = found->startSeconds;
                    track->repeat = found->looping;
                    track->repeatCount = found->repeatCount;
                }
            }
            else
            {
                AnimationTimelineEditing::Playback::ClipLayer layer;
                layer.clipIndex = index;
                if (found != layers.end())
                {
                    layer.settings = *found;
                    layer.weight = found->weight;
                    layer.startSeconds = found->startSeconds;
                    layer.repeat = found->looping;
                    layer.repeatCount = found->repeatCount;
                }
                track->stackedClips.push_back(std::move(layer));
            }
        }
        track->expandedClips[index] = false;
        added = true;
    }
    if (!added) { document.error = "These animation names are already in the model. Use Add to timeline below."; return false; }
    document.clipIndex = track->clipIndex;
    document.animationAssetToAdd.clear();
    document.error.clear();
    RecordAnimationEdit(document, std::move(before));
    return true;
}

void EditorState::RemoveAnimationFromDocument(AnimationDocument& document,
    size_t rigIndex, int clipIndex)
{
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [&](const auto& item) { return item.index == rigIndex; });
    if (track == document.tracks.end() || clipIndex < 0 ||
        clipIndex >= static_cast<int>(track->sourceManager->clips.size())) return;
    FocusAnimationRig(document, rigIndex);
    auto before = track->sourceManager->clips;
    AnimationTimelineEditing::EraseClip(*track, clipIndex);
    track->sourceManager->clips.erase(track->sourceManager->clips.begin() + clipIndex);
    track->expandedClips.clear();
    document.clipIndex = track->clipIndex;
    document.selectedKeys.clear();
    document.selectedRows.clear();
    document.rowSelectionAnchor.reset();
    document.clipDragging = false;
    document.keyDragging = false;
    document.playing = false;
    RecordAnimationEdit(document, std::move(before));
}

void EditorState::DrawAnimationLayerTools(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.sourceManager) return;
    if (!document.drawingProperties) ImGui::TextUnformatted("Model animations");
    ImGui::TextWrapped("Primary and additional tracks come from this model's Animation Manager. "
        "Remove from timeline keeps the clip available; Remove from model detaches it when you save.");
    Engine::Core::Component::DrawAssetPathPicker(ui, &document,
        "animationAssetToAdd", "Animation / pack", "animation", document.animationAssetToAdd);
    if (ImGui::Button("Add file to model and timeline"))
        AddAnimationToDocument(document, document.animationAssetToAdd);
    int removeClip = -1;
    size_t removeRig = 0;
    for (auto& track : document.tracks)
    {
        if (!track.selected || !track.sourceManager) continue;
        auto& available = track.sourceManager->clips;
        ImGui::PushID(&track);
        ImGui::Separator();
        ImGui::TextUnformatted(track.name.c_str());
        const char* primary = track.clipIndex >= 0 && track.clipIndex < static_cast<int>(available.size())
            ? available[track.clipIndex].clipName.c_str() : "None (rest pose)";
        ImGui::SetNextItemWidth(220.f);
        if (ImGui::BeginCombo("Primary animation", AnimationDropdownLabel(primary).c_str()))
        {
            if (ImGui::Selectable("None (rest pose)", track.clipIndex < 0))
            {
                AnimationTimelineEditing::Remove(track, track.clipIndex);
                FocusAnimationRig(document, track.index);
                document.clipIndex = track.clipIndex;
                AnimationPlaybackChanged(document);
            }
            for (int index = 0; index < static_cast<int>(available.size()); ++index)
            {
                ImGui::PushID(index);
                if (AnimationDropdownItem(available[index].clipName, track.clipIndex == index))
                {
                    AnimationTimelineEditing::Remove(track, index);
                    track.clipIndex = index;
                    track.startSeconds = 0.f;
                    FocusAnimationRig(document, track.index);
                    document.clipIndex = index;
                    document.selectedKeys.clear();
                    document.selectedRows.clear();
                    AnimationPlaybackChanged(document);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        for (int index = 0; index < static_cast<int>(available.size()); ++index)
        {
            ImGui::PushID(index);
            ImGui::TextUnformatted(available[index].clipName.c_str());
            const bool onTimeline = AnimationTimelineEditing::Contains(track, index);
            if (ImGui::SmallButton(onTimeline ? "Remove from timeline" : "Add to timeline"))
            {
                if (onTimeline) AnimationTimelineEditing::Remove(track, index);
                else if (track.clipIndex < 0) track.clipIndex = index;
                else track.stackedClips.push_back({index, 1.f});
                FocusAnimationRig(document, track.index);
                document.clipIndex = track.clipIndex;
                document.selectedKeys.clear();
                document.selectedRows.clear();
                AnimationPlaybackChanged(document);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove from model"))
            { removeClip = index; removeRig = track.index; }
            ImGui::PopID();
        }
        if (available.empty()) ImGui::TextDisabled("No clips. Add an animation or pack above.");
        ImGui::PopID();
    }
    if (removeClip >= 0) RemoveAnimationFromDocument(document, removeRig, removeClip);
    ImGui::Separator();
    auto& packPath = document.sourceManager->packExportPath;
    char path[1024]{};
    std::snprintf(path, sizeof(path), "%s", packPath.c_str());
    if (ImGui::InputTextWithHint("Pack file", "path/to/animations.animpack", path, sizeof(path)))
        packPath = path;
    if (ImGui::Button("Save animation pack"))
    {
        if (std::filesystem::path(packPath).extension() != ".animpack")
            document.error = "Enter an .animpack file path.";
        else
        {
            for (const auto& track : document.tracks)
                if (track.index == document.rigIndex)
                    AnimationTimelineEditing::Write(track, *document.sourceManager);
            auto pack = Engine::Model::AnimationPackAsset::Capture(*document.sourceManager);
            std::string ignored;
            if (auto previous = Engine::Model::AnimationPackAsset::Load(packPath, ignored))
                pack.sourceTracks = std::move(previous->sourceTracks);
            if (pack.Save(packPath, document.error)) document.error.clear();
        }
    }
    if (!document.error.empty()) ImGui::TextWrapped("%s", document.error.c_str());
}

void EditorState::DrawAnimationTimeline(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.sourceManager)
    {
        ui.DisabledLabel("Drop a prefab, scene object, or .anim onto the Animation Editor.");
        return;
    }
    ImGui::Text("%.3f s", document.playheadSeconds);
    ImGui::SameLine();
    ImGui::TextDisabled("Rows: click / Ctrl toggle / Shift range | Keys: Ctrl click / right-drag select");
    const bool hasVisibleAnimation = std::any_of(document.tracks.begin(),
        document.tracks.end(), [](const auto& track)
        { return track.selected && track.sourceManager &&
            !track.sourceManager->clips.empty(); });
    if (!hasVisibleAnimation)
    {
        ui.DisabledLabel("Use File > New > Animation to create a clip.");
        return;
    }
    float timelineDuration = .01f;
    for (const auto& track : document.tracks)
        if (track.selected)
        {
            const auto& available = track.sourceManager->clips;
            const auto includeClip = [&](int index, float start,
                bool repeat, int count)
            {
                if (index >= 0 && index < static_cast<int>(available.size()))
                    timelineDuration = std::max(timelineDuration,
                        start + available[index].duration *
                        (repeat && count > 0 ? count : 1));
            };
            includeClip(track.clipIndex, track.startSeconds,
                track.repeat, track.repeatCount);
            for (const auto& layer : track.stackedClips)
                includeClip(layer.clipIndex, layer.startSeconds,
                    layer.repeat, layer.repeatCount);
        }
    DrawAnimationGraph(document, timelineDuration);
}


}
