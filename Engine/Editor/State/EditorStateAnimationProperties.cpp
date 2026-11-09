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

void EditorState::DrawAnimationProperties(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.sourceManager)
    {
        ui.DisabledLabel("Drop a prefab, scene object, or .anim onto the Animation Editor.");
        if (!document.error.empty()) ui.ColoredLabel(
            document.error.c_str(), { 1.f, .5f, .35f, 1.f });
        return;
    }
    ImGui::PushID(&document);
    document.drawingProperties = true;
    struct FinishProperties
    {
        AnimationDocument& document;
        ~FinishProperties()
        {
            document.drawingProperties = false;
            document.propertiesHeaderOverride = -1;
            ImGui::PopID();
        }
    } finishProperties{document};
    const char* sections[]{"Rig details", "Key editing", "Model rig constraints",
        "Humanoid animation mapping", "Viewport rig controls", "Pose controller keys",
        "Pose library and symmetry", "Chain and full-body IK", "Model animations",
        "Repair bone mappings", "Selected bone", "Two-bone IK"};
    const auto setExpanded = [&](bool open)
    {
        document.propertiesHeaderOverride = open ? 1 : 0;
        document.propertiesSearch[0] = '\0';
        document.propertiesJumpTarget.clear();
        // Set hidden descendants too, so reopening Selected bone after
        // Collapse all does not reveal an IK section left expanded earlier.
        for (const char* section : sections)
            ImGui::GetStateStorage()->SetInt(ImGui::GetID(section), open ? 1 : 0);
        if (m_primaryProperties) m_primaryProperties->SetComponentHeadersOpen(open);
    };
    if (ui.Button(document.propertiesOutlineOpen
            ? "List <##animationOutline" : "List >##animationOutline", 48.f))
        document.propertiesOutlineOpen = !document.propertiesOutlineOpen;
    ui.Tooltip("Show or hide the animation section outline");
    ui.SameLine();
    if (ui.Button("-##collapseAnimationProperties", 24.f))
        setExpanded(false);
    ui.Tooltip("Collapse all animation sections and selected bone components");
    ui.SameLine();
    if (ui.Button("+##expandAnimationProperties", 24.f))
        setExpanded(true);
    ui.Tooltip("Expand all animation sections and selected bone components");
    ui.SameLine();
    ui.SearchInput("##animationPropertiesSearch", document.propertiesSearch,
        sizeof(document.propertiesSearch), "Find animation section...");
    if (document.propertiesOutlineOpen)
    {
        if (ui.BeginChildRegion("animationSectionOutline", 112.f))
        {
            for (const char* section : sections)
                if (ui.Selectable(section))
                {
                    document.propertiesJumpTarget = section;
                    document.propertiesSearch[0] = '\0';
                }
        }
        ui.EndChild();
    }
    ui.Separator();
    if (AnimationPropertiesHeader(document, "Rig details"))
    {
        const std::string rigLabel = document.rigName + " / " +
            std::filesystem::path(document.path).filename().string();
        ui.ValueLabel("Rig", rigLabel.c_str());
        ui.ValueLabel("Rig asset", document.rigPath.empty()
            ? "Created when the clip is saved" : document.rigPath.c_str());
        if (document.boundMeshNames.empty())
            ui.DisabledLabel("No skinned meshes are bound to this skeleton.");
        else
            for (size_t index = 0; index < document.boundMeshNames.size(); ++index)
                ui.ValueLabel(index == 0 ? "Bound meshes" : "",
                    document.boundMeshNames[index].c_str());
    }
    if (AnimationPropertiesHeader(document, "Key editing"))
    {
        DrawAnimationKeyTools(ui, document);
        ImGui::SetNextItemWidth(160.f);
        ImGui::SliderFloat("Timeline zoom", &document.timelinePixelsPerSecond,
            35.f, 3200.f, "%.0f px/s", ImGuiSliderFlags_Logarithmic);
    }
    DrawAnimationRigConstraints(ui, document);
    DrawAnimationRigControls(ui, document);
    if (AnimationPropertiesHeader(document, "Pose controller keys"))
    {
        using Space = Engine::Model::AnimationClip::ControllerKey::Space;
        ui.ValueLabel("New key target space",
            document.controllerSpace == Space::Body ? "Body" : "World");
        if (ui.Button("Body##ControllerSpace"))
            document.controllerSpace = Space::Body;
        ui.SameLine();
        if (ui.Button("World##ControllerSpace"))
            document.controllerSpace = Space::World;
        ui.DisabledLabel("Body follows the model root; World stays in scene space.");
        if (ui.Button("Bake IK and controllers to bone keys (30 fps)"))
            BakeAnimationLimbs(document);
    }
    DrawAnimationPoseLibrary(ui, document);
    DrawAnimationChainIK(ui, document);
    ui.Separator();
    if (AnimationPropertiesHeader(document, "Model animations", ImGuiTreeNodeFlags_DefaultOpen))
        DrawAnimationLayerTools(ui, document);
    DrawAnimationMappingRepair(ui, document);
    const std::string saveLabel = std::string(Icons::Save) +
        (document.sourceObject ? " Apply Animation to Object" :
            !document.standaloneAssetPath.empty() ? " Save Animation" : " Save Selected Rigs");
    if (ui.Button(saveLabel.c_str()))
    {
        const size_t focused = document.rigIndex;
        const std::string previousSnapshot = document.savedSnapshot;
        bool saved = true;
        for (const auto& track : document.tracks)
            if (track.selected)
            {
                FocusAnimationRig(document, track.index);
                if (!SaveAnimationDocument(document))
                { saved = false; break; }
            }
        FocusAnimationRig(document, focused);
        if (!saved)
        {
            document.savedSnapshot = previousSnapshot;
            document.dirty = true;
            RefreshAnimationDocumentTitle(document);
        }
    }
    if (!document.error.empty()) ui.ColoredLabel(document.error.c_str(),
        { 1.f, .5f, .35f, 1.f });

    if (!AnimationPropertiesHeader(document, "Selected bone", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    if (document.selectedBone < 0 ||
        document.selectedBone >= static_cast<int>(joints.size()) ||
        !joints[document.selectedBone]) return;
    auto* joint = joints[document.selectedBone];
    auto* bone = joint->GetComponent<Engine::Components::AnimationBone>();
    if (!bone && document.selectedBone >= static_cast<int>(
            document.poseSkeleton->jointNodes.size())) return;
    const unsigned nodeIndex = bone ? bone->nodeIndex :
        document.poseSkeleton->jointNodes[document.selectedBone];
    ui.Separator();
    const std::string boneInfo = joint->name + " (node " +
        std::to_string(nodeIndex) + ")";
    ui.ValueLabel("Selected bone", boneInfo.c_str());
    auto& transform = joint->transform;
    const glm::dvec3 oldPosition = transform.position;
    const glm::vec3 oldRotation = transform.rotation;
    const glm::vec3 oldScale = transform.scale;
    float angles[]{ glm::degrees(oldRotation.x),
        glm::degrees(oldRotation.y), glm::degrees(oldRotation.z) };
    glm::vec3 editedPosition(transform.position);
    const bool positionChanged = ui.DragFloat3("Pose position",
        &editedPosition.x, .01f);
    if (positionChanged) transform.position = glm::dvec3(editedPosition);
    const bool rotationChanged = ui.DragFloat3("Pose rotation (degrees)",
        angles, .5f);
    if (rotationChanged)
        transform.rotation = { glm::radians(angles[0]),
            glm::radians(angles[1]), glm::radians(angles[2]) };
    const bool scaleChanged = ui.DragFloat3("Pose scale",
        &transform.scale.x, .01f);
    uint8_t channels = 0;
    if (positionChanged && transform.position != oldPosition)
        channels |= Engine::Components::Transform::EditorPosition;
    if (rotationChanged && transform.rotation != oldRotation)
        channels |= Engine::Components::Transform::EditorRotation;
    if (scaleChanged && transform.scale != oldScale)
        channels |= Engine::Components::Transform::EditorScale;
    if (channels)
    {
        document.playing = false;
        transform.NotifyEditorTransformChanged(channels);
        document.poseControlEditPending = true;
        if (m_renderer) m_renderer->MarkDirty();
    }
    if (document.poseControlEditPending && !ui.IsAnyItemActive())
    {
        document.poseControlEditPending = false;
        if (document.autoKey) AddAnimationKey(document);
    }
    // A selected tip and its two parents form an authoring IK chain.
    auto* lower = joint->Parent;
    auto* upper = lower ? lower->Parent : nullptr;
    const bool validChain = upper && lower &&
        std::find(joints.begin(), joints.end(), upper) != joints.end() &&
        std::find(joints.begin(), joints.end(), lower) != joints.end();
    if (validChain && AnimationPropertiesHeader(document, "Two-bone IK"))
    {
        if (document.ikControlTip != document.selectedBone ||
            document.ikControlTime != document.playheadSeconds)
        {
            document.ikControlTip = document.selectedBone;
            document.ikControlTime = document.playheadSeconds;
            document.ikTarget = joint->transform.GetWorldPosition();
            glm::mat3 tipBasis(joint->transform.GetWorldMatrix());
            for (int axis = 0; axis < 3; ++axis)
                if (glm::length(tipBasis[axis]) > 1e-6f)
                    tipBasis[axis] = glm::normalize(tipBasis[axis]);
            document.ikTargetRotation = glm::eulerAngles(
                glm::normalize(glm::quat_cast(tipBasis)));
            const glm::vec3 root = upper->transform.GetWorldPosition();
            const glm::vec3 mid = lower->transform.GetWorldPosition();
            const glm::vec3 end = document.ikTarget;
            document.ikPole = mid + (mid - (root + end) * .5f);
            document.ikBlend = 0.f;
            document.ikSpace = Engine::Model::AnimationClip::LimbKey::Space::Body;
            document.ikSpaceNode = -1;
            document.ikPropReference.Clear();
            if (document.clipIndex >= 0 && document.clipIndex <
                static_cast<int>(document.sourceManager->clips.size()))
            {
                const auto& clip = document.sourceManager->clips[document.clipIndex];
                const auto found = std::find_if(clip.limbs.begin(), clip.limbs.end(),
                    [&](const auto& limb) { return limb.tip == nodeIndex; });
                if (found != clip.limbs.end() && !found->keys.empty())
                {
                    const auto key = Engine::Model::AnimationClip::SampleLimb(
                        *found, AnimationLocalTime(document,
                            document.rigIndex, document.clipIndex));
                    auto* model = document.poseSkeleton->ResolveModel();
                    if (model && model->Owner)
                    {
                        const glm::mat4 world = Engine::Model::LimbSpaceMatrix(
                            key, model->Owner->transform.GetWorldMatrix(),
                            *upper, model->ResolveNodes());
                        document.ikTarget = Engine::Model::LimbWorldPoint(world, key.target);
                        document.ikPole = Engine::Model::LimbWorldPoint(world, key.pole);
                        document.ikTargetRotation = glm::eulerAngles(
                            glm::normalize(Engine::Model::LimbSpaceRotation(world) * key.rotation));
                    }
                    document.ikBlend = key.blend;
                    document.ikSpace = key.space;
                    document.ikSpaceNode = key.spaceNode;
                    document.ikPropReference = key.propReference;
                }
            }
            document.ikControlEditPending = false;
        }
        ui.ValueLabel("Chain", (upper->name + " / " + lower->name +
            " / " + joint->name).c_str());
        using ControlSpace = Engine::Model::AnimationClip::LimbKey::Space;
        const auto switchSpace = [&](ControlSpace space,
            const Engine::Core::ComponentReference* prop = nullptr)
        {
            const auto& reference = prop ? *prop : document.ikPropReference;
            if (document.ikSpace == space &&
                (space != ControlSpace::Prop ||
                    (document.ikPropReference.objectPath == reference.objectPath &&
                     document.ikPropReference.objectName == reference.objectName))) return;
            const auto oldSpace = document.ikSpace;
            const int oldNode = document.ikSpaceNode;
            const auto oldProp = document.ikPropReference;
            document.ikSpace = space;
            document.ikSpaceNode = -1;
            if (prop) document.ikPropReference = *prop;
            if (!AddAnimationLimbKey(document, *upper, *lower, *joint, false))
            {
                document.ikSpace = oldSpace;
                document.ikSpaceNode = oldNode;
                document.ikPropReference = oldProp;
            }
        };
        ui.ValueLabel("Control space", document.ikSpace == ControlSpace::Local
            ? "Local" : document.ikSpace == ControlSpace::World ? "World"
            : document.ikSpace == ControlSpace::Prop ? "Prop" : "Body");
        if (ui.Button("Local##IKSpace")) switchSpace(ControlSpace::Local);
        ImGui::SameLine();
        if (ui.Button("World##IKSpace")) switchSpace(ControlSpace::World);
        ImGui::SameLine();
        if (ui.Button("Body##IKSpace")) switchSpace(ControlSpace::Body);
        ImGui::SameLine();
        if (ui.Button("Prop##IKSpace") && document.ikPropReference.IsAssigned())
            switchSpace(ControlSpace::Prop);
        if (document.poseScene)
        {
            const char* propName = document.ikPropReference.IsAssigned()
                ? document.ikPropReference.objectName.c_str() : "Choose prop";
            if (ImGui::BeginCombo("Prop reference", propName))
            {
                size_t index = 0;
                for (const auto& object : document.poseScene->GetObjects())
                {
                    auto* candidate = object.get();
                    if (!candidate) continue;
                    bool inChain = false;
                    for (auto* parent = candidate; parent; parent = parent->Parent)
                        if (parent == upper) { inChain = true; break; }
                    if (inChain) continue;
                    const auto reference = Engine::Core::CaptureComponentReference(
                        &candidate->transform, "Transform");
                    ImGui::PushID(static_cast<int>(index++));
                    if (ImGui::Selectable(candidate->name.c_str(),
                            document.ikSpace == ControlSpace::Prop &&
                            document.ikPropReference.objectPath == reference.objectPath))
                        switchSpace(ControlSpace::Prop, &reference);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        bool changed = ui.DragFloat3("IK target (world)",
            &document.ikTarget.x, .01f);
        changed = ui.DragFloat3("IK pole (world)",
            &document.ikPole.x, .01f) || changed;
        float targetAngles[]{ glm::degrees(document.ikTargetRotation.x),
            glm::degrees(document.ikTargetRotation.y),
            glm::degrees(document.ikTargetRotation.z) };
        if (ui.DragFloat3("IK target rotation (degrees)", targetAngles, .5f))
        {
            document.ikTargetRotation = glm::radians(glm::vec3(
                targetAngles[0], targetAngles[1], targetAngles[2]));
            changed = true;
        }
        changed = ui.DragFloat("IK position weight",
            &document.ikPositionWeight, .01f, 0.f, 1.f) || changed;
        changed = ui.DragFloat("IK rotation weight",
            &document.ikRotationWeight, .01f, 0.f, 1.f) || changed;
        if (ui.Button("Switch to IK (match pose)"))
        {
            document.ikBlend = 1.f;
            AddAnimationLimbKey(document, *upper, *lower, *joint, true);
        }
        if (ui.Button("Switch to FK (match pose)"))
        {
            document.ikBlend = 0.f;
            AddAnimationLimbKey(document, *upper, *lower, *joint, true);
        }
        if (ui.DragFloat("FK / IK blend", &document.ikBlend, .01f, 0.f, 1.f))
        {
            document.ikBlend = std::clamp(document.ikBlend, 0.f, 1.f);
            changed = true;
        }
        if (changed)
        {
            const glm::mat4 targetWorld = glm::translate(glm::mat4(1.f),
                document.ikTarget) * glm::mat4_cast(
                    glm::quat(document.ikTargetRotation));
            if (Engine::Components::TwoBoneIK::Solve(*upper, *lower, *joint,
                    document.ikTarget, document.ikPole,
                    document.ikPositionWeight, document.ikRotationWeight,
                    &targetWorld))
            {
                upper->transform.NotifyEditorTransformChanged(
                    Engine::Components::Transform::EditorRotation);
                lower->transform.NotifyEditorTransformChanged(
                    Engine::Components::Transform::EditorRotation);
                joint->transform.NotifyEditorTransformChanged(
                    Engine::Components::Transform::EditorRotation);
                document.ikControlEditPending = true;
                if (m_renderer) m_renderer->MarkDirty();
            }
        }
        if (document.ikControlEditPending && !ui.IsAnyItemActive())
        {
            document.ikControlEditPending = false;
            if (document.autoKey)
                AddAnimationLimbKey(document, *upper, *lower, *joint, false);
        }
        if (ui.Button("Key FK/IK controls"))
            AddAnimationLimbKey(document, *upper, *lower, *joint, false);
        if (ui.Button("Key current IK pose to bones"))
            AddAnimationIKKeys(document, *upper, *lower, *joint);
        if (ui.Button("Bake IK and controllers to bone keys (30 fps)"))
            BakeAnimationLimbs(document);
    }
}


}
