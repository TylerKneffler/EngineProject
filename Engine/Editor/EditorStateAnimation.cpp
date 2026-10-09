#include "pch.h"
#include "EditorState.h"
#include "Core/AnimationClipEditing.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/Views/AnimationView.h"
#include "Core/View/Views/AnimationTimelineView.h"
#include "Core/View/Views/GameView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/SkeletonBindPose.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/ComponentReference.h"
#include "Core/Model/RigAsset.h"
#include "Core/Object.h"
#include "Core/Prefab/PrefabAsset.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <cctype>
#include <unordered_set>

namespace Engine::Editor
{
namespace
{
std::string AnimationIdentity(const std::string& path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = std::filesystem::path(path).lexically_normal();
    std::string result = normalized.generic_string();
#ifdef _WIN32
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
    return result;
}

Engine::Components::Skeleton* RigAt(Engine::Scene::Scene& scene,
    size_t rigIndex)
{
    for (const auto& object : scene.GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* skeleton = dynamic_cast<
                        Engine::Components::Skeleton*>(component))
                {
                    if (rigIndex == 0) return skeleton;
                    --rigIndex;
                }
    return nullptr;
}

Engine::Components::Model* ManagerModel(
    Engine::Components::AnimationManager& manager)
{
    if (manager.modelReference.IsAssigned())
        return Engine::Core::ResolveComponentReference<
            Engine::Components::Model>(manager.Owner,
                manager.modelReference);
    for (auto* object = manager.Owner; object; object = object->Parent)
        if (auto* model = object->GetComponent<Engine::Components::Model>())
            return model;
    return nullptr;
}

Engine::Components::AnimationManager* ManagerForRig(
    Engine::Scene::Scene& scene, Engine::Components::Skeleton* skeleton,
    bool* ambiguous = nullptr)
{
    if (ambiguous) *ambiguous = false;
    auto* model = skeleton ? skeleton->ResolveModel() : nullptr;
    if (!model) return nullptr;
    Engine::Components::AnimationManager* sole = nullptr;
    Engine::Components::AnimationManager* onRig = nullptr;
    size_t count = 0;
    size_t onRigCount = 0;
    for (const auto& object : scene.GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* manager = dynamic_cast<
                        Engine::Components::AnimationManager*>(component);
                    manager && ManagerModel(*manager) == model)
                {
                    sole = manager;
                    ++count;
                    if (manager->Owner == skeleton->Owner)
                    { onRig = manager; ++onRigCount; }
                }
    if (onRigCount == 1) return onRig;
    if (count == 1) return sole;
    if (ambiguous && count > 1) *ambiguous = true;
    return nullptr;
}

std::vector<std::string> BoundMeshNames(Engine::Scene::Scene& scene,
    Engine::Components::Skeleton* skeleton)
{
    std::vector<std::string> names;
    if (!skeleton) return names;
    for (const auto& object : scene.GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* skin = dynamic_cast<
                        Engine::Components::SkinnedMesh*>(component);
                    skin && skin->ResolveSkeleton() == skeleton)
                    names.push_back(object->name);
    return names;
}

std::string RigIdentity(const std::string& path, size_t rigIndex)
{ return AnimationIdentity(path) + "#rig=" + std::to_string(rigIndex); }

bool RigMatchesSkeleton(const Engine::Model::RigAsset& rig,
    Engine::Components::Skeleton& skeleton)
{
    if (rig.skinIndex != skeleton.skinIndex ||
        rig.joints.size() != skeleton.jointNodes.size()) return false;
    const auto& joints = skeleton.ResolveJoints();
    for (size_t i = 0; i < rig.joints.size(); ++i)
        if (rig.joints[i].nodeIndex != skeleton.jointNodes[i] ||
            i >= joints.size() || !joints[i] ||
            rig.joints[i].name != joints[i]->name)
            return false;
    return true;
}

std::string LowerText(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

float LastKeyTime(const Engine::Model::AnimationClip& clip)
{
    float result = 0.f;
    for (const auto& channel : clip.channels)
        if (!channel.times.empty())
            result = std::max(result, channel.times.back());
    return result;
}
}

std::pair<std::string, size_t> EditorState::SelectedAnimationRig() const
{
    auto* scene = GetActiveDocumentScene();
    auto* selected = scene ? scene->GetSelectedObject() : nullptr;
    if (!selected || m_activeAnimationDocument) return {};

    auto* prefabRoot = selected->GetPrefabInstanceRoot();
    auto samePrefab = [&](Engine::Components::Skeleton* skeleton)
    {
        return skeleton && skeleton->Owner &&
            (!prefabRoot || skeleton->Owner->GetPrefabInstanceRoot() == prefabRoot);
    };
    Engine::Components::Skeleton* rig = nullptr;
    if (auto* skin = selected->GetComponent<Engine::Components::SkinnedMesh>())
        rig = skin->ResolveSkeleton();
    if (!rig)
    {
        size_t localRigs = 0;
        for (auto* component : selected->Components)
            if (auto* candidate = dynamic_cast<Engine::Components::Skeleton*>(component))
            { rig = candidate; ++localRigs; }
        if (localRigs > 1) rig = nullptr;
    }
    if (!rig)
    {
        size_t jointRigs = 0;
        for (const auto& object : scene->GetObjects())
            if (object)
                for (auto* component : object->Components)
                    if (auto* candidate = dynamic_cast<Engine::Components::Skeleton*>(component);
                        samePrefab(candidate))
                        for (auto* joint : candidate->ResolveJoints())
                            if (joint == selected)
                            { rig = candidate; ++jointRigs; }
        if (jointRigs > 1) return {};
    }

    if (!rig)
    {
        Engine::Components::AnimationManager* manager = nullptr;
        for (auto* object = selected; object; object = object->Parent)
            if ((manager = object->GetComponent<Engine::Components::AnimationManager>()))
                break;
        if (!manager) return {};
        auto* model = ManagerModel(*manager);
        std::vector<Engine::Components::Skeleton*> matches;
        for (const auto& object : scene->GetObjects())
            if (object)
                for (auto* component : object->Components)
                    if (auto* candidate = dynamic_cast<Engine::Components::Skeleton*>(component);
                        samePrefab(candidate) && candidate->ResolveModel() == model)
                        matches.push_back(candidate);
        if (matches.size() == 1) rig = matches.front();
        else
        {
            size_t pathMatches = 0;
            for (auto* candidate : matches)
                if (!candidate->rigPath.empty() &&
                    std::find(manager->rigPaths.begin(), manager->rigPaths.end(),
                        candidate->rigPath) != manager->rigPaths.end())
                { rig = candidate; ++pathMatches; }
            if (pathMatches != 1) return {};
        }
    }
    if (!samePrefab(rig) || !rig->ResolveModel()) return {};

    std::string path;
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab)
        path = m_activeSceneAssetDocument->stageDataPath.empty()
            ? m_activeSceneAssetDocument->path
            : m_activeSceneAssetDocument->stageDataPath;
    else if (m_prefabDocumentFocused && !m_activePrefabPath.empty())
        path = m_activePrefabPath;
    else if (auto* root = rig->Owner->GetPrefabInstanceRoot();
        root && root->Prefab)
        path = root->Prefab->GetPath();
    if (path.empty()) return {};

    size_t ordinal = 0;
    auto* rigRoot = rig->Owner->GetPrefabInstanceRoot();
    for (const auto& object : scene->GetObjects())
        if (object && (!rigRoot || object->GetPrefabInstanceRoot() == rigRoot))
            for (auto* component : object->Components)
                if (auto* candidate = dynamic_cast<Engine::Components::Skeleton*>(component))
                {
                    if (candidate == rig) return { path, ordinal };
                    ++ordinal;
                }
    return {};
}

bool EditorState::CanOpenAnimationForSelection() const
{
    return !SelectedAnimationRig().first.empty();
}

void EditorState::OpenAnimationForSelection()
{
    const auto [path, ordinal] = SelectedAnimationRig();
    if (path.empty()) return;
    const char* error = nullptr;
    if (m_activeSceneAssetDocument &&
        (m_activeSceneAssetDocument->dirty ||
            m_activeSceneAssetDocument->meshEdit.dirty))
        error = "Save the rig and weights before opening the Animation Editor.";
    else if (m_prefabDocumentFocused && m_prefabHasUnsavedChanges)
        error = "Save the prefab before opening the Animation Editor.";
    if (error)
    {
        if (m_primaryConsole) m_primaryConsole->AddLog(ConsoleView::Level::Warning, error);
        return;
    }
    QueueAnimationDocumentOpen(path, ordinal);
}

void EditorState::QueueAnimationDocumentOpen(const std::string& path,
    size_t rigIndex)
{
    if (path.empty()) return;
    const std::string identity = RigIdentity(path, rigIndex);
    for (const auto& document : m_animationDocuments)
        if (document && document->identity == identity)
        {
            document->view->SetOpen(true);
            document->timeline->SetOpen(true);
            SetActiveAnimationDocument(document.get());
            return;
        }
    for (const auto& document : m_animationDocuments)
        if (document && AnimationIdentity(document->path) ==
                AnimationIdentity(path) && document->dirty)
        {
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Warning,
                "Save the current animation before switching rigs in this prefab.");
            return;
        }
    for (const auto& pending : m_pendingAnimationDocuments)
        if (RigIdentity(pending.first, pending.second) == identity) return;
    m_pendingAnimationDocuments.emplace_back(path, rigIndex);
}

void EditorState::ProcessPendingAnimationDocumentOpens()
{
    while (!m_pendingAnimationDocuments.empty())
    {
        const auto [path, rigIndex] =
            std::move(m_pendingAnimationDocuments.front());
        m_pendingAnimationDocuments.pop_front();
        bool blocked = false;
        for (const auto& open : m_animationDocuments)
            if (open && AnimationIdentity(open->path) ==
                    AnimationIdentity(path))
            {
                if (open->dirty)
                { blocked = true; break; }
                open->view->SetOpen(false);
                open->timeline->SetOpen(false);
            }
        if (blocked) continue;
        HandleAnimationDocumentClosures();
        auto document = std::make_unique<AnimationDocument>();
        document->path = path;
        document->rigIndex = rigIndex;
        document->identity = RigIdentity(path, rigIndex);
        document->sourceScene = std::make_unique<Engine::Scene::Scene>();
        document->sourceScene->Init(m_renderer->GetGraphicsProvider());
        document->sourceRoot = Engine::Serialization::SceneSerializer::
            InstantiatePrefab(*document->sourceScene, path,
                document->sourceScene->GetGraphicsProvider());
        if (!document->sourceRoot)
        {
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Could not open animation prefab: " + path);
            continue;
        }
        document->sourceRoot->Prefab.reset();
        auto* sourceSkeleton = RigAt(*document->sourceScene, rigIndex);
        if (!sourceSkeleton || !sourceSkeleton->ResolveModel())
        {
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Animation Editor needs a prefab with a resolved skeleton and model.");
            continue;
        }
        document->rigName = sourceSkeleton->Owner->name;
        document->rigPath = sourceSkeleton->rigPath;
        document->boundMeshNames = BoundMeshNames(*document->sourceScene,
            sourceSkeleton);
        if (!document->rigPath.empty())
        {
            auto rig = Engine::Model::RigAsset::Load(document->rigPath);
            if (!rig || !RigMatchesSkeleton(*rig, *sourceSkeleton))
                document->error = "Rig asset is missing or out of date; saving the clip will regenerate it.";
        }
        bool managerAmbiguous = false;
        document->sourceManager = ManagerForRig(*document->sourceScene,
            sourceSkeleton, &managerAmbiguous);
        if (managerAmbiguous)
        {
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
        document->savedSnapshot = document->sourceScene->SaveToString();
        document->poseScene = std::make_unique<Engine::Scene::Scene>();
        document->poseScene->Init(m_renderer->GetGraphicsProvider());
        if (!document->poseScene->LoadFromString(document->savedSnapshot))
        {
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Could not create an isolated animation pose scene.");
            continue;
        }
        document->poseSkeleton = RigAt(*document->poseScene, rigIndex);
        document->poseManager = ManagerForRig(*document->poseScene,
            document->poseSkeleton);
        if (!document->poseSkeleton || !document->poseManager)
            continue;
        document->poseScene->SetEditorMeshEditPose(false);
        document->poseScene->SetEditorWeightPaint(nullptr, -1);
        document->poseManager->playing = false;
        document->poseManager->holdCurrentPoseWhenStopped = false;
        document->poseManager->looping = false;
        document->poseManager->layers.clear();
        document->poseManager->Start();
        if (!document->sourceManager->clips.empty())
            document->clipIndex = 0;
        const auto& joints = document->poseSkeleton->ResolveJoints();
        if (!joints.empty() && joints[0])
        {
            document->selectedBone = 0;
            document->poseScene->SetSelectedObject(joints[0]);
        }
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
        auto timeline = std::make_unique<AnimationTimelineView>();
        timeline->SetDefaultDockArea(EditorPanelDockArea::BottomPanel);
        AnimationDocument* raw = document.get();
        raw->view = view.get();
        raw->timeline = timeline.get();
        view->SetDocumentPath(path);
        view->SetEditMode(SceneEditMode::Skeleton);
        view->AllowObjectCreation = false;
        view->AllowAssetDrops = false;
        view->CanSelectObject = [raw](const Engine::Core::Object* object)
        {
            if (!object) return false;
            for (auto* joint : raw->poseSkeleton->ResolveJoints())
                if (joint == object) return true;
            return false;
        };
        view->OnObjectSelected = [this, raw](Engine::Core::Object* object)
        {
            if (!object) return;
            const auto& bones = raw->poseSkeleton->ResolveJoints();
            for (size_t index = 0; index < bones.size(); ++index)
                if (bones[index] == object)
                {
                    raw->selectedBone = static_cast<int>(index);
                    raw->poseScene->SetSelectedObject(object);
                    if (m_primaryHierarchy)
                        m_primaryHierarchy->SetSelectedObject(object);
                    if (m_primaryProperties)
                        m_primaryProperties->SetSelectedObject(object);
                    break;
                }
        };
        view->OnGizmoInteraction = [this, raw](bool active)
        {
            if (active) raw->gizmoWasActive = true;
            else if (raw->gizmoWasActive)
            {
                raw->gizmoWasActive = false;
                if (raw->autoKey) AddAnimationKey(*raw);
            }
            if (m_renderer && active) m_renderer->MarkDirty();
        };
        view->OnFocused = [this, raw]()
        { SetActiveAnimationDocument(raw); };
        timeline->OnFocused = [this, raw]()
        { SetActiveAnimationDocument(raw); };
        timeline->OnDrawTimeline = [this, raw](IEditorUi& ui)
        { DrawAnimationTimeline(ui, *raw); };
        RefreshAnimationDocumentTitle(*raw);
        SampleAnimationDocument(*raw);
        view->RequestFocusOnNextDraw();
        m_panels.push_back(std::move(view));
        m_panels.push_back(std::move(timeline));
        m_animationDocuments.push_back(std::move(document));
        SetActiveAnimationDocument(raw);
    }
}

void EditorState::SetActiveAnimationDocument(AnimationDocument* document)
{
    if (!document)
    {
        m_activeAnimationDocument = nullptr;
        SetActiveSceneAssetDocument(nullptr, true);
        return;
    }
    if (m_activeAnimationDocument == document) return;
    m_activeAnimationDocument = document;
    m_activeSceneAssetDocument = nullptr;
    m_activeAssetDocument = nullptr;
    m_prefabDocumentFocused = false;
    m_gameViewFocused = false;
    for (auto& panel : m_panels)
        if (auto* game = dynamic_cast<GameView*>(panel.get()))
            game->SetScene(document->poseScene.get());
    if (m_primaryHierarchy)
    {
        m_primaryHierarchy->Init(document->poseScene.get());
        m_primaryHierarchy->SetObjectFilter([document](const Engine::Core::Object* object)
        {
            for (auto* joint : document->poseSkeleton->ResolveJoints())
                if (joint == object) return true;
            return false;
        });
        m_primaryHierarchy->SetAllowDelete(false);
        m_primaryHierarchy->SetFilteredObjectContextActions(false, nullptr);
        m_primaryHierarchy->SetSkeletonContextActions(false, nullptr);
        m_primaryHierarchy->SetSelectedObject(
            document->poseScene->GetSelectedObject());
    }
    if (m_primaryProperties)
    {
        m_primaryProperties->Init(document->poseScene.get());
        m_primaryProperties->SetAllowComponentStructureEdits(false);
        m_primaryProperties->SetSelectedObject(
            document->poseScene->GetSelectedObject());
        m_primaryProperties->SetSelectedAsset("");
    }
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::SampleAnimationDocument(AnimationDocument& document)
{
    if (!document.poseManager || !document.poseSkeleton ||
        !document.sourceManager) return;
    document.poseManager->clips = document.sourceManager->clips;
    const int clipIndex = document.clipIndex;
    const auto& clips = document.sourceManager->clips;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips.size()))
    {
        auto* sourceSkeleton = RigAt(*document.sourceScene,
            document.rigIndex);
        auto* sourceModel = sourceSkeleton ? sourceSkeleton->ResolveModel() : nullptr;
        auto* poseModel = document.poseSkeleton->ResolveModel();
        if (sourceModel && poseModel)
        {
            const auto& sourceNodes = sourceModel->ResolveNodes();
            const auto& poseNodes = poseModel->ResolveNodes();
            for (size_t index = 0; index < std::min(sourceNodes.size(),
                    poseNodes.size()); ++index)
                if (sourceNodes[index] && poseNodes[index])
                {
                    poseNodes[index]->transform.ClearEditorOverride();
                    poseNodes[index]->transform.position =
                        sourceNodes[index]->transform.position;
                    poseNodes[index]->transform.rotation =
                        sourceNodes[index]->transform.rotation;
                    poseNodes[index]->transform.scale =
                        sourceNodes[index]->transform.scale;
                }
        }
        if (m_renderer) m_renderer->MarkDirty();
        return;
    }
    for (auto* joint : document.poseSkeleton->ResolveJoints())
        if (joint) joint->transform.ClearEditorOverride();
    document.poseManager->clip = clips[clipIndex].clipName;
    document.poseManager->time = static_cast<float>(document.frame) /
        static_cast<float>(document.framesPerSecond);
    document.poseManager->Tick(0.f);
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::RecordAnimationEdit(AnimationDocument& document,
    std::vector<Engine::Model::AnimationClip> before)
{
    if (m_historyLimit > 0)
    {
        if (document.undo.size() >= m_historyLimit) document.undo.pop_front();
        document.undo.push_back(std::move(before));
    }
    document.redo.clear();
    document.dirty = document.sourceScene->SaveToString() !=
        document.savedSnapshot;
    RefreshAnimationDocumentTitle(document);
    SampleAnimationDocument(document);
}

bool EditorState::AddAnimationKey(AnimationDocument& document)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return false;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    if (document.selectedBone < 0 || document.selectedBone >=
            static_cast<int>(joints.size()) || !joints[document.selectedBone])
        return false;
    auto* bone = joints[document.selectedBone]->GetComponent<
        Engine::Components::AnimationBone>();
    if (!bone) return false;
    auto before = document.sourceManager->clips;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    const auto& transform = joints[document.selectedBone]->transform;
    const float frameTime = static_cast<float>(document.frame) /
        document.framesPerSecond;
    const float keyTime = AnimationClipEditing::NearestKeyTime(clip,
        bone->nodeIndex, frameTime, .5f / document.framesPerSecond)
        .value_or(frameTime);
    const bool keyed = AnimationClipEditing::UpsertBonePose(clip,
        bone->nodeIndex, keyTime,
        transform.position, glm::quat(transform.rotation), transform.scale);
    if (keyed) RecordAnimationEdit(document, std::move(before));
    return keyed;
}

bool EditorState::StepAnimationHistory(AnimationDocument& document, bool redo)
{
    auto& source = redo ? document.redo : document.undo;
    auto& target = redo ? document.undo : document.redo;
    if (source.empty()) return false;
    target.push_back(document.sourceManager->clips);
    document.sourceManager->clips = std::move(source.back());
    source.pop_back();
    document.dirty = document.sourceScene->SaveToString() !=
        document.savedSnapshot;
    RefreshAnimationDocumentTitle(document);
    SampleAnimationDocument(document);
    return true;
}

bool EditorState::SaveAnimationDocument(AnimationDocument& document)
{
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
    if (std::find(latestManager->rigPaths.begin(),
            latestManager->rigPaths.end(), rigPath) ==
        latestManager->rigPaths.end())
        latestManager->rigPaths.push_back(rigPath);
    latestManager->clips = document.sourceManager->clips;
    if (latestManager->clip.empty() && !latestManager->clips.empty())
        latestManager->clip = latestManager->clips.front().clipName;
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
    if (std::find(document.sourceManager->rigPaths.begin(),
            document.sourceManager->rigPaths.end(), rigPath) ==
        document.sourceManager->rigPaths.end())
        document.sourceManager->rigPaths.push_back(rigPath);
    document.rigPath = rigPath;
    document.savedSnapshot = document.sourceScene->SaveToString();
    document.dirty = false;
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
        manager->clips = document.sourceManager->clips;
        manager->rigPaths = document.sourceManager->rigPaths;
        if (manager->clip.empty() && !manager->clips.empty())
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
        std::filesystem::path(document.path).filename().string();
    if (document.view)
        document.view->SetTitle("Animation: " + filename + " / " +
            document.rigName +
            (document.dirty ? " *" : "") +
            "###AnimationDocument:" + document.identity);
    if (document.timeline)
        document.timeline->SetTitle("Timeline: " + filename +
            "###AnimationTimeline:" + document.identity);
}

void EditorState::ScanAnimationRigAssets(AnimationDocument& document)
{
    document.rigAssets.clear();
    std::unordered_set<std::string> seen;
    const std::filesystem::path assets =
        m_projectSettings.assetsDirectory.empty()
            ? std::filesystem::path("Assets")
            : std::filesystem::path(m_projectSettings.assetsDirectory);
    std::error_code error;
    if (!std::filesystem::is_directory(assets, error))
    {
        document.error = "Assets directory was not found.";
        return;
    }
    for (std::filesystem::recursive_directory_iterator it(assets,
            std::filesystem::directory_options::skip_permission_denied,
            error), end;
        it != end && !error; it.increment(error))
    {
        if (!it->is_regular_file(error)) continue;
        const std::string extension =
            LowerText(it->path().extension().string());
        if (extension == ".rig")
        {
            auto rig = Engine::Model::RigAsset::Load(it->path().string());
            if (!rig || rig->prefabPath.empty()) continue;
            Engine::Scene::Scene candidate;
            if (!Engine::Serialization::SceneSerializer::InstantiatePrefab(
                    candidate, rig->prefabPath, nullptr)) continue;
            auto* skeleton = RigAt(candidate, rig->rigIndex);
            if (!skeleton || !RigMatchesSkeleton(*rig, *skeleton)) continue;
            const std::string identity = RigIdentity(rig->prefabPath,
                rig->rigIndex);
            if (!seen.insert(identity).second) continue;
            document.rigAssets.push_back({ rig->prefabPath,
                it->path().filename().string() + " / " +
                    skeleton->Owner->name + " (" +
                    std::to_string(rig->meshNames.size()) +
                    " bound meshes)", rig->rigIndex });
            continue;
        }
        if (extension != ".prefab") continue;
        Engine::Scene::Scene candidate;
        const std::string path = it->path().string();
        if (!Engine::Serialization::SceneSerializer::InstantiatePrefab(
                candidate, path, nullptr)) continue;
        for (size_t rig = 0;; ++rig)
        {
            auto* skeleton = RigAt(candidate, rig);
            if (!skeleton) break;
            if (!skeleton->ResolveModel()) continue;
            auto meshes = BoundMeshNames(candidate, skeleton);
            std::string label = it->path().filename().string() + " / " +
                skeleton->Owner->name + " (" +
                std::to_string(meshes.size()) + " bound mesh" +
                (meshes.size() == 1 ? "" : "es") + ")";
            if (!skeleton->rigPath.empty())
                label += " / " + std::filesystem::path(
                    skeleton->rigPath).filename().string();
            for (const auto& mesh : meshes) label += " / " + mesh;
            if (seen.insert(RigIdentity(path, rig)).second)
                document.rigAssets.push_back({ path, std::move(label), rig });
        }
    }
    std::sort(document.rigAssets.begin(), document.rigAssets.end(),
        [](const auto& left, const auto& right)
        { return left.label < right.label; });
    document.error.clear();
}

void EditorState::DrawAnimationTimeline(IEditorUi& ui,
    AnimationDocument& document)
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
    ui.InputText("Search rig assets", document.rigSearch,
        sizeof(document.rigSearch));
    ui.SameLine();
    if (ui.Button("Scan Assets")) ScanAnimationRigAssets(document);
    const std::string query = LowerText(document.rigSearch);
    for (const auto& asset : document.rigAssets)
    {
        if (!query.empty() && LowerText(asset.label).find(query) ==
                std::string::npos &&
            LowerText(asset.path).find(query) == std::string::npos)
            continue;
        ui.PushId(&asset);
        if (ui.Selectable(asset.label.c_str(),
                AnimationIdentity(asset.path) ==
                    AnimationIdentity(document.path) &&
                asset.rigIndex == document.rigIndex))
            QueueAnimationDocumentOpen(asset.path, asset.rigIndex);
        ui.PopId();
    }
    ui.Separator();
    auto& clips = document.sourceManager->clips;
    if (ui.FrameRate() > 1.f && document.playing && document.clipIndex >= 0 &&
        document.clipIndex < static_cast<int>(clips.size()))
    {
        document.playheadSeconds += std::min(1.f / ui.FrameRate(), .1f);
        const float duration = std::max(clips[document.clipIndex].duration,
            1.f / document.framesPerSecond);
        if (document.playheadSeconds > duration)
            document.playheadSeconds = 0.f;
        document.frame = static_cast<int>(std::round(
            document.playheadSeconds * document.framesPerSecond));
        SampleAnimationDocument(document);
    }
    ui.InputText("New clip name", document.newClipName,
        sizeof(document.newClipName));
    ui.SameLine();
    if (ui.Button("Create Clip"))
    {
        std::string name = document.newClipName;
        if (name.empty()) name = "Animation " +
            std::to_string(clips.size() + 1);
        const bool duplicate = std::any_of(clips.begin(), clips.end(),
            [&](const auto& clip) { return clip.clipName == name; });
        if (duplicate) document.error = "Clip names must be unique.";
        else
        {
            auto before = clips;
            clips.emplace_back();
            clips.back().clipName = name;
            clips.back().duration = 2.f;
            document.clipIndex = static_cast<int>(clips.size()) - 1;
            document.frame = 0;
            document.timelineStartFrame = 0;
            document.playheadSeconds = 0.f;
            document.newClipName[0] = '\0';
            document.error.clear();
            RecordAnimationEdit(document, std::move(before));
        }
    }
    if (clips.empty())
    {
        ui.DisabledLabel("Create a clip to begin posing and keyframing bones.");
        if (!document.error.empty()) ui.ColoredLabel(document.error.c_str(),
            { 1.f, .5f, .35f, 1.f });
        return;
    }
    std::vector<const char*> names;
    names.reserve(clips.size());
    for (const auto& clip : clips) names.push_back(clip.clipName.c_str());
    if (document.clipIndex < 0 ||
        document.clipIndex >= static_cast<int>(clips.size()))
        document.clipIndex = 0;
    if (ui.Combo("Clip", &document.clipIndex, names.data(),
            static_cast<int>(names.size())))
    {
        document.playing = false;
        document.frame = 0;
        document.timelineStartFrame = 0;
        document.playheadSeconds = 0.f;
        SampleAnimationDocument(document);
    }
    auto& clip = clips[document.clipIndex];
    float duration = clip.duration;
    if (ui.DragFloat("Duration (seconds)", &duration, .01f, 0.f, 3600.f))
    {
        auto before = clips;
        clip.duration = std::max(duration, LastKeyTime(clip));
        RecordAnimationEdit(document, std::move(before));
    }
    const int maxFrame = std::max(1, static_cast<int>(
        std::ceil(clip.duration * document.framesPerSecond)));
    document.frame = std::clamp(document.frame, 0, maxFrame);
    if (ui.SliderInt("Frame", &document.frame, 0, maxFrame))
    {
        document.playing = false;
        document.playheadSeconds = static_cast<float>(document.frame) /
            document.framesPerSecond;
        document.timelineStartFrame = std::max(0, document.frame - 3);
        SampleAnimationDocument(document);
    }
    const std::string timeLabel = std::to_string(document.frame) + " / " +
        std::to_string(document.framesPerSecond) + " fps";
    ui.ValueLabel("Time", timeLabel.c_str());
    if (ui.Button(document.playing ? "Pause" : "Play"))
    {
        document.playing = !document.playing;
        document.playheadSeconds = static_cast<float>(document.frame) /
            document.framesPerSecond;
    }
    ui.SameLine();
    ui.Checkbox("Auto Key", &document.autoKey);
    ui.SameLine();
    if (ui.Button("Save Clip + Rig")) SaveAnimationDocument(document);
    if (!document.error.empty()) ui.ColoredLabel(document.error.c_str(),
        { 1.f, .5f, .35f, 1.f });

    const auto& joints = document.poseSkeleton->ResolveJoints();
    if (auto* selected = document.poseScene->GetSelectedObject())
        for (size_t index = 0; index < joints.size(); ++index)
            if (joints[index] == selected)
            { document.selectedBone = static_cast<int>(index); break; }
    const int visibleFrames = std::clamp(static_cast<int>(
        (ui.AvailableContentWidth() - 180.f) / 55.f), 4, 16);
    if (document.playing && (document.frame < document.timelineStartFrame ||
        document.frame >= document.timelineStartFrame + visibleFrames))
        document.timelineStartFrame = std::max(0,
            document.frame - visibleFrames / 2);
    if (ui.Button("< Frames")) document.timelineStartFrame =
        std::max(0, document.timelineStartFrame - visibleFrames);
    ui.SameLine();
    if (ui.Button("Frames >")) document.timelineStartFrame =
        std::min(maxFrame, document.timelineStartFrame + visibleFrames);
    if (ui.BeginTable("Animation Timeline", visibleFrames + 1))
    {
        ui.TableSetupColumn("Bone / node");
        for (int column = 0; column < visibleFrames; ++column)
        {
            const std::string heading = std::to_string(
                document.timelineStartFrame + column);
            ui.TableSetupCompactColumn(heading.c_str());
        }
        ui.TableHeadersRow();
        for (size_t boneIndex = 0; boneIndex < joints.size(); ++boneIndex)
        {
            auto* joint = joints[boneIndex];
            if (!joint) continue;
            auto* bone = joint->GetComponent<Engine::Components::AnimationBone>();
            if (!bone) continue;
            ui.PushId(joint);
            ui.TableNextRow();
            ui.TableNextColumn();
            if (ui.Selectable(joint->name.c_str(),
                    document.selectedBone == static_cast<int>(boneIndex)))
            {
                document.selectedBone = static_cast<int>(boneIndex);
                document.poseScene->SetSelectedObject(joint);
                if (m_primaryHierarchy)
                    m_primaryHierarchy->SetSelectedObject(joint);
                if (m_primaryProperties)
                    m_primaryProperties->SetSelectedObject(joint);
            }
            for (int column = 0; column < visibleFrames; ++column)
            {
                ui.TableNextColumn();
                const int frame = document.timelineStartFrame + column;
                if (frame > maxFrame) continue;
                const float time = static_cast<float>(frame) /
                    document.framesPerSecond;
                const bool keyed = AnimationClipEditing::IsKeyed(
                    clip, bone->nodeIndex, time,
                    .5f / document.framesPerSecond);
                const std::string label = (keyed ? "K##" : ".##") +
                    std::to_string(frame);
                if (ui.Button(label.c_str(), 24.f))
                {
                    document.selectedBone = static_cast<int>(boneIndex);
                    document.poseScene->SetSelectedObject(joint);
                    document.frame = frame;
                    document.playing = false;
                    document.playheadSeconds = time;
                    SampleAnimationDocument(document);
                }
            }
            ui.PopId();
        }
        ui.EndTable();
    }
    if (document.selectedBone < 0 ||
        document.selectedBone >= static_cast<int>(joints.size()) ||
        !joints[document.selectedBone]) return;
    auto* joint = joints[document.selectedBone];
    auto* bone = joint->GetComponent<Engine::Components::AnimationBone>();
    if (!bone) return;
    ui.Separator();
    const std::string boneInfo = joint->name + " (node " +
        std::to_string(bone->nodeIndex) + ")";
    ui.ValueLabel("Selected bone", boneInfo.c_str());
    auto& transform = joint->transform;
    const glm::vec3 oldPosition = transform.position;
    const glm::vec3 oldRotation = transform.rotation;
    const glm::vec3 oldScale = transform.scale;
    float angles[]{ glm::degrees(oldRotation.x),
        glm::degrees(oldRotation.y), glm::degrees(oldRotation.z) };
    const bool positionChanged = ui.DragFloat3("Pose position",
        &transform.position.x, .01f);
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
        transform.NotifyEditorTransformChanged(channels);
        document.poseControlEditPending = true;
        if (m_renderer) m_renderer->MarkDirty();
    }
    if (document.poseControlEditPending && !ui.IsAnyItemActive())
    {
        document.poseControlEditPending = false;
        if (document.autoKey) AddAnimationKey(document);
    }
    if (ui.Button("Add / Replace Keyframe")) AddAnimationKey(document);
    ui.SameLine();
    if (ui.Button("Delete Keyframe"))
    {
        auto before = clips;
        const float frameTime = static_cast<float>(document.frame) /
            document.framesPerSecond;
        const auto existing = AnimationClipEditing::NearestKeyTime(clip,
            bone->nodeIndex, frameTime, .5f / document.framesPerSecond);
        if (existing && AnimationClipEditing::DeleteBoneKey(clip,
                bone->nodeIndex, *existing))
            RecordAnimationEdit(document, std::move(before));
    }
}

void EditorState::HandleAnimationDocumentClosures()
{
    for (auto it = m_animationDocuments.begin(); it != m_animationDocuments.end();)
    {
        AnimationDocument* document = it->get();
        if (document->view->IsOpen() && document->timeline->IsOpen())
        { ++it; continue; }
        if (document->dirty)
        {
            document->view->SetOpen(true);
            document->timeline->SetOpen(true);
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Warning,
                "Save the animation clip before closing its editor.");
            ++it;
            continue;
        }
        if (m_activeAnimationDocument == document)
            SetActiveAnimationDocument(nullptr);
        for (IEditorPanel* closing : { static_cast<IEditorPanel*>(document->view),
            static_cast<IEditorPanel*>(document->timeline) })
            for (auto panel = m_panels.begin(); panel != m_panels.end(); ++panel)
                if (panel->get() == closing)
                {
                    if (auto* view = dynamic_cast<View*>(closing))
                        m_viewFactory->FreeSrvSlot(view->GetSrvSlotIndex());
                    m_viewFactory->NotifyPanelRemoved(closing);
                    m_panels.erase(panel);
                    break;
                }
        it = m_animationDocuments.erase(it);
    }
}
}
