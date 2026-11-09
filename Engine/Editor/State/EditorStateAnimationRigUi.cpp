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

void EditorState::DrawAnimationMappingRepair(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return;
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex &&
            CachedAnimationClip(document, track, document.clipIndex).mappingReady)
            return;
    auto* model = document.poseSkeleton->ResolveModel();
    if (!model) return;
    auto& clips = document.sourceManager->clips;
    auto& clip = clips[document.clipIndex];
    const auto rigBones = AnimationClipMapping::RigNodes(
        *document.poseSkeleton);
    const auto mapping = AnimationClipMapping::Inspect(clip, rigBones,
        AnimationClipMapping::ModelBones(*model));
    if (mapping.issues.empty()) return;
    ImGui::Separator();
    ImGui::TextColored({ 1.f, .55f, .3f, 1.f },
        "%zu unresolved bone channel%s", mapping.issues.size(),
        mapping.issues.size() == 1 ? "" : "s");
    if (!AnimationPropertiesHeader(document, "Repair bone mappings",
            ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ui.Button("Confirm existing indices for this rig"))
    {
        auto before = clips;
        bool repaired = false;
        for (const auto& issue : mapping.issues)
            if (issue.kind == AnimationClipMapping::IssueKind::Unidentified)
                for (const auto& bone : rigBones)
                    if (bone.nodeIndex == issue.nodeIndex)
                        repaired |= AnimationClipMapping::Repair(clip,
                            issue.channelIndex, bone);
        if (repaired)
        {
            document.error.clear();
            RecordAnimationEdit(document, std::move(before));
            SampleAnimationDocument(document);
        }
    }
    ui.DisabledLabel("Choose a channel and target bone to repair other mappings.");
    if (ImGui::BeginChild("##UnresolvedBoneChannels", { 0.f, 140.f }, true))
        for (const auto& issue : mapping.issues)
        {
            const char* reason = issue.kind ==
                    AnimationClipMapping::IssueKind::Unidentified
                ? "unverified" : issue.kind ==
                    AnimationClipMapping::IssueKind::Ambiguous
                ? "ambiguous" : issue.kind ==
                    AnimationClipMapping::IssueKind::IndexMismatch
                ? "index differs" : "missing";
            const std::string label = "Channel " +
                std::to_string(issue.channelIndex + 1) + " / " +
                (issue.targetPath.empty() ? "node " +
                    std::to_string(issue.nodeIndex) : issue.targetPath) +
                " / " + reason;
            if (ui.Selectable(label.c_str(),
                    document.repairIssueChannel ==
                        static_cast<int>(issue.channelIndex)))
            {
                document.repairIssueChannel =
                    static_cast<int>(issue.channelIndex);
                document.repairBoneIndex = 0;
                for (size_t bone = 0; bone < rigBones.size(); ++bone)
                    if (rigBones[bone].path == issue.targetPath ||
                        rigBones[bone].nodeIndex == issue.nodeIndex)
                    { document.repairBoneIndex = static_cast<int>(bone); break; }
            }
        }
    ImGui::EndChild();
    if (rigBones.empty()) return;
    const bool selectedIssue = std::any_of(mapping.issues.begin(),
        mapping.issues.end(), [&](const auto& issue)
        { return issue.channelIndex ==
            static_cast<size_t>(document.repairIssueChannel); });
    if (!selectedIssue) return;
    std::vector<const char*> names;
    names.reserve(rigBones.size());
    for (const auto& bone : rigBones) names.push_back(bone.path.c_str());
    document.repairBoneIndex = std::clamp(document.repairBoneIndex, 0,
        static_cast<int>(names.size()) - 1);
    ui.Combo("Target bone", &document.repairBoneIndex, names.data(),
        static_cast<int>(names.size()));
    if (ui.Button("Apply bone mapping"))
        AssignAnimationChannels(document, document.rigIndex,
            document.clipIndex, document.repairIssueChannel,
            document.repairBoneIndex);
    ImGui::SameLine();
    if (ui.Button("Pick bone in 3D"))
    {
        document.repairAwaitingViewportBone = true;
        document.repairRigIndex = document.rigIndex;
        document.repairClipIndex = document.clipIndex;
        document.view->SetEditMode(SceneEditMode::Skeleton);
        document.view->SetPoseEditing(true);
        document.view->SetModelManipulation(true);
        document.view->SetModelSelectionMode(1);
        document.poseScene->SetEditorBonesVisible(true);
    }
    if (document.repairAwaitingViewportBone)
        ui.DisabledLabel("Click a bone in the 3D view to assign it.");
}

void EditorState::DrawAnimationRigToolbar(IEditorUi& ui,
    AnimationDocument& document)
{
    ui.ValueLabel("Animation Editor", document.sourceManager
        ? document.rigName.c_str() : "Drop a prefab, scene object, or .anim here");
}

void EditorState::DrawAnimationRigControls(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.poseSkeleton) return;
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [&](const auto& item) { return item.index == document.rigIndex; });
    if (track == document.tracks.end()) return;
    auto& controls = track->controls;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    const auto& palette = document.poseSkeleton->jointNodes;
    if (AnimationPropertiesHeader(document, "Humanoid animation mapping"))
    {
        namespace H = Engine::Model::HumanoidRig;
        auto& mapping = track->humanoid;
        auto* model = document.poseSkeleton->ResolveModel();
        if (!model) { ui.DisabledLabel("This skeleton has no model to map."); return; }
        auto stored = Engine::Model::RigAsset::Load(track->path, document.poseSkeleton->skinIndex);
        const auto rest = stored && !stored->bindNodes.empty() ? stored->bindNodes :
            Engine::Model::AnimationClipAsset::CaptureBaseSkeleton(*model);
        const auto nodes = H::Nodes(rest, palette);
        bool edited = false;
        ui.DisabledLabel("Assign body parts once, then reuse this rig for animations with different bone names. Detection uses rest positions and parent chains.");
        static const char* axes[] = {"X", "Y", "Z"};
        edited = ui.Combo("Rest-pose up axis", &mapping.upAxis, axes, 3) || edited;
        edited = ui.Combo("Rest-pose forward axis", &mapping.forwardAxis, axes, 3) || edited;
        edited = ui.Checkbox("Left is positive side", &mapping.leftPositive) || edited;
        if (ui.Button("Detect unassigned body parts"))
        {
            const auto detection = H::Detect(nodes, mapping);
            for (size_t role = 0; role < H::Count; ++role)
                if (mapping.bones[role] < 0)
                {
                    mapping.bones[role] = detection.mapping.bones[role];
                    track->humanoidConfidence[role] = detection.confidence[role];
                }
            edited = true;
        }
        ui.SameLine();
        if (ui.Button("Clear body assignments"))
        { mapping.bones.fill(-1); track->humanoidConfidence.fill(0.f); edited = true; }
        ui.SearchInput("##HumanoidBoneSearch", track->humanoidSearch,
            sizeof(track->humanoidSearch), "Filter bone paths");
        for (size_t role = 0; role < H::Count; ++role)
        {
            ImGui::PushID(int(role));
            const int current = mapping.bones[role];
            const std::string path = current >= 0 && model->ResolveNode(unsigned(current))
                ? AnimationClipMapping::NodePath(*model, unsigned(current)) : "Unassigned";
            if (ImGui::BeginCombo(H::Labels[role], path.c_str()))
            {
                if (ImGui::Selectable("Unassigned", current < 0))
                { mapping.bones[role] = -1; edited = true; }
                for (size_t index = 0; index < palette.size() && index < joints.size(); ++index)
                    if (joints[index])
                    {
                        const auto bonePath = AnimationClipMapping::NodePath(*model, palette[index]);
                        if (track->humanoidSearch[0] && LowerText(bonePath).find(
                                LowerText(track->humanoidSearch)) == std::string::npos) continue;
                        ImGui::PushID(int(index));
                        const std::string label = "[" + std::to_string(palette[index]) + "] " + bonePath;
                        if (ImGui::Selectable(label.c_str(), current == int(palette[index])))
                        { mapping.bones[role] = int(palette[index]); track->humanoidConfidence[role] = 1.f; edited = true; }
                        ImGui::PopID();
                    }
                ImGui::EndCombo();
            }
            const bool selected = document.selectedBone >= 0 && size_t(document.selectedBone) < palette.size();
            ImGui::BeginDisabled(!selected);
            if (ImGui::SmallButton("Assign selected bone"))
            {
                mapping.bones[role] = int(palette[document.selectedBone]);
                track->humanoidConfidence[role] = 1.f; edited = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(current < 0);
            if (ImGui::SmallButton("Select assigned bone"))
                if (const auto found = std::find(palette.begin(), palette.end(), unsigned(current)); found != palette.end())
                {
                    document.selectedBone = int(found - palette.begin());
                    auto* object = model->ResolveNode(unsigned(current));
                    document.poseScene->SetSelectedObject(object);
                    if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
                    if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
                    if (m_renderer) m_renderer->MarkDirty();
                }
            ImGui::EndDisabled();
            if (track->humanoidConfidence[role] > 0.f && track->humanoidConfidence[role] < 1.f)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("Suggested; review assignment");
            }
            ImGui::PopID();
        }
        const auto issues = H::Validate(mapping, nodes);
        if (issues.empty()) ui.DisabledLabel("Major humanoid chains are assigned and connected.");
        else for (const auto& issue : issues)
            ui.ColoredLabel(issue.c_str(), {1.f,.65f,.3f,1.f});
        if (edited)
        {
            track->controlsDirty = true; document.dirty = true;
            RefreshAnimationDocumentTitle(document);
        }
        if (ui.Button("Save humanoid mapping")) SaveAnimationDocument(document);
        ui.DisabledLabel("Save on both source and target rigs. Their exported .anim files retain the source body assignments. Hair, tails, props, and unusual chains can stay unassigned.");
    }
    if (!AnimationPropertiesHeader(document, "Viewport rig controls")) return;
    const auto labelFor = [&](int node) -> std::string
    {
        for (size_t i = 0; i < palette.size() && i < joints.size(); ++i)
            if (static_cast<int>(palette[i]) == node && joints[i])
                return joints[i]->name;
        return "None";
    };
    bool changed = false;
    if (ui.Button("Use humanoid preset"))
    {
        controls = HumanoidControls(*document.poseSkeleton, &track->humanoid);
        changed = true;
    }
    const auto field = [&](const char* label, int& node)
    {
        const std::string current = labelFor(node);
        if (!ImGui::BeginCombo(label, current.c_str())) return false;
        bool edited = false;
        if (ImGui::Selectable("None", node < 0))
        { node = -1; edited = true; }
        for (size_t i = 0; i < palette.size() && i < joints.size(); ++i)
            if (joints[i])
            {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(joints[i]->name.c_str(),
                        node == static_cast<int>(palette[i])))
                { node = static_cast<int>(palette[i]); edited = true; }
                ImGui::PopID();
            }
        ImGui::EndCombo();
        return edited;
    };
    changed = field("Root", controls.root) || changed;
    changed = field("Pelvis", controls.pelvis) || changed;
    static const char* names[] = {"Left arm", "Right arm", "Left leg", "Right leg"};
    for (size_t index = 0; index < controls.limbs.size(); ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::TreeNode(names[index]))
        {
            auto& limb = controls.limbs[index];
            changed = field("Upper", limb.upper) || changed;
            changed = field("Lower", limb.lower) || changed;
            changed = field("Hand / foot", limb.tip) || changed;
            const auto resolve = [&](int node) -> Engine::Core::Object*
            {
                for (size_t i = 0; i < palette.size() && i < joints.size(); ++i)
                    if (static_cast<int>(palette[i]) == node) return joints[i];
                return nullptr;
            };
            auto* upper = resolve(limb.upper);
            auto* lower = resolve(limb.lower);
            auto* tip = resolve(limb.tip);
            if (limb.upper >= 0 && limb.lower >= 0 && limb.tip >= 0 &&
                (!upper || !lower || !tip || lower->Parent != upper ||
                    tip->Parent != lower))
                ui.ColoredLabel("IK needs an upper -> lower -> tip hierarchy.",
                    {1.f, .56f, .36f, 1.f});
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (changed)
    {
        track->controlsDirty = true;
        document.dirty = true;
        RefreshAnimationDocumentTitle(document);
        if (m_renderer) m_renderer->MarkDirty();
    }
    if (ui.Button("Save rig control mapping"))
    {
        if (document.path.empty())
            document.error = "Open a prefab to save its rig controls.";
        else SaveAnimationDocument(document);
    }
    ui.DisabledLabel("Select or drag handles in the Animation viewport. Save Selected Rigs stores this mapping.");
}

void EditorState::DrawAnimationPoseLibrary(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.poseSkeleton ||
        !AnimationPropertiesHeader(document, "Pose library and symmetry")) return;
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [&](const auto& item) { return item.index == document.rigIndex; });
    if (track == document.tracks.end()) return;
    const auto& palette = document.poseSkeleton->jointNodes;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    const auto& restJoints = track->sourceSkeleton->ResolveJoints();
    const auto paletteIndex = [&](unsigned node) -> int
    {
        const auto found = std::find(palette.begin(), palette.end(), node);
        return found == palette.end() ? -1 : static_cast<int>(found - palette.begin());
    };
    const auto markDirty = [&]()
    {
        track->controlsDirty = true;
        document.dirty = true;
        RefreshAnimationDocumentTitle(document);
    };
    const auto capturePose = [&]()
    {
        Engine::Model::RigAsset::Pose pose;
        pose.name = document.poseName[0] ? document.poseName :
            "Pose " + std::to_string(track->poses.size() + 1);
        std::unordered_set<unsigned> selected;
        if (document.poseSelectedOnly)
        {
            for (const auto& row : document.selectedRows)
                if (row.rigIndex == document.rigIndex)
                    selected.insert(row.nodeIndex);
            for (const auto& key : document.selectedKeys)
                if (key.rigIndex == document.rigIndex)
                    selected.insert(key.nodeIndex);
            if (selected.empty() && document.selectedBone >= 0 &&
                document.selectedBone < static_cast<int>(palette.size()))
                selected.insert(palette[document.selectedBone]);
        }
        for (size_t i = 0; i < palette.size() && i < joints.size(); ++i)
        {
            if (!joints[i] || (document.poseSelectedOnly &&
                selected.find(palette[i]) == selected.end())) continue;
            const auto& transform = joints[i]->transform;
            pose.joints.push_back({palette[i], transform.position,
                glm::normalize(glm::quat(transform.rotation)), transform.scale});
        }
        return pose;
    };
    ImGui::InputText("Pose name", document.poseName, sizeof(document.poseName));
    ImGui::Checkbox("Selected controls only", &document.poseSelectedOnly);
    if (ui.Button("Capture pose"))
    {
        auto pose = capturePose();
        if (pose.joints.empty()) document.error =
            "Select a bone or timeline controls to capture.";
        else
        {
            track->poses.push_back(std::move(pose));
            document.selectedPose = static_cast<int>(track->poses.size()) - 1;
            document.error.clear();
            markDirty();
        }
    }
    if (ImGui::BeginCombo("Saved pose", document.selectedPose >= 0 &&
            document.selectedPose < static_cast<int>(track->poses.size())
            ? track->poses[document.selectedPose].name.c_str() : "Select pose"))
    {
        for (size_t i = 0; i < track->poses.size(); ++i)
            if (ImGui::Selectable((track->poses[i].name + " (" +
                    std::to_string(track->poses[i].joints.size()) + " bones)").c_str(),
                    document.selectedPose == static_cast<int>(i)))
                document.selectedPose = static_cast<int>(i);
        ImGui::EndCombo();
    }
    ImGui::SliderFloat("Pose blend", &document.poseBlend, 0.f, 1.f);
    ImGui::Checkbox("Mirror left/right", &document.poseMirror);
    ImGui::Checkbox("Key result at playhead", &document.poseKeyResult);
    const bool validPose = document.selectedPose >= 0 &&
        document.selectedPose < static_cast<int>(track->poses.size());
    const auto applyPose = [&](const Engine::Model::RigAsset::Pose& pose)
    {
        auto* model = document.poseSkeleton->ResolveModel();
        const bool validClip = document.sourceManager && document.clipIndex >= 0 &&
            document.clipIndex < static_cast<int>(document.sourceManager->clips.size());
        if (document.poseKeyResult && (!validClip || !model))
            document.error = "Choose an animation clip before keying a pose.";
        else if (document.poseKeyResult &&
            !AnimationClipMapping::Inspect(document.sourceManager->clips[document.clipIndex],
                AnimationClipMapping::RigNodes(*document.poseSkeleton),
                AnimationClipMapping::ModelBones(*model)).issues.empty())
            document.error = "Repair unresolved bone mappings before keying this clip.";
        else
        {
            const auto mirrorNode = [&](unsigned node)
            {
                if (!document.poseMirror) return node;
                for (const auto& pair : track->symmetryPairs)
                {
                    if (pair.left == node) return pair.right;
                    if (pair.right == node) return pair.left;
                }
                return node;
            };
            const auto mirrorVector = [](glm::vec3 value)
            { value.x = -value.x; return value; };
            const auto mirrorRotation = [](glm::quat value)
            { return glm::normalize(glm::quat(value.w, value.x, -value.y, -value.z)); };
            std::vector<unsigned> affected;
            for (const auto& stored : pose.joints)
            {
                const int source = paletteIndex(stored.nodeIndex);
                const unsigned destinationNode = mirrorNode(stored.nodeIndex);
                const int destination = paletteIndex(destinationNode);
                if (source < 0 || destination < 0 ||
                    source >= static_cast<int>(restJoints.size()) ||
                    destination >= static_cast<int>(restJoints.size()) ||
                    !restJoints[source] || !restJoints[destination] ||
                    !joints[destination]) continue;
                glm::vec3 position = stored.position;
                glm::quat rotation = stored.rotation;
                glm::vec3 scale = stored.scale;
                if (document.poseMirror)
                {
                    const auto& sourceRest = restJoints[source]->transform;
                    const auto& targetRest = restJoints[destination]->transform;
                    position = glm::vec3(targetRest.position) + mirrorVector(
                        position - glm::vec3(sourceRest.position));
                    rotation = mirrorRotation(rotation *
                        glm::inverse(glm::quat(sourceRest.rotation))) *
                        glm::quat(targetRest.rotation);
                    for (int axis = 0; axis < 3; ++axis)
                        scale[axis] = targetRest.scale[axis] *
                            (std::abs(sourceRest.scale[axis]) > 1e-6f
                                ? scale[axis] / sourceRest.scale[axis] : 1.f);
                }
                auto& transform = joints[destination]->transform;
                transform.position = glm::mix(glm::vec3(transform.position), position,
                    document.poseBlend);
                transform.rotation = glm::eulerAngles(glm::normalize(glm::slerp(
                    glm::quat(transform.rotation), rotation, document.poseBlend)));
                transform.scale = glm::mix(transform.scale, scale,
                    document.poseBlend);
                affected.push_back(destinationNode);
            }
            if (document.poseKeyResult && !affected.empty())
            {
                auto before = document.sourceManager->clips;
                auto& clip = document.sourceManager->clips[document.clipIndex];
                bool keyed = false;
                for (unsigned node : affected)
                {
                    const auto& transform = joints[paletteIndex(node)]->transform;
                    keyed = AnimationClipEditing::UpsertBonePose(clip, node,
                        AnimationLocalTime(document, document.rigIndex,
                            document.clipIndex), transform.position,
                        glm::quat(transform.rotation), transform.scale) || keyed;
                    const std::string path = AnimationClipMapping::NodePath(*model, node);
                    for (auto& channel : clip.channels)
                        if (channel.nodeIndex == node && channel.targetPath.empty())
                            channel.targetPath = path;
                }
                if (keyed)
                {
                    clip.duration = std::max(clip.duration,
                        AnimationLocalTime(document, document.rigIndex,
                            document.clipIndex));
                    RecordAnimationEdit(document, std::move(before));
                }
            }
            document.error.clear();
            if (m_renderer) m_renderer->MarkDirty();
        }
    };
    if (validPose && ui.Button("Apply pose"))
        applyPose(track->poses[document.selectedPose]);
    if (validPose && ui.Button("Delete pose"))
    {
        track->poses.erase(track->poses.begin() + document.selectedPose);
        document.selectedPose = -1;
        markDirty();
    }
    if (ImGui::TreeNode("Standalone .pose files"))
    {
        auto* model = document.poseSkeleton->ResolveModel();
        const auto makeAsset = [&](const Engine::Model::RigAsset::Pose& pose)
        {
            Engine::Model::PoseAsset asset;
            asset.name = pose.name;
            if (!model) return asset;
            for (const auto& joint : pose.joints)
            {
                const std::string path = AnimationClipMapping::NodePath(
                    *model, joint.nodeIndex);
                if (path.empty()) { asset.joints.clear(); break; }
                asset.joints.push_back({path, joint.position,
                    joint.rotation, joint.scale});
            }
            return asset;
        };
        const auto saveNewAsset = [&](const Engine::Model::RigAsset::Pose& pose)
        {
            const auto asset = makeAsset(pose);
            if (asset.joints.empty())
            {
                document.error = "No compatible model bones to save as a pose.";
                return;
            }
            std::string filename = pose.name;
            for (char& ch : filename)
                if (!std::isalnum(static_cast<unsigned char>(ch)) &&
                    ch != '_' && ch != '-') ch = '_';
            if (filename.empty()) filename = "Pose";
            const std::filesystem::path directory = "Assets/Poses";
            std::filesystem::path path = directory / (filename + ".pose");
            for (int suffix = 2; std::filesystem::exists(path); ++suffix)
                path = directory / (filename + "_" +
                    std::to_string(suffix) + ".pose");
            if (!asset.Save(path.generic_string()))
                document.error = "Could not create the .pose file.";
            else
            {
                document.poseAssetPath = path.generic_string();
                document.error.clear();
            }
        };
        if (ui.Button("Capture directly to .pose"))
        {
            const auto pose = capturePose();
            if (pose.joints.empty())
                document.error = "Select a bone or timeline controls to capture.";
            else saveNewAsset(pose);
        }
        if (document.selectedPose >= 0 &&
            document.selectedPose < static_cast<int>(track->poses.size()) &&
            ui.Button("Copy rig pose to .pose"))
            saveNewAsset(track->poses[document.selectedPose]);
        Engine::Core::Component::DrawAssetPathPicker(ui, &document,
            "poseAssetPath", "Standalone pose", "pose",
            document.poseAssetPath);
        const auto asset = Engine::Model::PoseAsset::Load(document.poseAssetPath);
        if (asset)
        {
            ui.ValueLabel("Pose", asset->name.c_str());
            const auto mapToRig = [&]() ->
                std::optional<Engine::Model::RigAsset::Pose>
            {
                if (!model) return std::nullopt;
                const auto bones = AnimationClipMapping::RigBones(
                    *document.poseSkeleton);
                Engine::Model::RigAsset::Pose pose;
                pose.name = asset->name;
                for (const auto& joint : asset->joints)
                {
                    const auto found = std::find_if(bones.begin(), bones.end(),
                        [&](const auto& bone) { return bone.path == joint.path; });
                    if (found == bones.end() ||
                        std::count_if(bones.begin(), bones.end(),
                            [&](const auto& bone)
                            { return bone.path == joint.path; }) != 1)
                        return std::nullopt;
                    pose.joints.push_back({found->nodeIndex, joint.position,
                        joint.rotation, joint.scale});
                }
                return pose;
            };
            if (ui.Button("Apply .pose"))
            {
                if (const auto mapped = mapToRig()) applyPose(*mapped);
                else document.error =
                    "Pose bone paths do not match this rig.";
            }
            if (ui.Button("Copy .pose into rig library"))
            {
                if (auto mapped = mapToRig())
                {
                    track->poses.push_back(std::move(*mapped));
                    document.selectedPose =
                        static_cast<int>(track->poses.size()) - 1;
                    document.error.clear();
                    markDirty();
                }
                else document.error =
                    "Pose bone paths do not match this rig.";
            }
            if (ui.Button("Duplicate .pose file"))
            {
                const std::filesystem::path source(document.poseAssetPath);
                const auto directory = source.parent_path();
                auto copy = directory / (source.stem().string() + "_copy.pose");
                for (int suffix = 2; std::filesystem::exists(copy); ++suffix)
                    copy = directory / (source.stem().string() + "_copy_" +
                        std::to_string(suffix) + ".pose");
                if (!asset->Save(copy.generic_string()))
                    document.error = "Could not copy the .pose file.";
                else
                {
                    document.poseAssetPath = copy.generic_string();
                    document.error.clear();
                }
            }
            if (ui.Button("Delete .pose file"))
            {
                std::error_code error;
                const auto resolved = Engine::Components::Mesh::ResolveFilePath(
                    document.poseAssetPath);
                if (std::filesystem::path(resolved).extension() != ".pose" ||
                    !std::filesystem::remove(resolved, error) || error)
                    document.error = "Could not delete the .pose file.";
                else
                {
                    document.poseAssetPath.clear();
                    document.error.clear();
                }
            }
        }
        else if (!document.poseAssetPath.empty())
            ui.ColoredLabel("Invalid .pose file", {1.f, .35f, .35f, 1.f});
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Left/right bone map"))
    {
        const auto boneChoice = [&](const char* label, unsigned& node)
        {
            const int current = paletteIndex(node);
            const char* currentName = current >= 0 && current <
                static_cast<int>(joints.size()) && joints[current]
                ? joints[current]->name.c_str() : "Choose bone";
            if (!ImGui::BeginCombo(label, currentName)) return false;
            bool changed = false;
            for (size_t i = 0; i < palette.size() && i < joints.size(); ++i)
                if (joints[i] && ImGui::Selectable(joints[i]->name.c_str(),
                        node == palette[i]))
                { node = palette[i]; changed = true; }
            ImGui::EndCombo();
            return changed;
        };
        for (size_t i = 0; i < track->symmetryPairs.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            auto& pair = track->symmetryPairs[i];
            const bool leftChanged = boneChoice("Left", pair.left);
            const bool rightChanged = boneChoice("Right", pair.right);
            if (leftChanged || rightChanged)
                markDirty();
            if (ui.Button("Remove pair"))
            {
                track->symmetryPairs.erase(track->symmetryPairs.begin() + i);
                markDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ui.Button("Add pair") && palette.size() > 1)
        {
            track->symmetryPairs.push_back({palette[0], palette[1]});
            markDirty();
        }
        ImGui::TreePop();
    }
    if (ui.Button("Save pose library and bone map"))
    {
        if (document.path.empty())
            document.error = "Open a prefab to save its pose library.";
        else SaveAnimationDocument(document);
    }
}

void EditorState::DrawAnimationChainIK(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.poseSkeleton ||
        !AnimationPropertiesHeader(document, "Chain and full-body IK")) return;
    const auto& palette = document.poseSkeleton->jointNodes;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    const auto jointFor = [&](unsigned node) -> Engine::Core::Object*
    {
        const auto found = std::find(palette.begin(), palette.end(), node);
        const size_t index = static_cast<size_t>(found - palette.begin());
        return found != palette.end() && index < joints.size()
            ? joints[index] : nullptr;
    };
    const auto nodeFor = [&](Engine::Core::Object* joint) -> int
    {
        const auto found = std::find(joints.begin(), joints.end(), joint);
        const size_t index = static_cast<size_t>(found - joints.begin());
        return found != joints.end() && index < palette.size()
            ? static_cast<int>(palette[index]) : -1;
    };
    if (ui.Button("Add selected bone as effector"))
    {
        Engine::Core::Object* tip = document.selectedBone >= 0 &&
            document.selectedBone < static_cast<int>(joints.size())
            ? joints[document.selectedBone] : nullptr;
        Engine::Core::Object* root = tip ? tip->Parent : nullptr;
        while (root && root->Parent && nodeFor(root->Parent) >= 0)
            root = root->Parent;
        if (!tip || !root || nodeFor(root) < 0)
            document.error = "Select a bone with an ancestor in this rig.";
        else
        {
            AnimationDocument::ChainEffector control;
            control.rigIndex = document.rigIndex;
            control.rootNode = static_cast<unsigned>(nodeFor(root));
            control.tipNode = static_cast<unsigned>(nodeFor(tip));
            control.target = tip->transform.GetWorldPosition();
            glm::mat3 basis(tip->transform.GetWorldMatrix());
            for (int axis = 0; axis < 3; ++axis)
                if (glm::length(basis[axis]) > 1e-6f)
                    basis[axis] = glm::normalize(basis[axis]);
            control.targetRotation = glm::eulerAngles(
                glm::normalize(glm::quat_cast(basis)));
            control.pole = tip->Parent->transform.GetWorldPosition() +
                glm::vec3(0.f, 1.f, 0.f);
            document.chainEffectors.push_back(control);
            document.error.clear();
        }
    }
    for (size_t i = 0; i < document.chainEffectors.size(); ++i)
    {
        auto& control = document.chainEffectors[i];
        if (control.rigIndex != document.rigIndex) continue;
        ImGui::PushID(static_cast<int>(i));
        auto* tip = jointFor(control.tipNode);
        auto* root = jointFor(control.rootNode);
        if (ImGui::TreeNode((tip ? tip->name : "Missing tip").c_str()))
        {
            if (ImGui::BeginCombo("Tip", tip ? tip->name.c_str() : "Choose"))
            {
                for (size_t bone = 0; bone < palette.size() &&
                    bone < joints.size(); ++bone)
                    if (joints[bone] && ImGui::Selectable(joints[bone]->name.c_str(),
                            control.tipNode == palette[bone]))
                    {
                        control.tipNode = palette[bone];
                        tip = joints[bone];
                        if (tip->Parent && nodeFor(tip->Parent) >= 0)
                            control.rootNode = static_cast<unsigned>(
                                nodeFor(tip->Parent));
                        control.target = tip->transform.GetWorldPosition();
                    }
                ImGui::EndCombo();
            }
            if (ImGui::BeginCombo("Root", root ? root->name.c_str() : "Choose"))
            {
                for (auto* ancestor = tip ? tip->Parent : nullptr; ancestor;
                    ancestor = ancestor->Parent)
                {
                    const int node = nodeFor(ancestor);
                    if (node >= 0 && ImGui::Selectable(ancestor->name.c_str(),
                            control.rootNode == static_cast<unsigned>(node)))
                        control.rootNode = static_cast<unsigned>(node);
                }
                ImGui::EndCombo();
            }
            ui.DragFloat3("Target (world)", &control.target.x, .01f);
            float angles[]{glm::degrees(control.targetRotation.x),
                glm::degrees(control.targetRotation.y),
                glm::degrees(control.targetRotation.z)};
            if (ui.DragFloat3("Target rotation (degrees)", angles, .5f))
                control.targetRotation = glm::radians(glm::vec3(
                    angles[0], angles[1], angles[2]));
            ImGui::Checkbox("Use pole", &control.usePole);
            if (control.usePole)
                ui.DragFloat3("Pole (world)", &control.pole.x, .01f);
            ui.DragFloat("Position weight", &control.positionWeight,
                .01f, 0.f, 1.f);
            ui.DragFloat("Rotation weight", &control.rotationWeight,
                .01f, 0.f, 1.f);
            if (ui.Button("Remove effector"))
            {
                document.chainEffectors.erase(
                    document.chainEffectors.begin() + i);
                ImGui::TreePop();
                ImGui::PopID();
                break;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::InputInt("Solver iterations", &document.chainIterations);
    document.chainIterations = std::clamp(document.chainIterations, 1, 128);
    ui.DragFloat("Body root translation", &document.chainRootTranslation,
        .01f, 0.f, 1.f);
    const auto solve = [&](bool keyResult)
    {
        auto* model = document.poseSkeleton->ResolveModel();
        if (keyResult && (!document.sourceManager || !model ||
            document.clipIndex < 0 || document.clipIndex >=
                static_cast<int>(document.sourceManager->clips.size())))
        {
            document.error = "Choose an animation clip before keying IK.";
            return;
        }
        if (keyResult && !AnimationClipMapping::Inspect(
                document.sourceManager->clips[document.clipIndex],
                AnimationClipMapping::RigNodes(*document.poseSkeleton),
                AnimationClipMapping::ModelBones(*model)).issues.empty())
        {
            document.error = "Repair unresolved bone mappings before keying IK.";
            return;
        }
        SampleAnimationDocument(document);
        std::unordered_map<Engine::Core::Object*,
            std::vector<Engine::Components::ChainIK::Effector>> groups;
        std::unordered_set<unsigned> affected;
        for (const auto& control : document.chainEffectors)
        {
            if (control.rigIndex != document.rigIndex) continue;
            auto* root = jointFor(control.rootNode);
            auto* tip = jointFor(control.tipNode);
            if (!root || !tip || root == tip) continue;
            Engine::Components::ChainIK::Effector effector;
            for (auto* joint = tip; joint; joint = joint->Parent)
            {
                effector.chain.push_back(joint);
                if (joint == root) break;
            }
            if (effector.chain.back() != root) continue;
            std::reverse(effector.chain.begin(), effector.chain.end());
            effector.target = control.target;
            effector.rotation = glm::quat(control.targetRotation);
            effector.pole = control.pole;
            effector.usePole = control.usePole;
            effector.positionWeight = control.positionWeight;
            effector.rotationWeight = control.rotationWeight;
            for (auto* joint : effector.chain)
                if (const int node = nodeFor(joint); node >= 0)
                    affected.insert(static_cast<unsigned>(node));
            groups[root].push_back(std::move(effector));
        }
        if (groups.empty())
        {
            document.error = "Add a valid chain effector first.";
            return;
        }
        for (const auto& [root, effectors] : groups)
            Engine::Components::ChainIK::SolveMultiple(effectors,
                document.chainIterations, .001f,
                document.chainRootTranslation);
        for (unsigned node : affected)
            if (auto* joint = jointFor(node))
                joint->transform.NotifyEditorTransformChanged(
                    Engine::Components::Transform::EditorRotation |
                    Engine::Components::Transform::EditorPosition);
        if (keyResult)
        {
            auto before = document.sourceManager->clips;
            auto& clip = document.sourceManager->clips[document.clipIndex];
            bool keyed = false;
            for (unsigned node : affected)
            {
                auto* joint = jointFor(node);
                if (!joint) continue;
                const auto& transform = joint->transform;
                keyed = AnimationClipEditing::UpsertBonePose(clip, node,
                    AnimationLocalTime(document, document.rigIndex,
                        document.clipIndex), transform.position,
                    glm::quat(transform.rotation), transform.scale) || keyed;
                const std::string path = AnimationClipMapping::NodePath(*model, node);
                for (auto& channel : clip.channels)
                    if (channel.nodeIndex == node && channel.targetPath.empty())
                        channel.targetPath = path;
            }
            if (keyed)
            {
                clip.duration = std::max(clip.duration,
                    AnimationLocalTime(document, document.rigIndex,
                        document.clipIndex));
                RecordAnimationEdit(document, std::move(before));
            }
        }
        document.error.clear();
        if (m_renderer) m_renderer->MarkDirty();
    };
    if (ui.Button("Solve all effectors")) solve(false);
    if (ui.Button("Solve and key bone channels")) solve(true);
    ui.DisabledLabel("Add several effectors with the same root to solve shared body joints together.");
}

bool EditorState::DrawAnimationRigViewport(IEditorUi& ui,
    const EditorUiViewportInput& input, AnimationDocument& document)
{
    if (!document.poseScene || !document.poseSkeleton ||
        input.available.x <= 1.f || input.available.y <= 1.f) return false;
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [&](const auto& item) { return item.index == document.rigIndex; });
    if (track == document.tracks.end()) return false;
    auto* model = document.poseSkeleton->ResolveModel();
    auto* camera = document.poseScene->editorCamera.GetComponent<
        Engine::Components::Camera>();
    if (!model || !camera) return false;
    const auto& nodes = model->ResolveNodes();
    const auto& joints = document.poseSkeleton->ResolveJoints();
    const auto& palette = document.poseSkeleton->jointNodes;
    const auto jointAt = [&](int node) -> Engine::Core::Object*
    {
        if (node < 0 || static_cast<size_t>(node) >= nodes.size()) return nullptr;
        auto* object = nodes[node];
        return std::find(joints.begin(), joints.end(), object) != joints.end()
            ? object : nullptr;
    };
    const glm::mat4 viewProjection = camera->GetProjectionMatrix(
        input.available.x / input.available.y) * camera->GetViewMatrix();
    const auto project = [&](const glm::vec3& world, EditorUiVec2& screen)
    {
        const glm::vec4 clip = viewProjection * glm::vec4(world, 1.f);
        if (clip.w <= .0001f) return false;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.z < 0.f || ndc.z > 1.f) return false;
        screen = {(ndc.x * .5f + .5f) * input.available.x,
            (.5f - ndc.y * .5f) * input.available.y};
        return true;
    };
    struct Handle { int id; glm::vec3 world; const char* label; EditorUiColor color; };
    std::vector<Handle> handles;
    const auto addBone = [&](int id, int node, const char* label, EditorUiColor color)
    {
        if (auto* object = jointAt(node))
            handles.push_back({id, object->transform.GetWorldPosition(), label, color});
    };
    addBone(0, track->controls.root, "Root", {.98f, .81f, .29f, 1.f});
    addBone(1, track->controls.pelvis, "Pelvis", {.93f, .51f, .89f, 1.f});
    const glm::mat4 modelWorld = model->Owner
        ? model->Owner->transform.GetWorldMatrix() : glm::mat4(1.f);
    static const char* targetNames[] = {"L hand", "R hand", "L foot", "R foot"};
    static const char* poleNames[] = {"L elbow", "R elbow", "L knee", "R knee"};
    for (size_t i = 0; i < track->controls.limbs.size(); ++i)
    {
        const auto& limb = track->controls.limbs[i];
        auto* upper = jointAt(limb.upper);
        auto* lower = jointAt(limb.lower);
        auto* tip = jointAt(limb.tip);
        if (!upper || !lower || !tip || lower->Parent != upper ||
            tip->Parent != lower) continue;
        glm::vec3 target = tip->transform.GetWorldPosition();
        const glm::vec3 root = upper->transform.GetWorldPosition();
        const glm::vec3 mid = lower->transform.GetWorldPosition();
        glm::vec3 pole = mid * 2.f - (root + target) * .5f;
        if (document.sourceManager && document.clipIndex >= 0 &&
            document.clipIndex < static_cast<int>(document.sourceManager->clips.size()))
        {
            const auto& clip = document.sourceManager->clips[document.clipIndex];
            const auto key = std::find_if(clip.limbs.begin(), clip.limbs.end(),
                [&](const auto& item) { return item.tip == static_cast<unsigned>(limb.tip); });
            if (key != clip.limbs.end() && !key->keys.empty())
            {
                const auto sampled = Engine::Model::AnimationClip::SampleLimb(
                    *key, AnimationLocalTime(document, document.rigIndex,
                        document.clipIndex));
                const glm::mat4 space = Engine::Model::LimbSpaceMatrix(
                    sampled, modelWorld, *upper, model->ResolveNodes());
                target = Engine::Model::LimbWorldPoint(space, sampled.target);
                pole = Engine::Model::LimbWorldPoint(space, sampled.pole);
            }
        }
        if (document.ikControlTip >= 0 &&
            document.ikControlTip < static_cast<int>(joints.size()) &&
            joints[document.ikControlTip] == tip &&
            document.ikControlTime == document.playheadSeconds)
        {
            target = document.ikTarget;
            pole = document.ikPole;
        }
        handles.push_back({static_cast<int>(2 + 2 * i), target,
            targetNames[i], {.22f, .91f, .78f, 1.f}});
        handles.push_back({static_cast<int>(3 + 2 * i), pole,
            poleNames[i], {.99f, .53f, .27f, 1.f}});
    }
    int hovered = -1;
    float closest = 14.f * 14.f;
    for (const auto& handle : handles)
    {
        EditorUiVec2 screen;
        if (!project(handle.world, screen)) continue;
        const float dx = input.mousePosInViewport.x - screen.x;
        const float dy = input.mousePosInViewport.y - screen.y;
        const float distance = dx * dx + dy * dy;
        if (distance < closest)
        { closest = distance; hovered = handle.id; }
        const bool selected = document.rigHandleKind == handle.id;
        ui.DrawViewportCircle(screen, selected ? 10.f : 8.f,
            selected ? EditorUiColor{1.f, 1.f, .65f, 1.f} : handle.color);
        ui.DrawViewportCircle(screen, selected ? 10.f : 8.f,
            {.04f, .05f, .07f, .9f}, false, 2.f);
        ui.DrawViewportText({screen.x + 12.f, screen.y - 8.f},
            handle.label, handle.color);
    }
    const bool canDragHandles = document.view &&
        document.view->GetTransformTool() == EditorTransformTool::Translate;
    if (!canDragHandles && document.rigHandleDrag < 0) return false;
    if (input.leftClicked && hovered >= 0)
    {
        document.playing = false;
        document.rigHandleKind = hovered;
        document.rigHandleDrag = hovered;
        const int node = hovered == 0 ? track->controls.root :
            hovered == 1 ? track->controls.pelvis :
            track->controls.limbs[(hovered - 2) / 2].tip;
        for (size_t i = 0; i < palette.size(); ++i)
            if (static_cast<int>(palette[i]) == node && i < joints.size())
            {
                document.selectedBone = static_cast<int>(i);
                document.poseScene->SetSelectedObject(joints[i]);
                if (m_primaryHierarchy)
                    m_primaryHierarchy->SetSelectedObject(joints[i]);
                if (m_primaryProperties)
                    m_primaryProperties->SetSelectedObject(joints[i]);
                break;
            }
        if (hovered >= 2)
        {
            const int limbIndex = (hovered - 2) / 2;
            document.ikControlTip = document.selectedBone;
            document.ikControlTime = document.playheadSeconds;
            document.ikBlend = 1.f;
            document.ikSpace = Engine::Model::AnimationClip::LimbKey::Space::Body;
            document.ikSpaceNode = -1;
            document.ikPropReference.Clear();
            if (document.sourceManager && document.clipIndex >= 0 &&
                document.clipIndex < static_cast<int>(document.sourceManager->clips.size()))
            {
                const auto& clip = document.sourceManager->clips[document.clipIndex];
                const auto existing = std::find_if(clip.limbs.begin(), clip.limbs.end(),
                    [&](const auto& item)
                    { return item.tip == static_cast<unsigned>(
                        track->controls.limbs[limbIndex].tip); });
                if (existing != clip.limbs.end() && !existing->keys.empty())
                {
                    const auto key = Engine::Model::AnimationClip::SampleLimb(
                        *existing, AnimationLocalTime(document,
                            document.rigIndex, document.clipIndex));
                    document.ikSpace = key.space;
                    document.ikSpaceNode = key.spaceNode;
                    document.ikPropReference = key.propReference;
                }
            }
            for (const auto& handle : handles)
            {
                if (handle.id == 2 + 2 * limbIndex) document.ikTarget = handle.world;
                if (handle.id == 3 + 2 * limbIndex) document.ikPole = handle.world;
            }
            if (auto* tip = jointAt(track->controls.limbs[limbIndex].tip))
            {
                glm::mat3 basis(tip->transform.GetWorldMatrix());
                for (int axis = 0; axis < 3; ++axis)
                    if (glm::length(basis[axis]) > 1e-6f)
                        basis[axis] = glm::normalize(basis[axis]);
                document.ikTargetRotation = glm::eulerAngles(
                    glm::normalize(glm::quat_cast(basis)));
            }
        }
        return true;
    }
    if (document.rigHandleDrag < 0) return hovered >= 0 && input.leftClicked;
    const int dragging = document.rigHandleDrag;
    if (input.leftDown && (input.mouseDelta.x != 0.f || input.mouseDelta.y != 0.f))
    {
        const auto found = std::find_if(handles.begin(), handles.end(),
            [&](const Handle& item) { return item.id == dragging; });
        if (found != handles.end())
        {
            const glm::mat4 cameraWorld = glm::inverse(camera->GetViewMatrix());
            const glm::vec3 right = glm::normalize(glm::vec3(cameraWorld[0]));
            const glm::vec3 up = glm::normalize(glm::vec3(cameraWorld[1]));
            // Perspective scale depends on camera-space depth. Radial distance
            // makes handles near the edge of the viewport outrun the pointer.
            const float distance = std::abs((camera->GetViewMatrix() *
                glm::vec4(found->world, 1.f)).z);
            const float worldPerPixel = camera->orthographic
                ? 2.f * camera->orthographicSize / input.available.y
                : 2.f * std::max(.01f, distance) *
                    std::tan(glm::radians(camera->fov) * .5f) / input.available.y;
            const glm::vec3 delta = worldPerPixel *
                (right * input.mouseDelta.x - up * input.mouseDelta.y);
            if (dragging < 2)
            {
                const int node = dragging == 0 ? track->controls.root : track->controls.pelvis;
                if (auto* joint = jointAt(node))
                {
                    const glm::mat4 parentWorld = joint->Parent
                        ? joint->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
                    joint->transform.position = glm::vec3(glm::inverse(parentWorld) *
                        glm::vec4(joint->transform.GetWorldPosition() + delta, 1.f));
                    joint->transform.NotifyEditorTransformChanged(
                        Engine::Components::Transform::EditorPosition);
                }
            }
            else
            {
                if ((dragging - 2) % 2 == 0) document.ikTarget += delta;
                else document.ikPole += delta;
                const auto& limb = track->controls.limbs[(dragging - 2) / 2];
                auto* upper = jointAt(limb.upper);
                auto* lower = jointAt(limb.lower);
                auto* tip = jointAt(limb.tip);
                if (upper && lower && tip)
                {
                    Engine::Components::TwoBoneIK::Blend(*upper, *lower, *tip,
                        document.ikTarget, document.ikPole,
                        glm::quat(document.ikTargetRotation), document.ikBlend);
                    for (auto* joint : {upper, lower, tip})
                        joint->transform.NotifyEditorTransformChanged(
                            Engine::Components::Transform::EditorRotation);
                }
            }
            if (m_renderer) m_renderer->MarkDirty();
        }
    }
    if (!input.leftDown)
    {
        document.rigHandleDrag = -1;
        if (document.autoKey)
        {
            if (dragging < 2) AddAnimationKey(document, true);
            else
            {
                const auto& limb = track->controls.limbs[(dragging - 2) / 2];
                auto* upper = jointAt(limb.upper);
                auto* lower = jointAt(limb.lower);
                auto* tip = jointAt(limb.tip);
                if (upper && lower && tip)
                    AddAnimationLimbKey(document, *upper, *lower, *tip, false);
            }
        }
    }
    return true;
}

void EditorState::DrawAnimationRigConstraints(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.poseSkeleton) return;
    auto* model = document.poseSkeleton->ResolveModel();
    auto* constraints = model && model->Owner
        ? model->Owner->GetComponent<Engine::Components::RigConstraints>() : nullptr;
    if (!constraints || !AnimationPropertiesHeader(document, "Model rig constraints",
            ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [&](const auto& item) { return item.index == document.rigIndex; });
    if (track == document.tracks.end() || !track->sourceSkeleton) return;
    auto* sourceModel = track->sourceSkeleton->ResolveModel();
    if (!sourceModel || !sourceModel->Owner) return;
    ImGui::PushID("AnimationRigConstraints");
    ui.ValueLabel("Constraint owner", model->Owner->name.c_str());
    ui.DisabledLabel("Leg and toe IK hold the feet at their target bones.");
    ui.DisabledLabel("Turn them off for FK walking. Save to keep these model settings.");
    if (constraints->DrawProperties(ui))
    {
        auto* source = sourceModel->Owner->GetComponent<Engine::Components::RigConstraints>();
        if (!source) source = sourceModel->Owner->AddComponent<Engine::Components::RigConstraints>();
        source->Deserialize(constraints->Serialize());
        track->rigConstraintsDirty = true;
        document.dirty = true;
        RefreshAnimationDocumentTitle(document);
        SampleAnimationDocument(document);
    }
    ImGui::PopID();
}


}
