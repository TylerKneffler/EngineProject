#include "pch.h"
#include "EditorState.h"
#include "Core/AnimationClipEditing.h"
#include "Core/AnimationClipMapping.h"
#include "Core/AnimationRigSelection.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/Views/AnimationView.h"
#include "Core/View/Views/GameView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Engine/Editor/UI/EditorIcons.h"
#include "Core/SkeletonBindPose.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/ComponentReference.h"
#include "Core/Model/RigAsset.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Object.h"
#include "Core/Prefab/PrefabAsset.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <optional>
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
    Engine::Components::AnimationManager* byPath = nullptr;
    size_t count = 0;
    size_t onRigCount = 0;
    size_t pathCount = 0;
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
                    if (!skeleton->rigPath.empty() &&
                        std::find(manager->rigPaths.begin(),
                            manager->rigPaths.end(), skeleton->rigPath) !=
                            manager->rigPaths.end())
                    { byPath = manager; ++pathCount; }
                }
    if (pathCount == 1) return byPath;
    if (pathCount > 1)
    {
        if (ambiguous) *ambiguous = true;
        return nullptr;
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
{
    return AnimationIdentity(Engine::Components::Mesh::ResolveFilePath(path)) +
        "#rig=" + std::to_string(rigIndex);
}

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

ImVec4 RigTimelineColor(size_t rigIndex)
{
    static const ImVec4 colors[] = {
        { .36f, .78f, 1.f, 1.f },
        { 1.f, .70f, .30f, 1.f },
        { .76f, .58f, 1.f, 1.f },
        { .48f, .84f, .58f, 1.f },
        { 1.f, .53f, .73f, 1.f },
        { .94f, .85f, .40f, 1.f },
        { .36f, .85f, .81f, 1.f },
        { 1.f, .53f, .43f, 1.f },
    };
    constexpr size_t colorCount = sizeof(colors) / sizeof(colors[0]);
    if (rigIndex < colorCount) return colors[rigIndex];
    const float hue = std::fmod(.17f +
        static_cast<float>(rigIndex - colorCount) * .6180339f, 1.f);
    ImVec4 generated{};
    ImGui::ColorConvertHSVtoRGB(hue, .62f, .92f,
        generated.x, generated.y, generated.z);
    generated.w = 1.f;
    return generated;
}

ImVec4 RigTimelineTint(ImVec4 color, float alpha)
{
    color.x *= .42f;
    color.y *= .42f;
    color.z *= .42f;
    color.w = alpha;
    return color;
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
    Engine::Components::Skeleton* rig =
        AnimationRigSelection::ExplicitRig(selected);
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
            if (pathMatches != 1)
            {
                if (matches.empty()) return {};
                return { matches.front()->Owner->GetPrefabInstanceRoot() &&
                    matches.front()->Owner->GetPrefabInstanceRoot()->Prefab
                    ? matches.front()->Owner->GetPrefabInstanceRoot()->Prefab->GetPath()
                    : m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab
                        ? (m_activeSceneAssetDocument->stageDataPath.empty()
                            ? m_activeSceneAssetDocument->path
                            : m_activeSceneAssetDocument->stageDataPath)
                        : m_activePrefabPath,
                    static_cast<size_t>(-1) };
            }
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
    if (path.empty())
    {
        QueueAnimationDocumentOpen("", 0);
        return;
    }
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

bool EditorState::CanCreateAnimationClip() const
{
    return m_activeAnimationDocument &&
        m_activeAnimationDocument->sourceManager;
}

bool EditorState::CreateAnimationClip(const std::string& requestedName)
{
    if (!CanCreateAnimationClip()) return false;
    auto& document = *m_activeAnimationDocument;
    auto& clips = document.sourceManager->clips;
    std::string name = requestedName;
    if (name.empty())
    {
        for (size_t suffix = clips.size() + 1;; ++suffix)
        {
            name = "Animation " + std::to_string(suffix);
            if (std::none_of(clips.begin(), clips.end(),
                    [&](const auto& clip) { return clip.clipName == name; }))
                break;
        }
    }
    if (std::any_of(clips.begin(), clips.end(),
            [&](const auto& clip) { return clip.clipName == name; }))
    {
        document.error = "Clip names must be unique.";
        return false;
    }
    auto before = clips;
    clips.emplace_back();
    clips.back().clipName = name;
    clips.back().duration = 2.f;
    document.clipIndex = static_cast<int>(clips.size()) - 1;
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex)
        { track.clipIndex = document.clipIndex; break; }
    document.playheadSeconds = 0.f;
    document.repairIssueChannel = -1;
    document.error.clear();
    RecordAnimationEdit(document, std::move(before));
    return true;
}

void EditorState::QueueAnimationDocumentOpen(const std::string& path,
    size_t rigIndex)
{
    const std::string identity = path.empty()
        ? "__EmptyAnimationEditor__" : AnimationIdentity(path);
    for (const auto& document : m_animationDocuments)
        if (document && document->identity == identity)
        {
            document->view->SetOpen(true);
            if (rigIndex != static_cast<size_t>(-1))
                FocusAnimationRig(*document, rigIndex);
            if (auto requested = m_pendingAnimationClipSelections.find(identity);
                requested != m_pendingAnimationClipSelections.end())
            {
                SelectAnimationDocumentClip(*document, requested->second,
                    rigIndex);
                m_pendingAnimationClipSelections.erase(requested);
            }
            SetActiveAnimationDocument(document.get());
            return;
        }
    for (const auto& pending : m_pendingAnimationDocuments)
        if ((pending.first.empty() ? "__EmptyAnimationEditor__" :
                AnimationIdentity(pending.first)) == identity) return;
    m_pendingAnimationDocuments.emplace_back(path, rigIndex);
}

void EditorState::QueueAnimationAssetOpen(const std::string& path)
{
    const auto asset = Engine::Model::AnimationClipAsset::Load(path);
    if (!asset)
    {
        if (m_primaryConsole) m_primaryConsole->AddLog(ConsoleView::Level::Error,
            "Could not read animation asset: " + path);
        return;
    }
    std::string prefab = asset->previewPrefab;
    size_t rigIndex = asset->previewRig;
    if (prefab.empty())
    {
        const auto assets = m_projectSettings.assetsDirectory.empty()
            ? std::filesystem::path("Assets")
            : std::filesystem::path(m_projectSettings.assetsDirectory);
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(assets,
                std::filesystem::directory_options::skip_permission_denied,
                error), end;
            it != end && !error; it.increment(error))
            if (it->is_regular_file(error) &&
                LowerText(it->path().extension().string()) == ".rig")
                if (auto rig = Engine::Model::RigAsset::Load(
                        it->path().string()))
                    if (std::any_of(rig->animationPaths.begin(),
                            rig->animationPaths.end(), [&](const auto& item)
                            { return AnimationIdentity(item) ==
                                AnimationIdentity(path); }))
                    {
                        prefab = rig->prefabPath;
                        rigIndex = rig->rigIndex;
                        break;
                    }
    }
    if (prefab.empty())
    {
        if (m_primaryConsole) m_primaryConsole->AddLog(ConsoleView::Level::Error,
            "Assign this animation to a rig before opening it: " + path);
        return;
    }
    m_pendingAnimationClipSelections[AnimationIdentity(prefab)] =
        asset->clip.clipName;
    QueueAnimationDocumentOpen(prefab, rigIndex);
}

void EditorState::SelectAnimationDocumentClip(AnimationDocument& document,
    const std::string& clipName, size_t rigIndex)
{
    for (auto& track : document.tracks)
        if (track.index == rigIndex)
            for (size_t index = 0; index < track.sourceManager->clips.size(); ++index)
                if (track.sourceManager->clips[index].clipName == clipName)
                {
                    track.clipIndex = static_cast<int>(index);
                    track.stackedClips.clear();
                    if (document.rigIndex != rigIndex)
                        FocusAnimationRig(document, rigIndex);
                    document.clipIndex = track.clipIndex;
                    document.playheadSeconds = 0.f;
                    document.selectedKeys.clear();
                    SampleAnimationDocument(document);
                    return;
                }
    document.error = "Animation is not referenced by this rig: " + clipName;
}

void EditorState::ProcessPendingAnimationDocumentOpens()
{
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
            ScanAnimationRigAssets(*document);
            auto view = m_viewFactory->CreateAnimationView(
                document->poseScene.get(), "Animation Editor");
            if (!view) continue;
            AnimationDocument* raw = document.get();
            raw->view = view.get();
            view->SetEditMode(SceneEditMode::Skeleton);
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
            view->OnFocused = [this, raw]()
            { SetActiveAnimationDocument(raw); };
            view->OnDrawBottomPanel = [this, raw](IEditorUi& ui)
            { DrawAnimationTimeline(ui, *raw); };
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
        auto* sourceSkeleton = RigAt(*document->sourceScene, initialRig);
        if (!sourceSkeleton || !sourceSkeleton->ResolveModel())
        {
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Error,
                "Animation Editor needs a prefab with a resolved skeleton and model.");
            continue;
        }
        ScanAnimationRigAssets(*document);
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
        document->sourceManager->RefreshRigClips();
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
        document->poseSkeleton = RigAt(*document->poseScene, initialRig);
        document->poseManager = ManagerForRig(*document->poseScene,
            document->poseSkeleton);
        if (!document->poseSkeleton || !document->poseManager)
            continue;
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
            track.path = sourceRig->rigPath;
            track.boundMeshes = BoundMeshNames(*document->sourceScene,
                sourceRig);
            track.sourceSkeleton = sourceRig;
            track.poseSkeleton = poseRig;
            track.sourceManager = sourceManager;
            track.poseManager = poseManager;
            track.clipIndex = sourceManager->clips.empty() ? -1 : 0;
            for (size_t clip = 0; clip < sourceManager->clips.size(); ++clip)
                if (sourceManager->clips[clip].clipName == sourceManager->clip)
                {
                    track.clipIndex = static_cast<int>(clip);
                    break;
                }
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
        AnimationDocument* raw = document.get();
        raw->view = view.get();
        view->SetDocumentPath(path);
        view->SetEditMode(SceneEditMode::Skeleton);
        view->AllowObjectCreation = false;
        view->AllowAssetDrops = false;
        view->CanSelectObject = [raw](const Engine::Core::Object* object)
        {
            if (!object) return false;
            for (const auto& track : raw->tracks)
                if (track.selected)
                    for (auto* joint : track.poseSkeleton->ResolveJoints())
                        if (joint == object) return true;
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
        view->OnDrawBottomPanel = [this, raw](IEditorUi& ui)
        { DrawAnimationTimeline(ui, *raw); };
        RefreshAnimationDocumentTitle(*raw);
        SampleAnimationDocument(*raw);
        view->RequestFocusOnNextDraw();
        m_panels.push_back(std::move(view));
        m_animationDocuments.push_back(std::move(document));
        SetActiveAnimationDocument(raw);
        for (const auto& open : m_animationDocuments)
            if (open && open->path.empty() && open->view)
                open->view->SetOpen(false);
    }
}

void EditorState::FocusAnimationRig(AnimationDocument& document, size_t index)
{
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex)
        {
            track.selectedBone = document.selectedBone;
            break;
        }
    for (auto& track : document.tracks)
        if (track.index == index)
        {
            const bool wasSelected = track.selected;
            track.selected = true;
            document.rigIndex = index;
            document.rigName = track.name;
            document.rigPath = track.path;
            document.boundMeshNames = track.boundMeshes;
            document.sourceManager = track.sourceManager;
            document.poseManager = track.poseManager;
            document.poseSkeleton = track.poseSkeleton;
            document.clipIndex = track.clipIndex;
            document.selectedBone = track.selectedBone;
            document.repairIssueChannel = -1;
            RefreshAnimationDocumentTitle(document);
            if (m_activeAnimationDocument == &document)
            {
                m_activeAnimationDocument = nullptr;
                SetActiveAnimationDocument(&document);
            }
            if (!wasSelected) SampleAnimationDocument(document);
            return;
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
    if (m_activeAnimationDocument == document)
    {
        m_gameViewFocused = false;
        return;
    }
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
            for (const auto& track : document->tracks)
                if (track.selected)
                    for (auto* joint : track.poseSkeleton->ResolveJoints())
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
        m_primaryProperties->SetOpen(true);
        m_primaryProperties->Init(document->poseScene.get());
        m_primaryProperties->SetAllowComponentStructureEdits(false);
        m_primaryProperties->SetSelectedAsset("");
        m_primaryProperties->SetSelectedObject(
            document->poseScene->GetSelectedObject());
        m_primaryProperties->OnDrawContext = [this, document](IEditorUi& ui)
        {
            DrawAnimationProperties(ui, *document);
            return !document->sourceManager ||
                m_activeAnimationDocument != document ||
                !m_primaryProperties->GetSelectedObject();
        };
    }
    if (m_renderer) m_renderer->MarkDirty();
}

bool EditorState::IsAnimationEditorFocused() const
{
    return m_activeAnimationDocument && !m_gameViewFocused;
}

bool EditorState::IsAnimationPlaying() const
{
    return IsAnimationEditorFocused() && m_activeAnimationDocument->playing;
}

bool EditorState::CanToggleAnimationPlayback() const
{
    if (!IsAnimationEditorFocused()) return false;
    const AnimationDocument& document = *m_activeAnimationDocument;
    return document.sourceManager && document.clipIndex >= 0 &&
        document.clipIndex < static_cast<int>(document.sourceManager->clips.size());
}

bool EditorState::CanStopAnimationPlayback() const
{
    return IsAnimationEditorFocused() &&
        (m_activeAnimationDocument->playing ||
            m_activeAnimationDocument->playbackStarted ||
            m_activeAnimationDocument->playheadSeconds > 0.f);
}

void EditorState::ToggleAnimationPlayback()
{
    if (!CanToggleAnimationPlayback()) return;
    AnimationDocument& document = *m_activeAnimationDocument;
    document.playing = !document.playing;
    document.playbackStarted = true;
}

void EditorState::StopAnimationPlayback()
{
    if (!CanStopAnimationPlayback()) return;
    AnimationDocument& document = *m_activeAnimationDocument;
    document.playing = false;
    document.playbackStarted = false;
    document.playheadSeconds = 0.f;
    SampleAnimationDocument(document);
}

EditorState::AnimationDocument::CachedClip& EditorState::CachedAnimationClip(
    AnimationDocument& document, AnimationDocument::RigTrack& track,
    int clipIndex)
{
    const AnimationDocument::ClipCacheKey key{
        track.sourceManager, track.poseSkeleton, clipIndex };
    if (auto found = document.clipCache.find(key);
        found != document.clipCache.end()) return found->second;
    auto& source = track.sourceManager->clips[clipIndex];
    auto* model = track.poseSkeleton->ResolveModel();
    const auto mapping = AnimationClipMapping::Inspect(source,
        AnimationClipMapping::RigNodes(*track.poseSkeleton),
        model ? AnimationClipMapping::ModelBones(*model)
            : std::vector<AnimationClipMapping::Bone>{});
    AnimationDocument::CachedClip cached;
    cached.mappingReady = mapping.issues.empty();
    cached.compatible = AnimationClipMapping::CompatibleClip(source, mapping);
    for (const auto& channel : cached.compatible.channels)
    {
        auto& times = cached.keyTimes[channel.nodeIndex];
        times.insert(times.end(), channel.times.begin(), channel.times.end());
    }
    for (auto& [node, times] : cached.keyTimes)
    {
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(),
            [](float a, float b) { return std::abs(a - b) < .0001f; }),
            times.end());
    }
    return document.clipCache.emplace(key, std::move(cached)).first->second;
}

void EditorState::SampleAnimationDocument(AnimationDocument& document)
{
    if (!document.poseManager || !document.poseSkeleton ||
        !document.sourceManager) return;
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex)
        {
            track.selectedBone = document.selectedBone;
            break;
        }
    const bool layeredPreview = document.tracks.size() > 1 ||
        std::any_of(document.tracks.begin(), document.tracks.end(),
            [](const auto& track)
            { return track.weight != 1.f || !track.stackedClips.empty(); });
    if (layeredPreview)
    {
        for (auto& track : document.tracks)
        {
            track.poseManager->playing = false;
            track.poseManager->holdCurrentPoseWhenStopped = true;
        }
        std::unordered_set<Engine::Components::Model*> sampled;
        for (auto& leader : document.tracks)
        {
            if (!leader.selected) continue;
            auto* model = leader.poseSkeleton->ResolveModel();
            if (!model || !sampled.insert(model).second) continue;
            auto* preview = leader.poseManager;
            std::string layout = std::to_string(document.clipRevision);
            for (const auto& track : document.tracks)
                if (track.selected &&
                    track.poseSkeleton->ResolveModel() == model)
                {
                    layout += "/" + std::to_string(track.index) + ":" +
                        std::to_string(track.clipIndex);
                    for (const auto& layer : track.stackedClips)
                        layout += "," + std::to_string(layer.clipIndex);
                }
            const bool rebuild = document.previewLayouts[preview] != layout;
            if (rebuild)
            {
                preview->clips.clear();
                preview->layers.clear();
                Engine::Model::AnimationClip rest;
                rest.clipName = "__AnimationEditorRestPose__";
                rest.duration = 1.f;
                preview->clips.push_back(std::move(rest));
                preview->clip = preview->clips.front().clipName;
                preview->time = 0.f;
            }
            size_t previewLayer = 0;
            for (auto& track : document.tracks)
            {
                if (!track.selected || track.poseSkeleton->ResolveModel() != model)
                    continue;
                for (auto* joint : track.poseSkeleton->ResolveJoints())
                    if (joint) joint->transform.ClearEditorOverride();
                const auto addLayer = [&](int clipIndex, float weight,
                    size_t layerIndex)
                {
                    if (clipIndex < 0 ||
                        clipIndex >= static_cast<int>(
                            track.sourceManager->clips.size())) return;
                    if (rebuild)
                    {
                        auto clip = CachedAnimationClip(document, track,
                            clipIndex).compatible;
                        clip.clipName = "__AnimationEditorRig_" +
                            std::to_string(track.index) + "_Layer_" +
                            std::to_string(layerIndex) + "__" + clip.clipName;
                        preview->clips.push_back(std::move(clip));
                        Engine::Components::AnimationManager::Layer layer;
                        layer.clip = preview->clips.back().clipName;
                        layer.looping = true;
                        layer.nodeMask = track.poseSkeleton->jointNodes;
                        preview->layers.push_back(std::move(layer));
                    }
                    auto& layer = preview->layers[previewLayer++];
                    layer.time = document.playheadSeconds;
                    layer.weight = weight;
                };
                addLayer(track.clipIndex, track.weight, 0);
                for (size_t layerIndex = 0;
                    layerIndex < track.stackedClips.size(); ++layerIndex)
                    addLayer(track.stackedClips[layerIndex].clipIndex,
                        track.stackedClips[layerIndex].weight,
                        layerIndex + 1);
            }
            if (rebuild) document.previewLayouts[preview] = std::move(layout);
            preview->holdCurrentPoseWhenStopped = false;
            preview->Tick(0.f);
        }
        for (const auto& track : document.tracks)
        {
            if (track.selected || sampled.count(track.poseSkeleton->ResolveModel()))
                continue;
            auto* sourceModel = track.sourceSkeleton->ResolveModel();
            auto* poseModel = track.poseSkeleton->ResolveModel();
            if (!sourceModel || !poseModel) continue;
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
    auto* selectedTrack = static_cast<AnimationDocument::RigTrack*>(nullptr);
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex)
        { selectedTrack = &track; break; }
    if (!selectedTrack) return;
    const std::string layout = std::to_string(document.clipRevision) +
        "/single/" + std::to_string(document.rigIndex) + ":" +
        std::to_string(clipIndex);
    if (document.previewLayouts[document.poseManager] != layout)
    {
        document.poseManager->clips.clear();
        document.poseManager->layers.clear();
        document.poseManager->clips.push_back(
            CachedAnimationClip(document, *selectedTrack,
                clipIndex).compatible);
        document.previewLayouts[document.poseManager] = layout;
    }
    document.poseManager->clip = clips[clipIndex].clipName;
    document.poseManager->time = document.playheadSeconds;
    document.poseManager->looping = true;
    document.poseManager->Tick(0.f);
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::RecordAnimationEdit(AnimationDocument& document,
    std::vector<Engine::Model::AnimationClip> before)
{
    ++document.clipRevision;
    document.clipCache.clear();
    document.previewLayouts.clear();
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

bool EditorState::AddAnimationKey(AnimationDocument& document, bool exactTime)
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
    if (!bone && document.selectedBone >= static_cast<int>(
            document.poseSkeleton->jointNodes.size())) return false;
    const unsigned nodeIndex = bone ? bone->nodeIndex :
        document.poseSkeleton->jointNodes[document.selectedBone];
    auto before = document.sourceManager->clips;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    auto* model = document.poseSkeleton->ResolveModel();
    if (!model || !AnimationClipMapping::Inspect(clip,
            AnimationClipMapping::RigNodes(*document.poseSkeleton),
            AnimationClipMapping::ModelBones(*model)).issues.empty())
    {
        document.error = "Repair unresolved bone mappings before keying this clip.";
        return false;
    }
    const auto& transform = joints[document.selectedBone]->transform;
    const float frameTime = document.playheadSeconds;
    const float keyTime = exactTime ? frameTime :
        AnimationClipEditing::NearestKeyTime(clip,
            nodeIndex, frameTime, .02f).value_or(frameTime);
    const bool keyed = AnimationClipEditing::UpsertBonePose(clip,
        nodeIndex, keyTime,
        transform.position, glm::quat(transform.rotation), transform.scale);
    if (keyed)
    {
        const std::string targetPath = AnimationClipMapping::NodePath(
            *model, nodeIndex);
        for (auto& channel : clip.channels)
            if (channel.nodeIndex == nodeIndex &&
                channel.targetPath.empty())
                channel.targetPath = targetPath;
        clip.duration = std::max(clip.duration, keyTime);
        RecordAnimationEdit(document, std::move(before));
    }
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
    document.selectedKeys.clear();
    ++document.clipRevision;
    document.clipCache.clear();
    document.previewLayouts.clear();
    document.dirty = document.sourceScene->SaveToString() !=
        document.savedSnapshot;
    RefreshAnimationDocumentTitle(document);
    SampleAnimationDocument(document);
    return true;
}

bool EditorState::SaveAnimationDocument(AnimationDocument& document)
{
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
    const auto previousRig = Engine::Model::RigAsset::Load(rigPath);
    std::vector<std::string> animationPaths;
    for (size_t index = 0; index < document.sourceManager->clips.size(); ++index)
    {
        auto animation = document.sourceManager->clips[index];
        for (auto& channel : animation.channels)
            if (channel.targetPath.empty())
                channel.targetPath = AnimationClipMapping::NodePath(
                    *latestModel, channel.nodeIndex);
        std::string animationPath;
        if (previousRig)
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
        if (asset.previewPrefab.empty())
        {
            asset.previewPrefab = document.path;
            asset.previewRig = document.rigIndex;
        }
        if (asset.baseSkeleton.empty())
            asset.baseSkeleton = Engine::Model::AnimationClipAsset::
                CaptureBaseSkeleton(*latestModel);
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
    if (std::find(latestManager->rigPaths.begin(),
            latestManager->rigPaths.end(), rigPath) ==
        latestManager->rigPaths.end())
        latestManager->rigPaths.push_back(rigPath);
    latestManager->clips = document.sourceManager->clips;
    latestManager->RefreshRigClips();
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
    document.sourceManager->RefreshRigClips();
    document.rigPath = rigPath;
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex)
        { track.path = rigPath; break; }
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
}

void EditorState::ScanAnimationRigAssets(AnimationDocument& document)
{
    document.rigAssets.clear();
    document.rigAssetsDirty = false;
    std::unordered_set<std::string> seen;
    std::vector<std::filesystem::path> rigPaths;
    struct PreviewCandidate
    {
        std::string path;
        size_t rigIndex = 0;
        Engine::Model::RigAsset rig;
    };
    std::vector<PreviewCandidate> previewCandidates;
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
            rigPaths.push_back(it->path());
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
            previewCandidates.push_back({ path, rig,
                Engine::Model::RigAsset::Capture(candidate, *skeleton) });
            auto meshes = BoundMeshNames(candidate, skeleton);
            std::string label = "[Prefab] " +
                it->path().filename().string() + " / " +
                skeleton->Owner->name + " (" +
                std::to_string(meshes.size()) + " bound mesh" +
                (meshes.size() == 1 ? "" : "es") + ")";
            if (!skeleton->rigPath.empty())
                label += " / " + std::filesystem::path(
                    skeleton->rigPath).filename().string();
            for (const auto& mesh : meshes) label += " / " + mesh;
            if (seen.insert(RigIdentity(path, rig)).second)
                document.rigAssets.push_back({ path, std::move(label), rig,
                    path });
        }
    }
    // A rig asset is a separate resource. Its prefab reference supplies a
    // model for the animation viewport, but does not determine its identity.
    for (const auto& rigPath : rigPaths)
    {
        auto rig = Engine::Model::RigAsset::Load(rigPath.string());
        if (!rig) continue;
        const auto compatible = [&](const PreviewCandidate& candidate)
        {
            if (candidate.rig.skinIndex != rig->skinIndex ||
                candidate.rig.joints.size() != rig->joints.size()) return false;
            for (size_t joint = 0; joint < rig->joints.size(); ++joint)
                if (candidate.rig.joints[joint].nodeIndex !=
                        rig->joints[joint].nodeIndex ||
                    candidate.rig.joints[joint].name !=
                        rig->joints[joint].name) return false;
            return true;
        };
        const PreviewCandidate* preview = nullptr;
        for (const auto& candidate : previewCandidates)
            if (compatible(candidate) &&
                AnimationIdentity(candidate.path) ==
                    AnimationIdentity(document.path))
            { preview = &candidate; break; }
        if (!preview)
            for (const auto& candidate : previewCandidates)
                if (compatible(candidate) && !rig->prefabPath.empty() &&
                    AnimationIdentity(candidate.path) ==
                        AnimationIdentity(rig->prefabPath))
                { preview = &candidate; break; }
        if (!preview)
            for (const auto& candidate : previewCandidates)
                if (compatible(candidate))
                { preview = &candidate; break; }
        const std::string label = "[Rig] " + rigPath.filename().string() +
            " / " + std::to_string(rig->joints.size()) + " joints" +
            (preview ? " / preview: " +
                std::filesystem::path(preview->path).filename().string() :
                " / choose a compatible prefab to preview");
        document.rigAssets.push_back({ preview ? preview->path : "",
            label, preview ? preview->rigIndex : rig->rigIndex,
            rigPath.string() });
    }
    std::sort(document.rigAssets.begin(), document.rigAssets.end(),
        [](const auto& left, const auto& right)
        { return left.label < right.label; });
    document.error.clear();
}

void EditorState::DrawAnimationMappingRepair(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return;
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
    if (!ImGui::CollapsingHeader("Repair bone mappings",
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
    {
        auto before = clips;
        if (AnimationClipMapping::Repair(clip,
                static_cast<size_t>(document.repairIssueChannel),
                rigBones[document.repairBoneIndex]))
        {
            document.repairIssueChannel = -1;
            document.error.clear();
            RecordAnimationEdit(document, std::move(before));
            SampleAnimationDocument(document);
        }
    }
}

void EditorState::DrawAnimationRigToolbar(IEditorUi& ui,
    AnimationDocument& document)
{
    ImGui::SetNextItemWidth(240.f);
    const std::string label = document.sourceManager
        ? "Rig: " + document.rigName : "Choose animation rig";
    if (!ImGui::BeginCombo("##AnimationRigToolbar", label.c_str())) return;
    if (document.sourceManager)
    {
        for (auto& track : document.tracks)
        {
            ImGui::PushID(&track);
            bool selected = track.selected;
            if (ImGui::Checkbox("##Preview", &selected))
            {
                const int count = static_cast<int>(std::count_if(
                    document.tracks.begin(), document.tracks.end(),
                    [](const auto& item) { return item.selected; }));
                if (selected || count > 1)
                {
                    track.selected = selected;
                    if (selected) FocusAnimationRig(document, track.index);
                    else if (track.index == document.rigIndex)
                        for (const auto& other : document.tracks)
                            if (other.selected)
                            { FocusAnimationRig(document, other.index); break; }
                    SampleAnimationDocument(document);
                }
            }
            ImGui::SameLine();
            if (ImGui::Selectable(track.name.c_str(),
                    document.rigIndex == track.index))
                FocusAnimationRig(document, track.index);
            ImGui::PopID();
        }
        ImGui::Separator();
    }
    if (document.rigAssetsDirty)
        ScanAnimationRigAssets(document);
    ui.SearchInput("##RigAssetSearch", document.rigSearch,
        sizeof(document.rigSearch), "Search rig assets");
    ImGui::SameLine();
    if (ui.Button("Refresh rigs")) ScanAnimationRigAssets(document);
    const std::string query = LowerText(document.rigSearch);
    for (const auto& asset : document.rigAssets)
    {
        if (!query.empty() && LowerText(asset.label).find(query) ==
                std::string::npos &&
            LowerText(asset.path).find(query) == std::string::npos &&
            LowerText(asset.assetPath).find(query) == std::string::npos)
            continue;
        ImGui::PushID(&asset);
        const bool rigAsset = LowerText(
            std::filesystem::path(asset.assetPath).extension().string()) == ".rig";
        if (ImGui::Selectable(asset.label.c_str(),
                rigAsset ? AnimationIdentity(asset.assetPath) ==
                    AnimationIdentity(document.rigPath) :
                    RigIdentity(asset.path, asset.rigIndex) ==
                        RigIdentity(document.path, document.rigIndex)))
        {
            if (asset.path.empty())
                document.error = "Choose a prefab with a compatible model to preview this rig.";
            else
                QueueAnimationDocumentOpen(asset.path, asset.rigIndex);
            if (!asset.path.empty() && document.path.empty() && document.view &&
                m_activeAnimationDocument != &document)
                document.view->SetOpen(false);
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

void EditorState::DrawAnimationProperties(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.sourceManager)
    {
        ui.DisabledLabel("Choose a rig in the toolbar to edit its animations.");
        if (!document.error.empty()) ui.ColoredLabel(
            document.error.c_str(), { 1.f, .5f, .35f, 1.f });
        return;
    }
    auto& clips = document.sourceManager->clips;
    if (ImGui::CollapsingHeader("Rig details"))
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
    ui.Separator();
    Engine::Core::Component::DrawAssetPathPicker(ui, &document,
        "animationAssetToAdd", "Animation asset", "animation",
        document.animationAssetToAdd);
    ui.SameLine();
    if (ui.Button("Add to rig"))
    {
        const auto asset = Engine::Model::AnimationClipAsset::Load(
            document.animationAssetToAdd);
        auto rig = Engine::Model::RigAsset::Load(document.rigPath);
        if (!rig)
            document.error = "Save this rig before adding animation assets.";
        else if (!asset)
            document.error = "Choose a valid animation asset.";
        else if (std::find(rig->animationPaths.begin(),
                rig->animationPaths.end(), document.animationAssetToAdd) !=
                rig->animationPaths.end())
            document.error = "Animation is already referenced by this rig.";
        else if (std::any_of(clips.begin(), clips.end(),
                [&](const auto& clip)
                { return clip.clipName == asset->clip.clipName; }))
            document.error = "An animation with this name already exists on the rig.";
        else
        {
            auto* model = document.poseSkeleton->ResolveModel();
            auto mapped = asset->clip;
            bool compatible = model != nullptr;
            if (model)
            {
                const auto nodes = AnimationClipMapping::ModelBones(*model);
                for (auto& channel : mapped.channels)
                {
                    const auto found = std::find_if(nodes.begin(), nodes.end(),
                        [&](const auto& node)
                        { return node.path == channel.targetPath; });
                    if (channel.targetPath.empty() || found == nodes.end())
                    { compatible = false; break; }
                    channel.nodeIndex = found->nodeIndex;
                }
            }
            if (!compatible)
                document.error = "Animation channels do not match this rig.";
            else
            {
                rig->animationPaths.push_back(document.animationAssetToAdd);
                if (!rig->Save(document.rigPath))
                    document.error = "Could not save rig animation reference.";
                else
                {
                    document.sourceManager->RefreshRigClips();
                    for (auto& track : document.tracks)
                        if (track.index == document.rigIndex)
                            for (size_t index = 0;
                                index < track.sourceManager->clips.size(); ++index)
                                if (track.sourceManager->clips[index].clipName ==
                                    mapped.clipName)
                                { track.clipIndex = static_cast<int>(index); break; }
                    document.clipIndex = [&]()
                    {
                        for (const auto& track : document.tracks)
                            if (track.index == document.rigIndex)
                                return track.clipIndex;
                        return -1;
                    }();
                    document.animationAssetToAdd.clear();
                    document.error.clear();
                    ++document.clipRevision;
                    document.clipCache.clear();
                    SampleAnimationDocument(document);
                }
            }
        }
    }
    if (clips.empty())
    {
        ui.DisabledLabel("Create a clip or add an animation asset to this rig.");
        if (!document.error.empty()) ui.ColoredLabel(document.error.c_str(),
            { 1.f, .5f, .35f, 1.f });
        return;
    }
    DrawAnimationMappingRepair(ui, document);
    const std::string saveLabel = std::string(Icons::Save) +
        " Save Selected Rigs";
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
}

void EditorState::DrawAnimationTimeline(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.sourceManager)
    {
        ui.DisabledLabel("Choose a rig in the toolbar to open its timeline.");
        return;
    }
    auto& clips = document.sourceManager->clips;
    if (ui.FrameRate() > 1.f && document.playing &&
        document.clipIndex >= 0 &&
        document.clipIndex < static_cast<int>(clips.size()))
    {
        document.playheadSeconds += std::min(1.f / ui.FrameRate(), .1f);
        float duration = .01f;
        bool anyLooping = false;
        for (const auto& track : document.tracks)
            if (track.selected)
            {
                const auto includeClip = [&](int index)
                {
                    if (index >= 0 && index < static_cast<int>(
                            track.sourceManager->clips.size()))
                    {
                        const auto& clip = track.sourceManager->clips[index];
                        duration = std::max(duration, clip.duration);
                        anyLooping |= clip.looping;
                    }
                };
                includeClip(track.clipIndex);
                for (const auto& layer : track.stackedClips)
                    includeClip(layer.clipIndex);
            }
        if (document.playheadSeconds >= duration)
        {
            if (anyLooping)
                document.playheadSeconds = std::fmod(
                    document.playheadSeconds, duration);
            else
            {
                document.playheadSeconds = duration;
                document.playing = false;
            }
        }
        SampleAnimationDocument(document);
    }
    if (ui.Button("Key Selected Bone"))
    {
        document.playing = false;
        AddAnimationKey(document, true);
    }
    ui.SameLine();
    ui.Checkbox("Auto Key", &document.autoKey);
    ui.SameLine();
    ImGui::Text("%.3f s", document.playheadSeconds);
    ImGui::SetNextItemWidth(150.f);
    ImGui::SliderFloat("Zoom", &document.timelinePixelsPerSecond,
        35.f, 320.f, "%.0f px/s");
    for (auto& track : document.tracks)
    {
        if (!track.selected || !track.sourceManager) continue;
        const auto& available = track.sourceManager->clips;
        if (available.empty()) continue;
        ImGui::PushID(&track);
        if (document.tracks.size() > 1)
            ImGui::TextUnformatted(track.name.c_str());
        const char* baseName = track.clipIndex >= 0 &&
            track.clipIndex < static_cast<int>(available.size())
                ? available[track.clipIndex].clipName.c_str() : "Choose animation";
        ImGui::SetNextItemWidth(220.f);
        if (ImGui::BeginCombo("Base animation", baseName))
        {
            for (int index = 0; index < static_cast<int>(available.size()); ++index)
                if (ImGui::Selectable(available[index].clipName.c_str(),
                        index == track.clipIndex))
                {
                    track.clipIndex = index;
                    track.stackedClips.erase(std::remove_if(
                        track.stackedClips.begin(), track.stackedClips.end(),
                        [index](const auto& layer)
                        { return layer.clipIndex == index; }),
                        track.stackedClips.end());
                    if (document.rigIndex != track.index)
                        FocusAnimationRig(document, track.index);
                    document.clipIndex = index;
                    document.playheadSeconds = 0.f;
                    document.repairIssueChannel = -1;
                    document.selectedKeys.clear();
                    SampleAnimationDocument(document);
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::BeginCombo("Add stacked animation", "+ Add"))
        {
            bool hasOption = false;
            for (int index = 0; index < static_cast<int>(available.size()); ++index)
            {
                if (index == track.clipIndex || std::any_of(
                        track.stackedClips.begin(), track.stackedClips.end(),
                        [index](const auto& layer)
                        { return layer.clipIndex == index; })) continue;
                hasOption = true;
                if (ImGui::Selectable(available[index].clipName.c_str()))
                {
                    track.stackedClips.push_back({ index, 1.f });
                    SampleAnimationDocument(document);
                }
            }
            if (!hasOption) ImGui::TextDisabled("No other animations");
            ImGui::EndCombo();
        }
        for (size_t layer = 0; layer < track.stackedClips.size(); ++layer)
        {
            const int index = track.stackedClips[layer].clipIndex;
            if (index < 0 || index >= static_cast<int>(available.size()))
                continue;
            ImGui::PushID(static_cast<int>(layer));
            ImGui::Text("Stacked: %s", available[index].clipName.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                track.stackedClips.erase(track.stackedClips.begin() + layer);
                document.selectedKeys.clear();
                if (document.rigIndex == track.index)
                    document.clipIndex = track.clipIndex;
                SampleAnimationDocument(document);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("Double-click: key | Drag: move | Right-drag: select | Delete: remove");
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
            const auto includeClip = [&](int index)
            {
                if (index >= 0 && index < static_cast<int>(available.size()))
                    timelineDuration = std::max(timelineDuration,
                        available[index].duration);
            };
            includeClip(track.clipIndex);
            for (const auto& layer : track.stackedClips)
                includeClip(layer.clipIndex);
        }
    DrawAnimationGraph(document, timelineDuration);
}

void EditorState::DrawAnimationGraph(AnimationDocument& document,
    float duration)
{
    struct Row
    {
        enum class Kind { Header, Bone } kind = Kind::Header;
        AnimationDocument::RigTrack* track = nullptr;
        Engine::Core::Object* joint = nullptr;
        Engine::Model::AnimationClip* clip = nullptr;
        const std::unordered_map<unsigned, std::vector<float>>* keyTimes = nullptr;
        bool mappingReady = true;
        size_t boneIndex = 0;
        unsigned nodeIndex = 0;
        bool* expanded = nullptr;
        int clipIndex = -1;
        float height = 25.f;
        float offset = 0.f;
    };
    std::vector<Row> rows;
    for (auto& track : document.tracks)
    {
        if (!track.selected) continue;
        auto& clips = track.sourceManager->clips;
        const auto appendClip = [&](int clipIndex, bool& expanded)
        {
            Engine::Model::AnimationClip* clip = clipIndex >= 0 &&
                clipIndex < static_cast<int>(clips.size())
                    ? &clips[clipIndex] : nullptr;
            const std::unordered_map<unsigned, std::vector<float>>* keyTimes =
                nullptr;
            bool mappingReady = true;
            if (clip)
            {
                auto& cached = CachedAnimationClip(document, track, clipIndex);
                mappingReady = cached.mappingReady;
                keyTimes = &cached.keyTimes;
            }
            rows.push_back({ Row::Kind::Header, &track, nullptr, clip,
                keyTimes, mappingReady, 0, 0, &expanded, clipIndex, 28.f });
            if (!expanded) return;
            const auto& joints = track.poseSkeleton->ResolveJoints();
            for (size_t boneIndex = 0; boneIndex < joints.size(); ++boneIndex)
            {
                auto* joint = joints[boneIndex];
                if (!joint) continue;
                auto* bone = joint->GetComponent<
                    Engine::Components::AnimationBone>();
                const unsigned nodeIndex = bone ? bone->nodeIndex :
                    boneIndex < track.poseSkeleton->jointNodes.size()
                        ? track.poseSkeleton->jointNodes[boneIndex] : 0;
                rows.push_back({ Row::Kind::Bone, &track, joint, clip,
                    keyTimes, mappingReady, boneIndex, nodeIndex,
                    nullptr, clipIndex });
            }
        };
        if (track.clipIndex >= 0 &&
            track.clipIndex < static_cast<int>(clips.size()))
        {
            bool& expanded = track.expandedClips.try_emplace(
                track.clipIndex, true).first->second;
            appendClip(track.clipIndex, expanded);
        }
        for (const auto& layer : track.stackedClips)
            if (layer.clipIndex >= 0 &&
                layer.clipIndex < static_cast<int>(clips.size()) &&
                layer.clipIndex != track.clipIndex)
            {
                bool& expanded = track.expandedClips.try_emplace(
                    layer.clipIndex, true).first->second;
                appendClip(layer.clipIndex, expanded);
            }
    }
    float rowTotalHeight = 0.f;
    for (auto& row : rows)
    {
        row.offset = rowTotalHeight;
        rowTotalHeight += row.height;
    }
    constexpr float labelWidth = 292.f;
    constexpr float rulerHeight = 30.f;
    const float pixelsPerSecond = document.timelinePixelsPerSecond;
    const float seconds = std::max({ 5.f, duration + 1.f,
        document.playheadSeconds + 1.f });
    const float canvasWidth = std::max(ImGui::GetContentRegionAvail().x,
        labelWidth + seconds * pixelsPerSecond + 24.f);
    const float canvasHeight = rulerHeight + rowTotalHeight + 4.f;
    const float graphHeight = std::max(1.f,
        ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y);
    if (!ImGui::BeginChild("##AnimationGraph", { 0.f, graphHeight }, true,
            ImGuiWindowFlags_HorizontalScrollbar))
    { ImGui::EndChild(); return; }
    ImGui::Dummy({ canvasWidth, std::max(canvasHeight, graphHeight - 8.f) });
    const ImVec2 origin = ImGui::GetItemRectMin();
    const float timeX = origin.x + labelWidth;
    const float pinnedX = origin.x + ImGui::GetScrollX();
    const float labelRight = pinnedX + labelWidth;
    const float bottomY = origin.y + canvasHeight;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,
        { origin.x + canvasWidth, origin.y + rulerHeight },
        ImGui::GetColorU32(ImGuiCol_FrameBg));
    draw->AddLine({ timeX, origin.y }, { timeX, bottomY },
        ImGui::GetColorU32(ImGuiCol_Border));

    constexpr float steps[] = { .1f, .2f, .5f, 1.f, 2.f, 5.f,
        10.f, 20.f, 50.f, 100.f };
    float step = steps[std::size(steps) - 1];
    for (float candidate : steps)
        if (candidate * pixelsPerSecond >= 72.f)
        { step = candidate; break; }
    const float visibleStart = std::max(0.f,
        (ImGui::GetScrollX() - labelWidth) / pixelsPerSecond);
    const float visibleEnd = std::min(seconds,
        (ImGui::GetScrollX() + ImGui::GetWindowWidth() - labelWidth) /
            pixelsPerSecond + step);
    for (float time = std::ceil(visibleStart / step) * step;
        time <= visibleEnd + .0001f; time += step)
    {
        const float x = timeX + time * pixelsPerSecond;
        draw->AddLine({ x, origin.y + rulerHeight - 9.f },
            { x, bottomY }, IM_COL32(150, 160, 175, 52));
        char label[24];
        std::snprintf(label, sizeof(label),
            step >= 1.f ? "%.0f s" : "%.2f s", time);
        draw->AddText({ x + 4.f, origin.y + 5.f },
            ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    draw->AddRectFilled({ pinnedX, origin.y },
        { labelRight, origin.y + rulerHeight },
        ImGui::GetColorU32(ImGuiCol_FrameBg));
    draw->AddText({ pinnedX + 9.f, origin.y + 5.f },
        ImGui::GetColorU32(ImGuiCol_TextDisabled), "Animation / bone");

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsItemHovered();
    const bool leftClicked = hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool rightClicked = hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const auto timeAt = [&](float x)
    {
        return std::clamp((x - timeX) / pixelsPerSecond, 0.f, seconds);
    };
    const bool onRuler = mouse.y >= origin.y &&
        mouse.y < origin.y + rulerHeight;
    if (hovered && mouse.x >= labelRight && onRuler)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (leftClicked && mouse.x >= labelRight && onRuler)
    {
        document.playheadDragging = true;
        document.playing = false;
    }
    if (document.playheadDragging)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            document.playheadDragging = false;
        else
        {
            const float time = timeAt(mouse.x);
            if (time != document.playheadSeconds)
            {
                document.playheadSeconds = time;
                SampleAnimationDocument(document);
            }
        }
    }

    const float visibleTop = ImGui::GetWindowPos().y;
    const float visibleBottom = visibleTop + ImGui::GetWindowHeight();
    AnimationDocument::RigTrack* removeLayerFrom = nullptr;
    size_t removeLayerIndex = 0;
    AnimationDocument::RigTrack* editedTrack = nullptr;
    std::vector<Engine::Model::AnimationClip> editBefore;
    bool refreshClipCache = false;
    const auto drawClipProperties = [&](Row& row)
    {
        auto& clips = row.track->sourceManager->clips;
        const int clipIndex = row.clipIndex;
        const bool primary = row.track->clipIndex == clipIndex;
        auto stacked = std::find_if(row.track->stackedClips.begin(),
            row.track->stackedClips.end(), [&](const auto& layer)
            { return layer.clipIndex == clipIndex; });
        const bool isStacked = stacked != row.track->stackedClips.end();
        ImGui::TextDisabled("Preview: %s", primary ? "Primary animation"
            : isStacked ? "Stacked animation" : "Available");
        float inactiveWeight = 0.f;
        float& weight = primary ? row.track->weight
            : isStacked ? stacked->weight : inactiveWeight;
        ImGui::BeginDisabled(!primary && !isStacked);
        if (ImGui::SliderFloat("Weight", &weight, 0.f, 1.f, "%.2f"))
            SampleAnimationDocument(document);
        ImGui::EndDisabled();
        auto& clip = clips[clipIndex];
        float length = clip.duration;
        const bool lengthChanged = ImGui::DragFloat("Length (seconds)",
            &length, .01f, .01f, 3600.f, "%.2f s");
        if (ImGui::IsItemActivated())
        {
            document.durationDragBefore = clips;
            document.durationDragClip = clipIndex;
            document.durationDragChanged = false;
        }
        if (lengthChanged)
        {
            if (document.durationDragBefore.empty())
            {
                document.durationDragBefore = clips;
                document.durationDragClip = clipIndex;
            }
            const float next = std::max({ .01f, length, LastKeyTime(clip) });
            if (next != clip.duration)
            {
                clip.duration = next;
                document.durationDragChanged = true;
                refreshClipCache = true;
            }
        }
        if (ImGui::IsItemDeactivated() &&
            document.durationDragClip == clipIndex)
        {
            if (document.durationDragChanged)
            {
                editedTrack = row.track;
                editBefore = std::move(document.durationDragBefore);
            }
            document.durationDragBefore.clear();
            document.durationDragClip = -1;
            document.durationDragChanged = false;
        }
        bool loop = clip.looping;
        if (ImGui::Checkbox("Loop", &loop))
        {
            editBefore = clips;
            editedTrack = row.track;
            clip.looping = loop;
        }
        const float clipLength = std::max(clip.duration, .01f);
        const float localTime = clip.looping
            ? std::fmod(std::max(document.playheadSeconds, 0.f), clipLength)
            : std::min(std::max(document.playheadSeconds, 0.f), clipLength);
        ImGui::TextDisabled("Local time %.2f / %.2f s", localTime,
            clipLength);
        if (primary) ImGui::BeginDisabled();
        if (ImGui::Button(primary ? "Base animation" :
                "Remove stacked animation"))
        {
            if (isStacked)
            {
                removeLayerFrom = row.track;
                removeLayerIndex = static_cast<size_t>(
                    stacked - row.track->stackedClips.begin());
            }
        }
        if (primary) ImGui::EndDisabled();
    };
    const auto activateClip = [&](Row& row)
    {
        if (document.rigIndex != row.track->index)
            FocusAnimationRig(document, row.track->index);
        document.clipIndex = row.clipIndex;
    };
    using TimelineKey = AnimationDocument::TimelineKey;
    struct KeyVisual
    {
        Row* row = nullptr;
        float time = 0.f;
        float x = 0.f;
        float y = 0.f;
        std::vector<TimelineKey> keys;
    };
    std::vector<KeyVisual> keyVisuals;
    const auto sameKey = [](const TimelineKey& a, const TimelineKey& b)
    {
        return a.rigIndex == b.rigIndex && a.clipIndex == b.clipIndex &&
            a.nodeIndex == b.nodeIndex && std::abs(a.time - b.time) <= 1e-4f;
    };
    const auto isSelected = [&](const TimelineKey& key)
    {
        return std::any_of(document.selectedKeys.begin(),
            document.selectedKeys.end(), [&](const TimelineKey& selected)
            { return sameKey(key, selected); });
    };
    const auto drawKey = [&](Row& row, float center, float time,
        std::vector<TimelineKey> keys, ImU32 color)
    {
        const bool selected = std::any_of(keys.begin(), keys.end(), isSelected);
        float shownTime = time;
        if (document.keyDragging && selected &&
            std::all_of(keys.begin(), keys.end(), isSelected))
            shownTime += document.keyDragDelta;
        const float x = timeX + shownTime * pixelsPerSecond;
        if (x < labelRight - 8.f || x > origin.x + canvasWidth + 8.f)
            return;
        if (selected)
            draw->AddCircleFilled({ x, center }, 10.f,
                IM_COL32(255, 225, 125, 95));
        draw->AddQuadFilled({ x, center - 6.f },
            { x + 6.f, center }, { x, center + 6.f },
            { x - 6.f, center }, selected
                ? IM_COL32(255, 226, 135, 255) : color);
        if (selected)
            draw->AddCircle({ x, center }, 8.f,
                IM_COL32(255, 245, 195, 255), 0, 1.5f);
        keyVisuals.push_back({ &row, time, x, center, std::move(keys) });
    };
    for (size_t index = 0; index < rows.size(); ++index)
    {
        Row& row = rows[index];
        const float top = origin.y + rulerHeight + row.offset;
        const float bottom = top + row.height;
        if (row.kind != Row::Kind::Header &&
            (bottom < visibleTop || top > visibleBottom)) continue;
        const float center = top + row.height * .5f;
        const ImVec4 rigColor = RigTimelineColor(row.track->index);
        const ImU32 color = ImGui::GetColorU32(rigColor);
        const bool mouseInRow = hovered && mouse.y >= top &&
            mouse.y < bottom;
        if (row.kind == Row::Kind::Header)
        {
            draw->AddRectFilled({ origin.x, top },
                { origin.x + canvasWidth, bottom },
                ImGui::GetColorU32(RigTimelineTint(rigColor, .42f)));
            draw->AddRectFilled({ pinnedX, top },
                { labelRight, bottom },
                ImGui::GetColorU32(RigTimelineTint(rigColor,
                    row.track->clipIndex == row.clipIndex ? .72f : .42f)));
            const std::string label = row.clip
                ? row.clip->clipName : "No animation";
            const std::string rigLabel = row.track->name;
            const ImVec2 rigSize = ImGui::CalcTextSize(rigLabel.c_str());
            const bool showRig = rigSize.x < labelWidth * .35f;
            const float cogX = labelRight - 27.f;
            const ImVec2 arrowCenter{ pinnedX + 12.f, center };
            if (*row.expanded)
                draw->AddTriangleFilled(
                    { arrowCenter.x - 5.f, arrowCenter.y - 3.f },
                    { arrowCenter.x + 5.f, arrowCenter.y - 3.f },
                    { arrowCenter.x, arrowCenter.y + 4.f }, color);
            else
                draw->AddTriangleFilled(
                    { arrowCenter.x - 3.f, arrowCenter.y - 5.f },
                    { arrowCenter.x - 3.f, arrowCenter.y + 5.f },
                    { arrowCenter.x + 4.f, arrowCenter.y }, color);
            draw->PushClipRect({ pinnedX + 24.f, top },
                { std::max(pinnedX + 25.f,
                    cogX - (showRig ? rigSize.x : 0.f) - 12.f),
                    bottom }, true);
            draw->AddText({ pinnedX + 25.f, top + 5.f }, color,
                label.c_str());
            draw->PopClipRect();
            if (showRig)
                draw->AddText({ cogX - rigSize.x - 5.f, top + 5.f },
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    rigLabel.c_str());
            if (mouseInRow && mouse.x < cogX)
                ImGui::SetTooltip("%s / %s", label.c_str(),
                    rigLabel.c_str());
            if (mouseInRow && leftClicked &&
                mouse.x < cogX && !document.playheadDragging)
            {
                activateClip(row);
                *row.expanded = !*row.expanded;
            }
            ImGui::PushID(row.track);
            ImGui::PushID(row.clipIndex);
            ImGui::SetCursorScreenPos({ cogX, top + 3.f });
            if (ImGui::Button(Icons::Settings, { 22.f, 22.f }))
            {
                if (document.rigIndex != row.track->index)
                    FocusAnimationRig(document, row.track->index);
                ImGui::OpenPopup("##AnimationProperties");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Animation properties");
            ImGui::SetNextWindowSize({ 320.f, 0.f }, ImGuiCond_Appearing);
            if (ImGui::BeginPopup("##AnimationProperties"))
            {
                // Clip properties stay available while the bone rows are collapsed.
                ImGui::TextUnformatted(label.c_str());
                ImGui::Separator();
                drawClipProperties(row);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::PopID();
            if (row.keyTimes)
            {
                std::vector<std::pair<float, TimelineKey>> all;
                for (const auto& [node, times] : *row.keyTimes)
                    for (float time : times)
                        all.push_back({ time, { row.track->index,
                            row.clipIndex, node, time } });
                std::sort(all.begin(), all.end(), [](const auto& a,
                    const auto& b) { return a.first < b.first; });
                for (size_t first = 0; first < all.size();)
                {
                    size_t last = first + 1;
                    std::vector<TimelineKey> group{ all[first].second };
                    while (last < all.size() &&
                        std::abs(all[last].first - all[first].first) <= 1e-4f)
                        group.push_back(all[last++].second);
                    drawKey(row, center, all[first].first,
                        std::move(group), color);
                    first = last;
                }
            }
            continue;
        }
        if (index % 2 == 0)
            draw->AddRectFilled({ origin.x, top },
                { origin.x + canvasWidth, bottom },
                IM_COL32(130, 140, 160, 13));
        const bool activeBone = document.rigIndex == row.track->index &&
            document.selectedBone == static_cast<int>(row.boneIndex);
        if (activeBone)
            draw->AddRectFilled({ origin.x, top },
                { origin.x + canvasWidth, bottom },
                ImGui::GetColorU32(RigTimelineTint(rigColor, .25f)));
        draw->AddLine({ origin.x, bottom },
            { origin.x + canvasWidth, bottom },
            IM_COL32(150, 160, 175, 28));

        const auto times = row.keyTimes
            ? row.keyTimes->find(row.nodeIndex)
            : std::unordered_map<unsigned, std::vector<float>>::const_iterator{};
        if (row.keyTimes && times != row.keyTimes->end())
        for (auto time = std::lower_bound(times->second.begin(),
                 times->second.end(), visibleStart - 8.f / pixelsPerSecond);
             time != times->second.end() &&
                 *time <= visibleEnd + 8.f / pixelsPerSecond; ++time)
        {
            drawKey(row, center, *time,
                { { row.track->index, row.clipIndex,
                    row.nodeIndex, *time } }, color);
        }
        draw->AddRectFilled({ pinnedX, top },
            { labelRight, bottom },
            ImGui::GetColorU32(ImGuiCol_WindowBg));
        draw->AddText({ pinnedX + 12.f, top + 4.f },
            activeBone ? color : ImGui::GetColorU32(ImGuiCol_Text),
            row.joint->name.c_str());
        if (mouseInRow && mouse.x < labelRight && leftClicked &&
            !document.playheadDragging)
        {
            activateClip(row);
            document.selectedBone = static_cast<int>(row.boneIndex);
            document.poseScene->SetSelectedObject(row.joint);
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(row.joint);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(row.joint);
        }
    }
    const auto addSelection = [&](const TimelineKey& key)
    {
        if (!isSelected(key)) document.selectedKeys.push_back(key);
    };
    const auto findRow = [&](float y) -> Row*
    {
        for (Row& row : rows)
        {
            const float top = origin.y + rulerHeight + row.offset;
            if (y >= top && y < top + row.height) return &row;
        }
        return nullptr;
    };
    KeyVisual* hit = nullptr;
    for (KeyVisual& visual : keyVisuals)
        if (mouse.x >= labelRight && std::abs(mouse.x - visual.x) <= 8.f &&
            std::abs(mouse.y - visual.y) <= visual.row->height * .5f &&
            (!hit || std::abs(mouse.x - visual.x) <
                std::abs(mouse.x - hit->x))) hit = &visual;
    const bool additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
    if (rightClicked && mouse.x >= labelRight && !onRuler)
    {
        document.marqueeActive = true;
        document.marqueeStartX = mouse.x;
        document.marqueeStartY = mouse.y;
        document.marqueeAdditive = additive;
    }
    if (document.marqueeActive)
    {
        const float minX = std::min(document.marqueeStartX, mouse.x);
        const float maxX = std::max(document.marqueeStartX, mouse.x);
        const float minY = std::min(document.marqueeStartY, mouse.y);
        const float maxY = std::max(document.marqueeStartY, mouse.y);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            draw->AddRectFilled({ minX, minY }, { maxX, maxY },
                IM_COL32(100, 160, 255, 48));
            draw->AddRect({ minX, minY }, { maxX, maxY },
                IM_COL32(130, 185, 255, 220));
        }
        else
        {
            if (!document.marqueeAdditive) document.selectedKeys.clear();
            const size_t rig = [&]()
            {
                for (const KeyVisual& visual : keyVisuals)
                    if (visual.x >= minX && visual.x <= maxX &&
                        visual.y >= minY && visual.y <= maxY)
                        return visual.row->track->index;
                return static_cast<size_t>(-1);
            }();
            if (rig != static_cast<size_t>(-1))
            {
                if (!document.selectedKeys.empty() &&
                    document.selectedKeys.front().rigIndex != rig)
                    document.selectedKeys.clear();
                for (const KeyVisual& visual : keyVisuals)
                    if (visual.row->track->index == rig &&
                        visual.x >= minX && visual.x <= maxX &&
                        visual.y >= minY && visual.y <= maxY)
                        for (const TimelineKey& key : visual.keys)
                            addSelection(key);
            }
            document.marqueeActive = false;
        }
    }
    if (leftClicked && mouse.x >= labelRight && !onRuler &&
        !document.marqueeActive && !document.playheadDragging)
    {
        Row* row = findRow(mouse.y);
        if (hit && hit->row->mappingReady &&
            !(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                hit->row->kind == Row::Kind::Header))
        {
            activateClip(*hit->row);
            if (hit->row->kind == Row::Kind::Bone)
            {
                document.selectedBone = static_cast<int>(hit->row->boneIndex);
                document.poseScene->SetSelectedObject(hit->row->joint);
                if (m_primaryHierarchy)
                    m_primaryHierarchy->SetSelectedObject(hit->row->joint);
                if (m_primaryProperties)
                    m_primaryProperties->SetSelectedObject(hit->row->joint);
            }
            const bool alreadySelected = std::any_of(hit->keys.begin(),
                hit->keys.end(), isSelected);
            if (!additive && !alreadySelected)
                document.selectedKeys.clear();
            if (!document.selectedKeys.empty() &&
                document.selectedKeys.front().rigIndex !=
                    hit->row->track->index)
                document.selectedKeys.clear();
            for (const TimelineKey& key : hit->keys)
            {
                if (additive && isSelected(key))
                    document.selectedKeys.erase(std::remove_if(
                        document.selectedKeys.begin(),
                        document.selectedKeys.end(),
                        [&](const TimelineKey& item)
                        { return sameKey(item, key); }),
                        document.selectedKeys.end());
                else addSelection(key);
            }
            document.keyPressX = mouse.x;
            document.keyPressActive = true;
            document.keyDragging = false;
            document.keyDragDelta = 0.f;
            document.playheadSeconds = hit->time;
            document.playing = false;
            SampleAnimationDocument(document);
        }
        else if (row && row->clip && row->mappingReady &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            activateClip(*row);
            document.playheadSeconds = hit ? hit->time : timeAt(mouse.x);
            document.playing = false;
            SampleAnimationDocument(document);
            auto* model = row->track->poseSkeleton->ResolveModel();
            if (model)
            {
                auto before = row->track->sourceManager->clips;
                auto& clip = row->track->sourceManager->clips[row->clipIndex];
                const auto& joints = row->track->poseSkeleton->ResolveJoints();
                bool keyed = false;
                for (size_t boneIndex = 0; boneIndex < joints.size(); ++boneIndex)
                {
                    if (row->kind == Row::Kind::Bone &&
                        boneIndex != row->boneIndex) continue;
                    auto* joint = joints[boneIndex];
                    if (!joint) continue;
                    auto* bone = joint->GetComponent<
                        Engine::Components::AnimationBone>();
                    if (!bone && boneIndex >=
                        row->track->poseSkeleton->jointNodes.size()) continue;
                    const unsigned node = bone ? bone->nodeIndex :
                        row->track->poseSkeleton->jointNodes[boneIndex];
                    const auto& transform = joint->transform;
                    if (!AnimationClipEditing::UpsertBonePose(clip, node,
                            document.playheadSeconds, transform.position,
                            glm::quat(transform.rotation), transform.scale))
                        continue;
                    const std::string targetPath =
                        AnimationClipMapping::NodePath(*model, node);
                    for (auto& channel : clip.channels)
                        if (channel.nodeIndex == node &&
                            channel.targetPath.empty())
                            channel.targetPath = targetPath;
                    keyed = true;
                }
                if (keyed) RecordAnimationEdit(document, std::move(before));
            }
        }
        else if (row && !additive) document.selectedKeys.clear();
    }
    if (document.keyPressActive)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (std::abs(mouse.x - document.keyPressX) > 3.f)
                document.keyDragging = true;
            if (document.keyDragging && !document.selectedKeys.empty())
            {
                float earliest = document.selectedKeys.front().time;
                for (const TimelineKey& key : document.selectedKeys)
                    earliest = std::min(earliest, key.time);
                document.keyDragDelta = std::max(-earliest,
                    (mouse.x - document.keyPressX) / pixelsPerSecond);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        }
        else
        {
            if (document.keyDragging &&
                std::abs(document.keyDragDelta) > 1e-5f &&
                !document.selectedKeys.empty())
            {
                const size_t rig = document.selectedKeys.front().rigIndex;
                auto track = std::find_if(document.tracks.begin(),
                    document.tracks.end(), [&](const auto& candidate)
                    { return candidate.index == rig; });
                if (track != document.tracks.end())
                {
                    auto before = track->sourceManager->clips;
                    auto moved = before;
                    bool valid = true;
                    for (size_t clipIndex = 0; clipIndex < moved.size(); ++clipIndex)
                    {
                        std::vector<AnimationClipEditing::KeyRef> keys;
                        for (const TimelineKey& key : document.selectedKeys)
                            if (key.rigIndex == rig &&
                                key.clipIndex == static_cast<int>(clipIndex))
                                keys.push_back({ key.nodeIndex, key.time });
                        if (!keys.empty() && !AnimationClipEditing::MoveKeys(
                                moved[clipIndex], keys,
                                document.keyDragDelta)) valid = false;
                    }
                    if (valid)
                    {
                        track->sourceManager->clips = std::move(moved);
                        if (document.rigIndex != rig)
                            FocusAnimationRig(document, rig);
                        for (TimelineKey& key : document.selectedKeys)
                            key.time += document.keyDragDelta;
                        document.playheadSeconds = std::max(0.f,
                            document.playheadSeconds + document.keyDragDelta);
                        RecordAnimationEdit(document, std::move(before));
                    }
                }
            }
            document.keyPressActive = false;
            document.keyDragging = false;
            document.keyDragDelta = 0.f;
        }
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput &&
        !document.selectedKeys.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
            ImGui::IsKeyPressed(ImGuiKey_Backspace)))
    {
        const size_t rig = document.selectedKeys.front().rigIndex;
        auto track = std::find_if(document.tracks.begin(),
            document.tracks.end(), [&](const auto& candidate)
            { return candidate.index == rig; });
        if (track != document.tracks.end())
        {
            auto before = track->sourceManager->clips;
            bool removed = false;
            for (const TimelineKey& key : document.selectedKeys)
                if (key.rigIndex == rig && key.clipIndex >= 0 &&
                    key.clipIndex < static_cast<int>(
                        track->sourceManager->clips.size()))
                    removed |= AnimationClipEditing::DeleteBoneKey(
                        track->sourceManager->clips[key.clipIndex],
                        key.nodeIndex, key.time);
            if (removed)
            {
                if (document.rigIndex != rig) FocusAnimationRig(document, rig);
                document.selectedKeys.clear();
                RecordAnimationEdit(document, std::move(before));
            }
        }
    }
    if (editedTrack)
    {
        if (document.rigIndex != editedTrack->index)
            FocusAnimationRig(document, editedTrack->index);
        RecordAnimationEdit(document, std::move(editBefore));
    }
    else if (refreshClipCache)
    {
        ++document.clipRevision;
        document.clipCache.clear();
        document.previewLayouts.clear();
        SampleAnimationDocument(document);
    }
    if (removeLayerFrom && removeLayerIndex <
            removeLayerFrom->stackedClips.size())
    {
        removeLayerFrom->stackedClips.erase(
            removeLayerFrom->stackedClips.begin() + removeLayerIndex);
        document.selectedKeys.clear();
        document.clipIndex = removeLayerFrom->clipIndex;
        SampleAnimationDocument(document);
    }
    const float playheadX = timeX +
        document.playheadSeconds * pixelsPerSecond;
    if (playheadX >= labelRight)
    {
        draw->AddLine({ playheadX, origin.y }, { playheadX, bottomY },
            IM_COL32(255, 225, 125, 240), 2.f);
        draw->AddRectFilled({ playheadX - 6.f, origin.y + 1.f },
            { playheadX + 6.f, origin.y + 14.f },
            IM_COL32(255, 225, 125, 255), 2.f);
        char timeLabel[32];
        std::snprintf(timeLabel, sizeof(timeLabel), "%.2f s",
            document.playheadSeconds);
        draw->AddText({ playheadX + 9.f, origin.y + 1.f },
            IM_COL32(255, 225, 125, 255), timeLabel);
    }
    if (document.playing || document.playheadDragging)
    {
        const float scroll = ImGui::GetScrollX();
        const float viewportWidth = ImGui::GetWindowWidth();
        const float markerLocalX = labelWidth +
            document.playheadSeconds * pixelsPerSecond;
        if (markerLocalX > scroll + viewportWidth - 36.f)
            ImGui::SetScrollX(markerLocalX - viewportWidth + 36.f);
        else if (markerLocalX < scroll + labelWidth + 4.f)
            ImGui::SetScrollX(std::max(0.f,
                markerLocalX - labelWidth - 4.f));
    }
    ImGui::EndChild();
}

void EditorState::HandleAnimationDocumentClosures()
{
    for (auto it = m_animationDocuments.begin(); it != m_animationDocuments.end();)
    {
        AnimationDocument* document = it->get();
        if (document->view->IsOpen())
        { ++it; continue; }
        if (document->dirty)
        {
            document->view->SetOpen(true);
            if (m_primaryConsole) m_primaryConsole->AddLog(
                ConsoleView::Level::Warning,
                "Save the animation clip before closing its editor.");
            ++it;
            continue;
        }
        if (m_activeAnimationDocument == document)
            SetActiveAnimationDocument(nullptr);
        for (IEditorPanel* closing : { static_cast<IEditorPanel*>(document->view) })
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
