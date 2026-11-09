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

void EditorState::ProcessPendingAnimationDocumentOpens()
{
    if (!m_pendingAnimationDocuments.empty())
    {
        bool hasTimeline = false;
        for (const auto& panel : m_panels)
            if (dynamic_cast<AnimationTimelineView*>(panel.get()))
            { panel->SetOpen(true); hasTimeline = true; break; }
        if (!hasTimeline)
            if (auto timeline = m_viewFactory->Create("Animation Timeline"))
                m_panels.push_back(std::move(timeline));
    }
    while (!m_pendingAnimationDocuments.empty())
    {
        const auto [path, rigIndex] =
            std::move(m_pendingAnimationDocuments.front());
        m_pendingAnimationDocuments.pop_front();
        if (path.empty())
        {
            auto document = std::make_unique<AnimationDocument>();
            document->identity = "__EmptyAnimationEditor__";
            document->poseScene = std::make_unique<Engine::Scene::Scene>();
            document->poseScene->Init(m_renderer->GetGraphicsProvider());
            auto view = m_viewFactory->CreateAnimationView(
                document->poseScene.get(), "Animation Editor");
            if (!view) continue;
            AnimationDocument* raw = document.get();
            raw->view = view.get();
            view->SetEditMode(SceneEditMode::Skeleton);
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
            view->OnDocumentDrop = [this, raw](IEditorUi& ui) { HandleAnimationEditorDrop(ui, raw); };
            view->OnDrawDocumentTools = [](IEditorUi& ui)
            {
                ui.DisabledLabel("Drop a prefab, scene object, or .anim here to edit animations.");
                return false;
            };
            view->OnFocused = [this, raw]()
            { SetActiveAnimationDocument(raw); };
            view->RequestFocusOnNextDraw();
            m_panels.push_back(std::move(view));
            m_animationDocuments.push_back(std::move(document));
            SetActiveAnimationDocument(raw);
            continue;
        }
        const bool selectAll = rigIndex == static_cast<size_t>(-1);
        const size_t initialRig = selectAll ? 0 : rigIndex;
        auto document = std::make_unique<AnimationDocument>();
        document->path = path;
        document->rigIndex = initialRig;
        document->identity = AnimationIdentity(path);
        document->sourceScene = std::make_unique<Engine::Scene::Scene>();
        document->sourceScene->Init(m_renderer->GetGraphicsProvider());
        if (auto source = m_pendingAnimationSources.find(path); source != m_pendingAnimationSources.end())
        {
            if (document->sourceScene->LoadFromString(source->second.snapshot) &&
                !document->sourceScene->GetObjects().empty())
                document->sourceRoot = document->sourceScene->GetObjects().front().get();
            document->sourceObject = source->second.sourceObject;
            document->standaloneAssetPath = source->second.standaloneAssetPath;
            if (!source->second.prefabPath.empty()) document->path = source->second.prefabPath;
            m_pendingAnimationSources.erase(source);
        }
        else document->sourceRoot = Engine::Serialization::SceneSerializer::
            InstantiatePrefab(*document->sourceScene, path,
                document->sourceScene->GetGraphicsProvider());
        if (!document->sourceRoot)
        {
            m_animationDocumentOpenError = "Could not open animation prefab: " + path +
                ". Check that its skeleton and rig assets are readable.";
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Could not open animation prefab: " + path);
            continue;
        }
        document->sourceRoot->Prefab.reset();
        auto* sourceSkeleton = RigAt(*document->sourceScene, initialRig);
        if (!sourceSkeleton || !sourceSkeleton->ResolveModel())
        {
            m_animationDocumentOpenError = "Could not open " + path +
                ": the prefab needs a resolved skeleton and model.";
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Animation Editor needs a resolved skeleton and model.");
            continue;
        }
        document->rigName = sourceSkeleton->Owner->name;
        document->rigPath = sourceSkeleton->rigPath;
        document->boundMeshNames = BoundMeshNames(*document->sourceScene,
            sourceSkeleton);
        if (!document->rigPath.empty())
        {
            auto rig = Engine::Model::RigAsset::Load(
                document->rigPath, sourceSkeleton->skinIndex);
            if (!rig || !RigMatchesSkeleton(*rig, *sourceSkeleton))
                document->error = "Rig asset is missing or out of date; saving the clip will regenerate it.";
        }
        bool managerAmbiguous = false;
        document->sourceManager = ManagerForRig(*document->sourceScene,
            sourceSkeleton, &managerAmbiguous);
        if (managerAmbiguous)
        {
            m_animationDocumentOpenError = "Could not open " + path +
                ": multiple Animation Managers target this rig.";
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Multiple Animation Managers target this rig; select or repair the manager binding first.");
            continue;
        }
        if (!document->sourceManager)
        {
            document->sourceManager = sourceSkeleton->Owner->AddComponent<
                Engine::Components::AnimationManager>();
            document->sourceManager->modelReference =
                Engine::Core::CaptureComponentReference(
                    sourceSkeleton->ResolveModel(), "Model");
        }
        document->sourceManager->RefreshRigClips();
        if (auto asset = m_pendingAnimationClipAssets.find(document->identity);
            asset != m_pendingAnimationClipAssets.end())
        {
            if (const auto clipAsset = Engine::Model::AnimationClipAsset::Load(asset->second))
            {
                document->clipAssetPaths[clipAsset->clip.clipName] = asset->second;
                if (!document->sourceManager->FindClip(clipAsset->clip.clipName))
                {
                    std::string error;
                    if (!document->sourceManager->AddAnimationAsset(asset->second, error))
                        document->error = error;
                }
            }
            m_pendingAnimationClipAssets.erase(asset);
        }
        for (const auto& object : document->sourceScene->GetObjects())
            for (auto* component : object->Components)
                if (auto* manager = dynamic_cast<Engine::Components::AnimationManager*>(component))
                {
                    manager->RefreshRigClips();
                    manager->InstallEmbeddedClips(manager->clips);
                }
        document->savedSnapshot = document->sourceScene->SaveSnapshotToString();
        document->poseScene = std::make_unique<Engine::Scene::Scene>();
        document->poseScene->Init(m_renderer->GetGraphicsProvider());
        if (!document->poseScene->LoadFromString(document->savedSnapshot))
        {
            m_animationDocumentOpenError = "Could not create an animation preview for: " + path;
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Could not create an isolated animation pose scene.");
            continue;
        }
        document->poseSkeleton = RigAt(*document->poseScene, initialRig);
        document->poseManager = ManagerForRig(*document->poseScene,
            document->poseSkeleton);
        if (!document->poseSkeleton || !document->poseManager)
            continue;
        if (!document->poseScene->GetObjects().empty())
            document->poseScene->GetObjects().front()->enabled = true;
        for (size_t index = 0; auto* sourceRig =
                RigAt(*document->sourceScene, index); ++index)
        {
            auto* poseRig = RigAt(*document->poseScene, index);
            if (!poseRig) break;
            auto* sourceManager = ManagerForRig(*document->sourceScene,
                sourceRig);
            auto* poseManager = ManagerForRig(*document->poseScene, poseRig);
            if (!sourceManager || !poseManager) continue;
            sourceManager->RefreshRigClips();
            AnimationDocument::RigTrack track;
            track.index = index;
            track.name = sourceRig->Owner->name;
            if (std::count_if(sourceRig->Owner->Components.begin(), sourceRig->Owner->Components.end(),
                    [](const auto* component) {return dynamic_cast<const Engine::Components::Skeleton*>(component)!=nullptr;}) > 1)
                track.name += " / rig " + std::to_string(sourceRig->skinIndex) + " (" +
                    std::to_string(sourceRig->jointNodes.size()) + " joints)";
            track.path = sourceRig->rigPath;
            if (!track.path.empty())
                if (auto rig = Engine::Model::RigAsset::Load(
                        track.path, sourceRig->skinIndex))
                {
                    track.controls = rig->controls;
                    track.humanoid = rig->humanoid;
                    track.poses = rig->poses;
                    track.symmetryPairs = rig->symmetryPairs;
                }
            if (!document->standaloneAssetPath.empty() &&
                std::filesystem::path(document->standaloneAssetPath).extension() != ".animpack")
                if (const auto asset = Engine::Model::AnimationClipAsset::Load(document->standaloneAssetPath))
                    track.humanoid = asset->humanoid;
            track.boundMeshes = BoundMeshNames(*document->sourceScene,
                sourceRig);
            track.sourceSkeleton = sourceRig;
            track.poseSkeleton = poseRig;
            track.sourceManager = sourceManager;
            track.poseManager = poseManager;
            AnimationTimelineEditing::Read(track, *sourceManager);
            track.selectedBone = poseRig->ResolveJoints().empty() ? -1 : 0;
            track.selected = selectAll || index == initialRig;
            document->tracks.push_back(std::move(track));
        }
        if (document->tracks.empty()) continue;
        document->poseScene->SetEditorMeshEditPose(false);
        document->poseScene->SetEditorWeightPaint(nullptr, -1);
        document->poseManager->playing = false;
        document->poseManager->holdCurrentPoseWhenStopped = false;
        document->poseManager->looping = false;
        document->poseManager->layers.clear();
        for (auto& track : document->tracks)
        {
            track.poseManager->playing = false;
            track.poseManager->holdCurrentPoseWhenStopped = false;
            track.poseManager->looping = false;
            track.poseManager->layers.clear();
            track.poseManager->Start();
        }
        for (const auto& track : document->tracks)
            if (track.index == initialRig)
            { document->clipIndex = track.clipIndex; break; }
        if (auto requested = m_pendingAnimationClipSelections.find(
                document->identity);
            requested != m_pendingAnimationClipSelections.end())
        {
            SelectAnimationDocumentClip(*document, requested->second,
                initialRig);
            m_pendingAnimationClipSelections.erase(requested);
        }
        const auto& joints = document->poseSkeleton->ResolveJoints();
        if (!joints.empty() && joints[0])
        {
            document->selectedBone = 0;
            document->poseScene->SetSelectedObject(joints[0]);
        }
        Engine::Core::Object* poseRoot = document->poseSkeleton->Owner;
        while (poseRoot && poseRoot->Parent) poseRoot = poseRoot->Parent;
        document->poseScene->FocusEditorCamera(poseRoot);
        document->poseScene->StopEditorCameraFollow();
        const std::string filename = std::filesystem::path(path).filename().string();
        auto view = m_viewFactory->CreateAnimationView(
            document->poseScene.get(), "Animation: " + filename);
        if (!view)
        {
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Could not create an Animation Editor viewport.");
            continue;
        }
        // Animation authoring starts with its rig visible, independently of
        // the Scene View's saved overlay preferences.
        document->poseScene->SetEditorBonesVisible(true);
        for (const auto& object : document->poseScene->GetObjects())
            for (auto* component : object->Components)
                if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
                    skeleton->showBones = true;
        AnimationDocument* raw = document.get();
        raw->view = view.get();
        view->SetDocumentPath(path);
        view->SetEditMode(SceneEditMode::Skeleton);
        view->SetPoseEditing(true);
        view->SetModelManipulation(true);
        view->SetTransformTool(EditorTransformTool::Rotate);
        view->AllowObjectCreation = false;
        view->AllowAssetDrops = false;
        view->CanSelectObject = [raw](const Engine::Core::Object* object)
        {
            if (!object) return false;
            for (const auto& track : raw->tracks)
                if (track.selected)
                    for (auto* joint : track.poseSkeleton->ResolveJoints())
                        if (joint == object) return true;
            if (const auto* point = object->GetComponent<
                    Engine::Components::AnimationBone>();
                point && point->poseControlOnly)
                for (const auto& track : raw->tracks)
                    if (track.selected)
                        if (const auto* model =
                                track.poseSkeleton->ResolveModel())
                            for (auto* node : model->ResolveNodes())
                                if (node == object) return true;
            return false;
        };
        view->OnObjectSelected = [this, raw](Engine::Core::Object* object)
        {
            if (!object) return;
            for (const auto& track : raw->tracks)
                if (track.selected)
                {
                    const auto& bones = track.poseSkeleton->ResolveJoints();
                    for (size_t index = 0; index < bones.size(); ++index)
                        if (bones[index] == object)
                        {
                            if (raw->repairAwaitingViewportBone &&
                                raw->repairRigIndex == track.index)
                            {
                                const auto rigBones = AnimationClipMapping::RigNodes(
                                    *track.poseSkeleton);
                                const auto* model = track.poseSkeleton->ResolveModel();
                                const auto* bone = object->GetComponent<
                                    Engine::Components::AnimationBone>();
                                const unsigned node = bone ? bone->nodeIndex :
                                    index < track.poseSkeleton->jointNodes.size()
                                        ? track.poseSkeleton->jointNodes[index] : 0;
                                const auto found = std::find_if(rigBones.begin(),
                                    rigBones.end(), [node](const auto& item)
                                    { return item.nodeIndex == node; });
                                if (model && found != rigBones.end())
                                    AssignAnimationChannels(*raw, track.index,
                                        raw->repairClipIndex,
                                        raw->repairIssueChannel,
                                        static_cast<size_t>(found - rigBones.begin()));
                            }
                            FocusAnimationRig(*raw, track.index);
                            raw->selectedBone = static_cast<int>(index);
                            raw->poseScene->SetSelectedObject(object);
                            if (m_primaryHierarchy)
                                m_primaryHierarchy->SetSelectedObject(object);
                            if (m_primaryProperties)
                                m_primaryProperties->SetSelectedObject(object);
                            return;
                        }
                }
            if (const auto* controller = object->GetComponent<
                    Engine::Components::AnimationBone>();
                controller && controller->poseControlOnly)
                for (const auto& track : raw->tracks)
                    if (track.selected)
                        if (const auto* model =
                                track.poseSkeleton->ResolveModel())
                            for (auto* node : model->ResolveNodes())
                                if (node == object)
                                {
                                    FocusAnimationRig(*raw, track.index);
                                    raw->selectedBone = -1;
                                    raw->poseScene->SetSelectedObject(object);
                                    if (m_primaryHierarchy)
                                        m_primaryHierarchy->SetSelectedObject(object);
                                    if (m_primaryProperties)
                                        m_primaryProperties->SetSelectedObject(object);
                                    return;
                                }
        };
        view->SetPoseTranslationHandler([this, raw](Engine::Core::Object* tip,
            const glm::vec3& target)
        {
            auto* skeleton = raw->poseSkeleton;
            if (!skeleton || !tip) return false;
            const auto& joints = skeleton->ResolveJoints();
            auto* lower = tip->Parent;
            auto* upper = lower ? lower->Parent : nullptr;
            if (!upper || std::find(joints.begin(), joints.end(), upper) == joints.end() ||
                std::find(joints.begin(), joints.end(), lower) == joints.end()) return false;
            const auto track = std::find_if(raw->tracks.begin(), raw->tracks.end(),
                [raw](const auto& item) { return item.index == raw->rigIndex; });
            if (track == raw->tracks.end()) return false;
            const auto& nodes = skeleton->jointNodes;
            const auto tipIndex = std::find(joints.begin(), joints.end(), tip) - joints.begin();
            if (static_cast<size_t>(tipIndex) >= nodes.size()) return false;
            const auto isTip = [&](const auto& controls)
            {
                return std::any_of(controls.limbs.begin(), controls.limbs.end(),
                    [&](const auto& limb) { return limb.tip == static_cast<int>(nodes[tipIndex]); });
            };
            if (!raw->gizmoDragIK && !isTip(track->controls) &&
                !isTip(HumanoidControls(*skeleton))) return false;
            raw->playing = false;
            if (!raw->gizmoDragIK)
            {
                const glm::vec3 root = upper->transform.GetWorldPosition();
                const glm::vec3 end = tip->transform.GetWorldPosition();
                raw->ikPole = lower->transform.GetWorldPosition() * 2.f - (root + end) * .5f;
                glm::mat3 basis(tip->transform.GetWorldMatrix());
                for (int axis = 0; axis < 3; ++axis)
                    if (glm::length(basis[axis]) > 1e-6f) basis[axis] = glm::normalize(basis[axis]);
                raw->ikTargetRotation = glm::eulerAngles(glm::normalize(glm::quat_cast(basis)));
                raw->ikControlTip = static_cast<int>(tipIndex);
                raw->ikControlTime = raw->playheadSeconds;
                raw->ikSpace = Engine::Model::AnimationClip::LimbKey::Space::Body;
                raw->ikSpaceNode = -1;
                raw->ikPropReference.Clear();
            }
            raw->ikTarget = target;
            raw->ikBlend = 1.f;
            if (!Engine::Components::TwoBoneIK::Blend(*upper, *lower, *tip,
                    target, raw->ikPole, glm::quat(raw->ikTargetRotation), 1.f)) return false;
            raw->gizmoDragIK = true;
            for (auto* joint : {upper, lower, tip})
                joint->transform.NotifyEditorTransformChanged(Engine::Components::Transform::EditorRotation);
            if (m_renderer) m_renderer->MarkDirty();
            return true;
        });
        view->OnGizmoInteraction = [this, raw](bool active)
        {
            if (active && !raw->gizmoWasActive)
            {
                raw->playing = false;
                raw->gizmoDragIK = false;
                raw->gizmoWasActive = true;
                raw->gizmoDragBone = raw->poseScene
                    ? raw->poseScene->GetSelectedObject() : nullptr;
                if (raw->gizmoDragBone)
                {
                    const auto& transform = raw->gizmoDragBone->transform;
                    raw->gizmoDragStartPosition = transform.position;
                    raw->gizmoDragStartRotation = transform.rotation;
                    raw->gizmoDragStartScale = transform.scale;
                }
            }
            else if (raw->gizmoWasActive)
            {
                if (!active)
                {
                    raw->gizmoWasActive = false;
                    auto* bone = raw->gizmoDragBone;
                    const bool changed = raw->gizmoDragIK ||
                        (raw->view && raw->view->ModelDragEnded() &&
                            !raw->view->ModelChangedBones().empty()) || (bone &&
                        (glm::vec3(bone->transform.position) !=
                            raw->gizmoDragStartPosition ||
                         bone->transform.rotation !=
                            raw->gizmoDragStartRotation ||
                         bone->transform.scale !=
                            raw->gizmoDragStartScale));
                    raw->gizmoDragBone = nullptr;
                    if (raw->autoKey && changed)
                    {
                        const bool modelDrag = raw->view && raw->view->ModelDragEnded();
                        std::vector<std::pair<Engine::Components::AnimationBone*,
                            Engine::Model::AnimationClip::ControllerKey>>
                            controllerState;
                        if (modelDrag && raw->poseSkeleton)
                            for (auto* joint : raw->poseSkeleton->ResolveJoints())
                                if (auto* controller = joint ?
                                        joint->GetComponent<
                                            Engine::Components::AnimationBone>()
                                        : nullptr)
                                {
                                    Engine::Model::AnimationClip::ControllerKey state;
                                    state.active = controller->poseControllerActive;
                                    state.target = controller->poseControllerTarget;
                                    state.fixedPosition = controller->posePinPosition;
                                    state.fixedRotation = controller->posePinRotation;
                                    state.fixedTarget = controller->posePinTarget;
                                    state.fixedRotationEuler =
                                        controller->posePinRotationEuler;
                                    state.pole = controller->posePinPole;
                                    controllerState.emplace_back(controller, state);
                                }
                        std::vector<Engine::Core::Object*> affected = modelDrag
                            ? raw->view->ModelChangedBones()
                            : std::vector<Engine::Core::Object*>{};
                        bool poseKeyed = false;
                        if (raw->gizmoDragIK && bone && bone->Parent && bone->Parent->Parent)
                        {
                            poseKeyed = AddAnimationLimbKey(*raw,
                                *bone->Parent->Parent, *bone->Parent,
                                *bone, false);
                            affected.erase(std::remove_if(affected.begin(),
                                affected.end(), [bone](Engine::Core::Object* item) {
                                    return item == bone || item == bone->Parent ||
                                        item == bone->Parent->Parent;
                                }), affected.end());
                        }
                        if (!affected.empty())
                            poseKeyed = AddAnimationPoseKeys(*raw, affected) ||
                                poseKeyed;
                        else if (!raw->gizmoDragIK)
                            poseKeyed = AddAnimationKey(*raw) || poseKeyed;
                        if (modelDrag)
                        {
                            // Pose keying resamples the preview. Restore the
                            // controller targets reached by this drag before
                            // recording their key at the same playhead.
                            for (const auto& [controller, state] : controllerState)
                            {
                                controller->poseControllerActive = state.active;
                                controller->poseControllerTarget = state.target;
                                controller->posePinPosition = state.fixedPosition;
                                controller->posePinRotation = state.fixedRotation;
                                controller->posePinTarget = state.fixedTarget;
                                controller->posePinRotationEuler =
                                    state.fixedRotationEuler;
                                controller->posePinPole = state.pole;
                            }
                            AddAnimationControllerKeys(*raw, poseKeyed);
                        }
                    }
                    raw->gizmoDragIK = false;
                }
            }
            if (m_renderer && active) m_renderer->MarkDirty();
        };
        view->OnKeyModelControllersRequested = [this, raw]()
        { AddAnimationControllerKeys(*raw); };
        view->OnModelControllerEdited = [this, raw]()
        {
            auto* sourceSkeleton = RigAt(*raw->sourceScene,
                raw->rigIndex);
            auto* sourceModel = sourceSkeleton
                ? sourceSkeleton->ResolveModel() : nullptr;
            auto* poseModel = raw->poseSkeleton
                ? raw->poseSkeleton->ResolveModel() : nullptr;
            if (sourceModel && poseModel)
            {
                const auto& sourceNodes = sourceModel->ResolveNodes();
                const auto& poseNodes = poseModel->ResolveNodes();
                for (size_t node = 0; node < std::min(sourceNodes.size(),
                        poseNodes.size()); ++node)
                {
                    auto* sourceBone = sourceNodes[node]
                        ? sourceNodes[node]->GetComponent<
                            Engine::Components::AnimationBone>() : nullptr;
                    const auto* poseBone = poseNodes[node]
                        ? poseNodes[node]->GetComponent<
                            Engine::Components::AnimationBone>() : nullptr;
                    if (!sourceBone || !poseBone) continue;
                    sourceBone->poseControllerMode =
                        poseBone->poseControllerMode;
                    sourceBone->poseControllerScope =
                        poseBone->poseControllerScope;
                    sourceBone->poseControllerChainDepth =
                        poseBone->poseControllerChainDepth;
                    sourceBone->poseFacingEnabled =
                        poseBone->poseFacingEnabled;
                    sourceBone->poseFacingTarget =
                        poseBone->poseFacingTarget;
                    sourceBone->poseFacingAxis =
                        poseBone->poseFacingAxis;
                    sourceBone->poseFacingWeight =
                        poseBone->poseFacingWeight;
                    sourceBone->poseRestOffsetConfigured =
                        poseBone->poseRestOffsetConfigured;
                    sourceBone->poseRestLocalOffset =
                        poseBone->poseRestLocalOffset;
                }
                raw->dirty = true;
                RefreshAnimationDocumentTitle(*raw);
            }
            if (raw->autoKey) AddAnimationControllerKeys(*raw);
        };
        view->OnFocused = [this, raw]()
        { SetActiveAnimationDocument(raw); };
        view->OnDrawRigControls = [this, raw](IEditorUi& ui,
            const EditorUiViewportInput& input)
        { return DrawAnimationRigViewport(ui, input, *raw); };
        view->OnDocumentDrop = [this, raw](IEditorUi& ui) { HandleAnimationEditorDrop(ui, raw); };
        RefreshAnimationDocumentTitle(*raw);
        SampleAnimationDocument(*raw);
        view->RequestFocusOnNextDraw();
        m_animationDocumentOpenError.clear();
        for (auto& existing : m_animationDocuments)
            if (!existing->sourceManager && !existing->dirty)
                existing->view->SetOpen(false);
        m_panels.push_back(std::move(view));
        m_animationDocuments.push_back(std::move(document));
        SetActiveAnimationDocument(raw);
        for (const auto& open : m_animationDocuments)
            if (open && open->path.empty() && open->view)
                open->view->SetOpen(false);
    }
}

void EditorState::DrawAnimationCloseConfirmation(IEditorUi& ui)
{
    if (!ui.BeginModal("Close animation editor?", m_animationCloseDocument != nullptr)) return;
    auto* document = m_animationCloseDocument;
    if (!document)
    {
        ui.CloseModal();
        ui.EndModal();
        return;
    }
    ui.Label(document->view->GetTitle().c_str());
    ui.Label("This animation has unsaved changes.");
    if (!document->error.empty()) ui.ColoredLabel(document->error.c_str(), {1.f, .5f, .35f, 1.f});
    if (ui.Button("Save and close"))
    {
        if (SaveAnimationDocument(*document) && !document->dirty)
        {
            document->view->SetOpen(false);
            m_animationCloseDocument = nullptr;
            ui.CloseModal();
        }
    }
    ui.SameLine();
    if (ui.Button("Discard changes"))
    {
        document->dirty = false;
        document->view->SetOpen(false);
        m_animationCloseDocument = nullptr;
        ui.CloseModal();
    }
    ui.SameLine();
    if (ui.Button("Cancel"))
    {
        document->view->SetOpen(true);
        m_animationCloseDocument = nullptr;
        ui.CloseModal();
    }
    ui.EndModal();
}

void EditorState::HandleAnimationDocumentClosures()
{
    bool closed = false;
    for (auto it = m_animationDocuments.begin(); it != m_animationDocuments.end();)
    {
        AnimationDocument* document = it->get();
        if (document->view->IsOpen())
        { ++it; continue; }
        if (document->dirty)
        {
            document->view->SetOpen(true);
            if (!m_animationCloseDocument) m_animationCloseDocument = document;
            ++it;
            continue;
        }
        if (m_activeAnimationDocument == document)
            SetActiveAnimationDocument(nullptr);
        if (m_timelineAnimationDocument == document)
            m_timelineAnimationDocument = nullptr;
        if (m_animationCloseDocument == document) m_animationCloseDocument = nullptr;
        RetiredAnimationDocument retired;
        for (IEditorPanel* closing : { static_cast<IEditorPanel*>(document->view) })
            for (auto panel = m_panels.begin(); panel != m_panels.end(); ++panel)
                if (panel->get() == closing)
                {
                    if (auto* view = dynamic_cast<View*>(closing))
                        retired.srvSlot = view->GetSrvSlotIndex();
                    m_viewFactory->NotifyPanelRemoved(closing);
                    retired.panel = std::move(*panel);
                    m_panels.erase(panel);
                    break;
                }
        // UI drawing can close this viewport after its rendering commands and
        // ImGui texture references have already been recorded for this frame.
        retired.document = std::move(*it);
        m_retiredAnimationDocuments.push_back(std::move(retired));
        it = m_animationDocuments.erase(it);
        closed = true;
    }
    if (!m_timelineAnimationDocument && !m_animationDocuments.empty())
        m_timelineAnimationDocument = m_animationDocuments.back().get();
    if (closed) SaveAnimationEditorSession();
}

void EditorState::ReleaseRetiredAnimationDocuments()
{
    if (m_retiredAnimationDocuments.empty()) return;
    // Called after RenderIfNeeded submits the command list. Waiting here also
    // protects references from earlier in-flight frames before reusing slots.
    m_renderer->WaitIdle();
    for (auto& retired : m_retiredAnimationDocuments)
    {
        retired.panel.reset();
        retired.document.reset();
        if (retired.srvSlot != UINT32_MAX)
            m_viewFactory->FreeSrvSlot(retired.srvSlot);
    }
    m_retiredAnimationDocuments.clear();
}
}
