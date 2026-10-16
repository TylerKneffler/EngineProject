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
#include "Core/Compoonents/Animation/TwoBoneIK.h"
#include "Core/Compoonents/Animation/ChainIK.h"
#include "Core/Model/AnimationControlSpace.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/ComponentReference.h"
#include "Core/Model/RigAsset.h"
#include "Core/Model/PoseAsset.h"
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
#include <limits>
#include <optional>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Editor
{
namespace
{
constexpr size_t kAnimationDropdownLabelLength = 40;

std::string AnimationDropdownLabel(const std::string& label)
{
    size_t end = 0;
    size_t ellipsisEnd = 0;
    size_t characters = 0;
    while (end < label.size() && characters < kAnimationDropdownLabelLength)
    {
        if (characters == kAnimationDropdownLabelLength - 3)
            ellipsisEnd = end;
        const unsigned char byte = static_cast<unsigned char>(label[end]);
        const size_t length = byte < 0x80 ? 1 : byte < 0xE0 ? 2 :
            byte < 0xF0 ? 3 : 4;
        end = std::min(label.size(), end + length);
        ++characters;
    }
    return end == label.size() ? label :
        label.substr(0, ellipsisEnd) + "...";
}

bool AnimationDropdownItem(const std::string& label, bool selected = false)
{
    const std::string display = AnimationDropdownLabel(label);
    const bool clicked = ImGui::Selectable(display.c_str(), selected);
    if (display != label && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", label.c_str());
    return clicked;
}

template <typename Document>
float AnimationLocalTime(const Document& document, size_t rigIndex,
    int clipIndex)
{
    const auto track = std::find_if(document.tracks.begin(),
        document.tracks.end(), [rigIndex](const auto& item)
        { return item.index == rigIndex; });
    if (track == document.tracks.end() || !track->sourceManager ||
        clipIndex < 0 || clipIndex >= static_cast<int>(
            track->sourceManager->clips.size()))
        return document.playheadSeconds;
    float start = 0.f;
    bool repeat = false;
    if (track->clipIndex == clipIndex)
    {
        start = track->startSeconds;
        repeat = track->repeat;
    }
    else
    {
        const auto layer = std::find_if(track->stackedClips.begin(),
            track->stackedClips.end(), [clipIndex](const auto& item)
            { return item.clipIndex == clipIndex; });
        if (layer != track->stackedClips.end())
        {
            start = layer->startSeconds;
            repeat = layer->repeat;
        }
    }
    const float length = std::max(.01f,
        track->sourceManager->clips[clipIndex].duration);
    const float elapsed = std::max(0.f, document.playheadSeconds - start);
    return repeat ? std::fmod(elapsed, length) : elapsed;
}

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

Engine::Model::RigAsset::ControlMap HumanoidControls(
    const Engine::Components::Skeleton& skeleton)
{
    Engine::Model::RigAsset::ControlMap result;
    const auto& joints = skeleton.ResolveJoints();
    const auto nodeOf = [&](const Engine::Core::Object* object) -> int
    {
        const auto found = std::find(joints.begin(), joints.end(), object);
        const size_t index = static_cast<size_t>(found - joints.begin());
        return found == joints.end() || index >= skeleton.jointNodes.size()
            ? -1 : static_cast<int>(skeleton.jointNodes[index]);
    };
    const auto normalized = [](std::string name)
    {
        name = LowerText(std::move(name));
        name.erase(std::remove_if(name.begin(), name.end(),
            [](unsigned char ch) { return !std::isalnum(ch); }), name.end());
        return name;
    };
    for (auto* joint : joints)
    {
        if (!joint) continue;
        const auto name = normalized(joint->name);
        if (result.pelvis < 0 && (name.find("pelvis") != std::string::npos ||
            name.find("hips") != std::string::npos || name == "hip"))
            result.pelvis = nodeOf(joint);
        if (result.root < 0 && (name == "root" || name == "armature" ||
            name == "bip001")) result.root = nodeOf(joint);
    }
    if (result.root < 0)
        for (auto* joint : joints)
            if (joint && nodeOf(joint->Parent) < 0)
            { result.root = nodeOf(joint); break; }
    if (result.pelvis < 0) result.pelvis = result.root;
    for (size_t index = 0; index < result.limbs.size(); ++index)
    {
        const bool left = index == 0 || index == 2;
        const bool arm = index < 2;
        for (auto* joint : joints)
        {
            if (!joint || !joint->Parent || !joint->Parent->Parent) continue;
            const auto name = normalized(joint->name);
            const bool side = left
                ? name.find("left") != std::string::npos ||
                    (!name.empty() && (name.front() == 'l' || name.back() == 'l'))
                : name.find("right") != std::string::npos ||
                    (!name.empty() && (name.front() == 'r' || name.back() == 'r'));
            const bool tip = arm
                ? name.find("hand") != std::string::npos ||
                    name.find("wrist") != std::string::npos
                : name.find("foot") != std::string::npos ||
                    name.find("ankle") != std::string::npos;
            if (!side || !tip || (arm && name.find("thumb") != std::string::npos) ||
                (!arm && name.find("toe") != std::string::npos)) continue;
            const int upper = nodeOf(joint->Parent->Parent);
            const int lower = nodeOf(joint->Parent);
            const int end = nodeOf(joint);
            if (upper >= 0 && lower >= 0 && end >= 0)
            {
                result.limbs[index] = {upper, lower, end};
                break;
            }
        }
    }
    return result;
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
            if (!track.path.empty())
                if (auto rig = Engine::Model::RigAsset::Load(
                        track.path, sourceRig->skinIndex))
                {
                    track.controls = rig->controls;
                    track.poses = rig->poses;
                    track.symmetryPairs = rig->symmetryPairs;
                }
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
        view->SetPoseEditing(true);
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
                    const bool changed = raw->gizmoDragIK || (bone &&
                        (bone->transform.position !=
                            raw->gizmoDragStartPosition ||
                         bone->transform.rotation !=
                            raw->gizmoDragStartRotation ||
                         bone->transform.scale !=
                            raw->gizmoDragStartScale));
                    raw->gizmoDragBone = nullptr;
                    if (raw->autoKey && changed)
                    {
                        if (raw->gizmoDragIK && bone && bone->Parent && bone->Parent->Parent)
                            AddAnimationLimbKey(*raw, *bone->Parent->Parent, *bone->Parent,
                                *bone, false);
                        else AddAnimationKey(*raw);
                    }
                    raw->gizmoDragIK = false;
                }
            }
            if (m_renderer && active) m_renderer->MarkDirty();
        };
        view->OnFocused = [this, raw]()
        { SetActiveAnimationDocument(raw); };
        view->OnDrawBottomPanel = [this, raw](IEditorUi& ui)
        { DrawAnimationTimeline(ui, *raw); };
        view->OnDrawRigControls = [this, raw](IEditorUi& ui,
            const EditorUiViewportInput& input)
        { return DrawAnimationRigViewport(ui, input, *raw); };
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
            document.ikControlTip = -1;
            document.rigHandleKind = -1;
            document.rigHandleDrag = -1;
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
    // Build clip-wide key groups once per revision, rather than sorting all
    // bone keys on every timeline frame.
    std::vector<std::pair<float, unsigned>> summary;
    for (const auto& [node, times] : cached.keyTimes)
        for (float time : times) summary.emplace_back(time, node);
    std::sort(summary.begin(), summary.end());
    for (const auto& [time, node] : summary)
    {
        if (cached.summaryKeys.empty() ||
            std::abs(time - cached.summaryKeys.back().time) > 1e-4f)
            cached.summaryKeys.push_back({time, {}});
        cached.summaryKeys.back().keys.emplace_back(node, time);
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
            { return track.weight != 1.f || track.startSeconds > 0.f ||
                track.repeat || !track.stackedClips.empty(); });
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
                    float startSeconds, bool repeat, int repeatCount,
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
                        layer.looping = false;
                        layer.nodeMask = track.poseSkeleton->jointNodes;
                        preview->layers.push_back(std::move(layer));
                    }
                    auto& layer = preview->layers[previewLayer++];
                    const float length = std::max(.01f,
                        track.sourceManager->clips[clipIndex].duration);
                    const float elapsed = document.playheadSeconds -
                        startSeconds;
                    const bool active = elapsed >= 0.f &&
                        (repeat ? repeatCount == 0 ||
                            elapsed < length * repeatCount :
                            elapsed <= length);
                    layer.time = repeat ? std::fmod(
                        std::max(0.f, elapsed), length) :
                        std::clamp(elapsed, 0.f, length);
                    layer.weight = active ? weight : 0.f;
                };
                addLayer(track.clipIndex, track.weight,
                    track.startSeconds, track.repeat, track.repeatCount, 0);
                for (size_t layerIndex = 0;
                    layerIndex < track.stackedClips.size(); ++layerIndex)
                    addLayer(track.stackedClips[layerIndex].clipIndex,
                        track.stackedClips[layerIndex].weight,
                        track.stackedClips[layerIndex].startSeconds,
                        track.stackedClips[layerIndex].repeat,
                        track.stackedClips[layerIndex].repeatCount,
                        layerIndex + 1);
            }
            if (rebuild) document.previewLayouts[preview] = std::move(layout);
            preview->holdCurrentPoseWhenStopped = false;
            preview->Tick(0.f);
            preview->holdCurrentPoseWhenStopped = true;
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
        document.poseScene->ApplyPoseConstraints();
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
    document.poseManager->looping = false;
    document.poseManager->holdCurrentPoseWhenStopped = false;
    document.poseManager->Tick(0.f);
    document.poseManager->holdCurrentPoseWhenStopped = true;
    document.poseScene->ApplyPoseConstraints();
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
        document.savedSnapshot || std::any_of(document.tracks.begin(),
            document.tracks.end(), [](const auto& track)
            { return track.controlsDirty; });
    RefreshAnimationDocumentTitle(document);
    SampleAnimationDocument(document);
}

bool EditorState::CopyAnimationKeys(AnimationDocument& document)
{
    if (document.selectedKeys.empty()) return false;
    const size_t rig = document.selectedKeys.front().rigIndex;
    const auto track = std::find_if(document.tracks.begin(),
        document.tracks.end(), [rig](const auto& item)
        { return item.index == rig; });
    if (track == document.tracks.end() || !track->sourceManager) return false;
    const auto startFor = [&](int clipIndex)
    {
        if (clipIndex == track->clipIndex) return track->startSeconds;
        const auto layer = std::find_if(track->stackedClips.begin(),
            track->stackedClips.end(), [clipIndex](const auto& item)
            { return item.clipIndex == clipIndex; });
        return layer == track->stackedClips.end() ? 0.f :
            layer->startSeconds;
    };
    float firstTime = std::numeric_limits<float>::infinity();
    for (const auto& key : document.selectedKeys)
        if (key.rigIndex == rig && key.clipIndex >= 0 &&
            key.clipIndex < static_cast<int>(track->sourceManager->clips.size()))
            firstTime = std::min(firstTime,
                startFor(key.clipIndex) + key.time);
    if (!std::isfinite(firstTime)) return false;
    std::vector<Engine::Model::AnimationChannel> copied;
    for (const auto& key : document.selectedKeys)
    {
        if (key.rigIndex != rig || key.clipIndex < 0 ||
            key.clipIndex >= static_cast<int>(track->sourceManager->clips.size()))
            continue;
        auto blocks = AnimationClipEditing::CopyKeyBlocks(
            track->sourceManager->clips[key.clipIndex], key.nodeIndex,
            key.time, startFor(key.clipIndex) + key.time - firstTime);
        copied.insert(copied.end(),
            std::make_move_iterator(blocks.begin()),
            std::make_move_iterator(blocks.end()));
    }
    if (copied.empty()) return false;
    document.copiedKeys = std::move(copied);
    document.copiedKeysRig = rig;
    if (document.rigIndex != rig || document.clipIndex < 0)
        document.pasteClipIndex = document.selectedKeys.front().clipIndex;
    else
        document.pasteClipIndex = document.clipIndex;
    document.error.clear();
    return true;
}

bool EditorState::PasteAnimationKeys(AnimationDocument& document)
{
    if (document.copiedKeys.empty()) return false;
    const size_t rig = document.copiedKeysRig;
    auto track = std::find_if(document.tracks.begin(),
        document.tracks.end(), [rig](const auto& item)
        { return item.index == rig; });
    if (track == document.tracks.end() || !track->sourceManager ||
        document.pasteClipIndex < 0 ||
        document.pasteClipIndex >= static_cast<int>(
            track->sourceManager->clips.size()))
    {
        document.error = "Choose a destination animation before pasting keys.";
        return false;
    }
    auto before = track->sourceManager->clips;
    const float localStart = AnimationLocalTime(document, rig,
        document.pasteClipIndex);
    if (!AnimationClipEditing::PasteKeyBlocks(
            track->sourceManager->clips[document.pasteClipIndex],
            document.copiedKeys, localStart))
    {
        document.error = "Selected keys could not be pasted into this animation.";
        return false;
    }
    if (document.pasteClipIndex != track->clipIndex &&
        std::none_of(track->stackedClips.begin(), track->stackedClips.end(),
            [&](const auto& layer)
            { return layer.clipIndex == document.pasteClipIndex; }))
        track->stackedClips.push_back({ document.pasteClipIndex, 1.f });
    if (document.rigIndex != rig) FocusAnimationRig(document, rig);
    document.clipIndex = document.pasteClipIndex;
    document.selectedKeys.clear();
    for (const auto& channel : document.copiedKeys)
    {
        const AnimationDocument::TimelineKey key{ rig,
            document.pasteClipIndex, channel.nodeIndex,
            localStart + channel.times.front() };
        if (std::none_of(document.selectedKeys.begin(),
                document.selectedKeys.end(), [&](const auto& existing)
                { return existing.rigIndex == key.rigIndex &&
                    existing.clipIndex == key.clipIndex &&
                    existing.nodeIndex == key.nodeIndex &&
                    std::abs(existing.time - key.time) <= 1e-4f; }))
            document.selectedKeys.push_back(key);
    }
    document.error.clear();
    RecordAnimationEdit(document, std::move(before));
    return true;
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
    const float frameTime = AnimationLocalTime(document,
        document.rigIndex, document.clipIndex);
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

bool EditorState::AddAnimationRowKeys(AnimationDocument& document)
{
    if (document.selectedRows.empty())
        return AddAnimationKey(document, true);
    const size_t rig = document.selectedRows.front().rigIndex;
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [rig](const auto& item) { return item.index == rig; });
    if (track == document.tracks.end() || !track->sourceManager ||
        !track->poseSkeleton) return false;
    auto* model = track->poseSkeleton->ResolveModel();
    if (!model) return false;
    auto& clips = track->sourceManager->clips;
    for (const auto& row : document.selectedRows)
    {
        if (row.rigIndex != rig || row.clipIndex < 0 ||
            row.clipIndex >= static_cast<int>(clips.size())) continue;
        if (!AnimationClipMapping::Inspect(clips[row.clipIndex],
                AnimationClipMapping::RigNodes(*track->poseSkeleton),
                AnimationClipMapping::ModelBones(*model)).issues.empty())
        {
            document.error = "Repair unresolved bone mappings before keying these rows.";
            return false;
        }
    }
    auto before = clips;
    bool keyed = false;
    const auto& palette = track->poseSkeleton->jointNodes;
    const auto& joints = track->poseSkeleton->ResolveJoints();
    for (const auto& row : document.selectedRows)
    {
        if (row.rigIndex != rig || row.clipIndex < 0 ||
            row.clipIndex >= static_cast<int>(clips.size())) continue;
        const auto found = std::find(palette.begin(), palette.end(), row.nodeIndex);
        const size_t index = static_cast<size_t>(found - palette.begin());
        if (found == palette.end() || index >= joints.size() || !joints[index])
            continue;
        auto& clip = clips[row.clipIndex];
        const auto& transform = joints[index]->transform;
        keyed = AnimationClipEditing::UpsertBonePose(clip, row.nodeIndex,
            AnimationLocalTime(document, row.rigIndex, row.clipIndex),
            transform.position,
            glm::quat(transform.rotation), transform.scale) || keyed;
        const std::string path = AnimationClipMapping::NodePath(
            *model, row.nodeIndex);
        for (auto& channel : clip.channels)
            if (channel.nodeIndex == row.nodeIndex && channel.targetPath.empty())
                channel.targetPath = path;
    }
    if (keyed)
    {
        if (document.rigIndex != rig) FocusAnimationRig(document, rig);
        RecordAnimationEdit(document, std::move(before));
        document.error.clear();
    }
    return keyed;
}

bool EditorState::AddAnimationIKKeys(AnimationDocument& document,
    Engine::Core::Object& upper, Engine::Core::Object& lower,
    Engine::Core::Object& tip)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return false;
    auto* model = document.poseSkeleton->ResolveModel();
    auto& clip = document.sourceManager->clips[document.clipIndex];
    if (!model || !AnimationClipMapping::Inspect(clip,
            AnimationClipMapping::RigNodes(*document.poseSkeleton),
            AnimationClipMapping::ModelBones(*model)).issues.empty())
    {
        document.error = "Repair unresolved bone mappings before keying this clip.";
        return false;
    }
    const auto& joints = document.poseSkeleton->ResolveJoints();
    auto before = document.sourceManager->clips;
    bool keyed = false;
    for (auto* joint : { &upper, &lower, &tip })
    {
        const auto found = std::find(joints.begin(), joints.end(), joint);
        if (found == joints.end()) continue;
        const size_t palette = static_cast<size_t>(found - joints.begin());
        if (palette >= document.poseSkeleton->jointNodes.size()) continue;
        const unsigned node = document.poseSkeleton->jointNodes[palette];
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
        clip.duration = std::max(clip.duration, AnimationLocalTime(document,
            document.rigIndex, document.clipIndex));
        RecordAnimationEdit(document, std::move(before));
    }
    return keyed;
}

bool EditorState::AddAnimationLimbKey(AnimationDocument& document,
    Engine::Core::Object& upper, Engine::Core::Object& lower,
    Engine::Core::Object& tip, bool matchIncoming)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return false;
    auto* model = document.poseSkeleton->ResolveModel();
    if (!model || !model->Owner) return false;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    const auto indexOf = [&](Engine::Core::Object& object) -> unsigned
    {
        const auto found = std::find(joints.begin(), joints.end(), &object);
        const size_t palette = static_cast<size_t>(found - joints.begin());
        return found == joints.end() || palette >= document.poseSkeleton->jointNodes.size()
            ? static_cast<unsigned>(-1) : document.poseSkeleton->jointNodes[palette];
    };
    const unsigned upperNode = indexOf(upper), lowerNode = indexOf(lower),
        tipNode = indexOf(tip);
    if (upperNode == static_cast<unsigned>(-1) ||
        lowerNode == static_cast<unsigned>(-1) ||
        tipNode == static_cast<unsigned>(-1)) return false;
    auto before = document.sourceManager->clips;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    auto found = std::find_if(clip.limbs.begin(), clip.limbs.end(),
        [&](const auto& limb) { return limb.tip == tipNode; });
    if (found == clip.limbs.end())
    {
        clip.limbs.push_back({upperNode, lowerNode, tipNode, {}});
        found = std::prev(clip.limbs.end());
    }
    const bool hadKeys = !found->keys.empty();
    const auto outgoing = hadKeys
        ? Engine::Model::AnimationClip::SampleLimb(*found,
            AnimationLocalTime(document, document.rigIndex,
                document.clipIndex))
        : Engine::Model::AnimationClip::LimbKey{};
    if (matchIncoming && document.ikBlend > 0.f)
    {
        document.ikTarget = tip.transform.GetWorldPosition();
        const glm::vec3 root = upper.transform.GetWorldPosition();
        const glm::vec3 mid = lower.transform.GetWorldPosition();
        document.ikPole = mid + mid - (root + document.ikTarget) * .5f;
        glm::mat3 basis(tip.transform.GetWorldMatrix());
        for (int axis = 0; axis < 3; ++axis)
            if (glm::length(basis[axis]) > 1e-6f)
                basis[axis] = glm::normalize(basis[axis]);
        document.ikTargetRotation = glm::eulerAngles(glm::normalize(glm::quat_cast(basis)));
    }
    if (matchIncoming && document.ikBlend <= 0.f)
    {
        for (auto* joint : {&upper, &lower, &tip})
        {
            const unsigned node = indexOf(*joint);
            const auto& transform = joint->transform;
            AnimationClipEditing::UpsertBonePose(clip, node,
                AnimationLocalTime(document, document.rigIndex,
                    document.clipIndex), transform.position,
                glm::quat(transform.rotation), transform.scale);
            const std::string path = AnimationClipMapping::NodePath(*model, node);
            for (auto& channel : clip.channels)
                if (channel.nodeIndex == node && channel.targetPath.empty())
                    channel.targetPath = path;
        }
    }
    Engine::Model::AnimationClip::LimbKey key;
    key.time = AnimationLocalTime(document, document.rigIndex,
        document.clipIndex);
    key.space = document.ikSpace;
    key.spaceNode = document.ikSpaceNode;
    key.propReference = document.ikPropReference;
    const glm::mat4 space = Engine::Model::LimbSpaceMatrix(key,
        model->Owner->transform.GetWorldMatrix(), upper, model->ResolveNodes());
    const glm::mat4 inverseSpace = glm::inverse(space);
    key.target = glm::vec3(inverseSpace * glm::vec4(document.ikTarget, 1.f));
    key.pole = glm::vec3(inverseSpace * glm::vec4(document.ikPole, 1.f));
    key.rotation = glm::normalize(glm::inverse(Engine::Model::LimbSpaceRotation(space)) *
        glm::quat(document.ikTargetRotation));
    key.blend = std::clamp(document.ikBlend, 0.f, 1.f);
    auto& keys = found->keys;
    if ((matchIncoming || !hadKeys || outgoing.space != key.space ||
            outgoing.spaceNode != key.spaceNode ||
            outgoing.propReference.objectPath != key.propReference.objectPath) &&
        key.time > .002f &&
        (!hadKeys || std::abs(outgoing.blend - key.blend) > .0001f ||
            outgoing.space != key.space || outgoing.spaceNode != key.spaceNode ||
            outgoing.propReference.objectPath != key.propReference.objectPath))
    {
        if (!hadKeys)
        {
            auto start = key;
            start.time = 0.f;
            start.blend = 0.f;
            keys.push_back(start);
        }
        auto guard = outgoing;
        guard.time = key.time - .001f;
        const auto guardAt = std::lower_bound(keys.begin(), keys.end(), guard.time,
            [](const auto& existing, float time) { return existing.time < time; });
        keys.insert(guardAt, guard);
    }
    const auto at = std::lower_bound(keys.begin(), keys.end(), key.time,
        [](const auto& existing, float time) { return existing.time < time; });
    if (at != keys.end() && std::abs(at->time - key.time) < .0001f)
        *at = key;
    else keys.insert(at, key);
    clip.duration = std::max(clip.duration, key.time);
    document.ikControlTime = -1.f;
    RecordAnimationEdit(document, std::move(before));
    return true;
}

bool EditorState::BakeAnimationLimbs(AnimationDocument& document)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return false;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    if (clip.limbs.empty()) return false;
    auto* model = document.poseSkeleton->ResolveModel();
    if (!model) return false;
    const auto before = document.sourceManager->clips;
    auto baked = clip;
    const float originalTime = document.playheadSeconds;
    const auto originalTracks = document.tracks;
    for (auto& track : document.tracks)
    {
        track.selected = track.index == document.rigIndex;
        if (track.selected)
        {
            track.weight = 1.f;
            track.startSeconds = 0.f;
            track.repeat = false;
            track.repeatCount = 0;
            track.stackedClips.clear();
        }
    }
    document.previewLayouts.clear();
    const int frames = std::max(1, static_cast<int>(std::ceil(clip.duration * 30.f)));
    std::vector<float> sampleTimes;
    sampleTimes.reserve(static_cast<size_t>(frames + 1));
    for (int frame = 0; frame <= frames; ++frame)
        sampleTimes.push_back(clip.duration * frame / frames);
    for (const auto& limb : clip.limbs)
        for (const auto& key : limb.keys)
            sampleTimes.push_back(key.time);
    std::sort(sampleTimes.begin(), sampleTimes.end());
    sampleTimes.erase(std::unique(sampleTimes.begin(), sampleTimes.end()),
        sampleTimes.end());
    const auto& nodes = model->ResolveNodes();
    for (float sampleTime : sampleTimes)
    {
        document.playheadSeconds = sampleTime;
        SampleAnimationDocument(document);
        for (const auto& limb : clip.limbs)
            for (unsigned node : {limb.upper, limb.lower, limb.tip})
                if (node < nodes.size() && nodes[node])
                {
                    const auto& transform = nodes[node]->transform;
                    AnimationClipEditing::UpsertBonePose(baked, node,
                        document.playheadSeconds, transform.position,
                        glm::quat(transform.rotation), transform.scale);
                    const std::string path = AnimationClipMapping::NodePath(*model, node);
                    for (auto& channel : baked.channels)
                        if (channel.nodeIndex == node && channel.targetPath.empty())
                            channel.targetPath = path;
                }
    }
    document.playheadSeconds = originalTime;
    document.tracks = originalTracks;
    document.previewLayouts.clear();
    baked.limbs.clear();
    clip = std::move(baked);
    document.ikControlTime = -1.f;
    RecordAnimationEdit(document, before);
    return true;
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
        document.savedSnapshot || std::any_of(document.tracks.begin(),
            document.tracks.end(), [](const auto& track)
            { return track.controlsDirty; });
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
    const auto previousRig = Engine::Model::RigAsset::Load(
        rigPath, latestSkeleton->skinIndex);
    for (const auto& track : document.tracks)
        if (track.index == document.rigIndex)
        {
            rig.controls = track.controls;
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
        { track.path = rigPath; track.controlsDirty = false; break; }
    document.savedSnapshot = document.sourceScene->SaveToString();
    document.dirty = std::any_of(document.tracks.begin(), document.tracks.end(),
        [](const auto& track) { return track.controlsDirty; });
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
        for (const auto& rigValue :
            Engine::Model::RigAsset::LoadAll(rigPath.string()))
        {
            const auto* rig = &rigValue;
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
    const std::string preview = AnimationDropdownLabel(label);
    ImGui::SetNextWindowSizeConstraints({ 240.f, 0.f },
        { 420.f, FLT_MAX });
    if (!ImGui::BeginCombo("##AnimationRigToolbar", preview.c_str())) return;
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
            if (AnimationDropdownItem(track.name,
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
        if (AnimationDropdownItem(asset.label,
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

void EditorState::DrawAnimationRigControls(IEditorUi& ui,
    AnimationDocument& document)
{
    if (!document.poseSkeleton ||
        !ImGui::CollapsingHeader("Viewport rig controls")) return;
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [&](const auto& item) { return item.index == document.rigIndex; });
    if (track == document.tracks.end()) return;
    auto& controls = track->controls;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    const auto& palette = document.poseSkeleton->jointNodes;
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
        controls = HumanoidControls(*document.poseSkeleton);
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
        !ImGui::CollapsingHeader("Pose library and symmetry")) return;
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
                    position = targetRest.position + mirrorVector(
                        position - sourceRest.position);
                    rotation = mirrorRotation(rotation *
                        glm::inverse(glm::quat(sourceRest.rotation))) *
                        glm::quat(targetRest.rotation);
                    for (int axis = 0; axis < 3; ++axis)
                        scale[axis] = targetRest.scale[axis] *
                            (std::abs(sourceRest.scale[axis]) > 1e-6f
                                ? scale[axis] / sourceRest.scale[axis] : 1.f);
                }
                auto& transform = joints[destination]->transform;
                transform.position = glm::mix(transform.position, position,
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
        !ImGui::CollapsingHeader("Chain and full-body IK")) return;
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
    DrawAnimationRigControls(ui, document);
    DrawAnimationPoseLibrary(ui, document);
    DrawAnimationChainIK(ui, document);
    ui.Separator();
    Engine::Core::Component::DrawAssetPathPicker(ui, &document,
        "animationAssetToAdd", "Animation asset", "animation",
        document.animationAssetToAdd);
    ui.SameLine();
    if (ui.Button("Add to rig"))
    {
        const auto asset = Engine::Model::AnimationClipAsset::Load(
            document.animationAssetToAdd);
        auto rig = Engine::Model::RigAsset::Load(
            document.rigPath, static_cast<unsigned>(document.rigIndex));
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
    if (validChain && ImGui::CollapsingHeader("Two-bone IK"))
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
        if (ui.Button("Bake IK to bone keys (30 fps)"))
            BakeAnimationLimbs(document);
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
    if (document.playing &&
        document.clipIndex >= 0 &&
        document.clipIndex < static_cast<int>(clips.size()))
    {
        document.playheadSeconds += std::max(0.f, ImGui::GetIO().DeltaTime);
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
    if (ui.Button(document.selectedRows.empty()
            ? "Key Selected Bone" : "Key Selected Rows"))
    {
        document.playing = false;
        AddAnimationRowKeys(document);
    }
    ui.SameLine();
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
    if (!document.selectedRows.empty()) ui.SameLine();
    ImGui::BeginDisabled(document.selectedKeys.empty());
    if (ui.Button("Copy Keys")) CopyAnimationKeys(document);
    ImGui::EndDisabled();
    if (!document.copiedKeys.empty())
    {
        ui.SameLine();
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
            ui.SameLine();
            if (ui.Button("Paste Keys")) PasteAnimationKeys(document);
        }
    }
    ui.SameLine();
    ui.Checkbox("Auto Key", &document.autoKey);
    ui.SameLine();
    ImGui::Text("%.3f s", document.playheadSeconds);
    ImGui::SetNextItemWidth(150.f);
    ImGui::SliderFloat("Zoom", &document.timelinePixelsPerSecond,
        35.f, 3200.f, "%.0f px/s", ImGuiSliderFlags_Logarithmic);
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
        ImGui::SetNextWindowSizeConstraints({ 220.f, 0.f },
            { 420.f, FLT_MAX });
        if (ImGui::BeginCombo("Base animation",
                AnimationDropdownLabel(baseName).c_str()))
        {
            for (int index = 0; index < static_cast<int>(available.size()); ++index)
            {
                ImGui::PushID(index);
                if (AnimationDropdownItem(available[index].clipName,
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
                    document.selectedRows.clear();
                    document.rowSelectionAnchor.reset();
                    SampleAnimationDocument(document);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextWindowSizeConstraints({ 220.f, 0.f },
            { 420.f, FLT_MAX });
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
                ImGui::PushID(index);
                if (AnimationDropdownItem(available[index].clipName))
                {
                    track.stackedClips.push_back({ index, 1.f });
                    SampleAnimationDocument(document);
                }
                ImGui::PopID();
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
                document.selectedRows.clear();
                document.rowSelectionAnchor.reset();
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
        float startSeconds = 0.f;
        bool repeat = false;
        int repeatCount = 0;
        const std::vector<AnimationDocument::CachedClip::SummaryKey>* summaryKeys = nullptr;
    };
    std::vector<Row> rows;
    for (auto& track : document.tracks)
    {
        if (!track.selected) continue;
        auto& clips = track.sourceManager->clips;
        const auto appendClip = [&](int clipIndex, bool& expanded,
            float startSeconds, bool repeat, int repeatCount)
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
            rows.back().startSeconds = startSeconds;
            rows.back().repeat = repeat;
            rows.back().repeatCount = repeatCount;
            if (clip)
                rows.back().summaryKeys = &CachedAnimationClip(
                    document, track, clipIndex).summaryKeys;
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
                rows.back().startSeconds = startSeconds;
            }
        };
        if (track.clipIndex >= 0 &&
            track.clipIndex < static_cast<int>(clips.size()))
        {
            bool& expanded = track.expandedClips.try_emplace(
                track.clipIndex, true).first->second;
            appendClip(track.clipIndex, expanded, track.startSeconds,
                track.repeat, track.repeatCount);
        }
        for (const auto& layer : track.stackedClips)
            if (layer.clipIndex >= 0 &&
                layer.clipIndex < static_cast<int>(clips.size()) &&
                layer.clipIndex != track.clipIndex)
            {
                bool& expanded = track.expandedClips.try_emplace(
                    layer.clipIndex, true).first->second;
                appendClip(layer.clipIndex, expanded, layer.startSeconds,
                    layer.repeat, layer.repeatCount);
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

    constexpr float steps[] = { .02f, .05f, .1f, .2f, .5f, 1.f, 2.f, 5.f,
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
    if (document.clipDragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        document.clipDragging = false;
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
        float& start = primary ? row.track->startSeconds
            : stacked->startSeconds;
        bool& repeat = primary ? row.track->repeat : stacked->repeat;
        int& count = primary ? row.track->repeatCount : stacked->repeatCount;
        if (ImGui::DragFloat("Start (seconds)", &start, .01f,
                0.f, 3600.f, "%.2f s"))
        {
            start = std::max(0.f, start);
            SampleAnimationDocument(document);
        }
        if (ImGui::RadioButton("Isolated (play once)", !repeat))
        {
            repeat = false;
            SampleAnimationDocument(document);
        }
        if (ImGui::RadioButton("Repeat", repeat))
        {
            repeat = true;
            SampleAnimationDocument(document);
        }
        if (repeat)
        {
            bool infinite = count == 0;
            if (ImGui::Checkbox("Infinite", &infinite))
            {
                count = infinite ? 0 : 2;
                SampleAnimationDocument(document);
            }
            if (!infinite && ImGui::InputInt("Number of plays", &count))
            {
                count = std::max(1, count);
                SampleAnimationDocument(document);
            }
        }
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
        const float elapsed = std::max(document.playheadSeconds - start, 0.f);
        const float localTime = repeat
            ? std::fmod(elapsed, clipLength)
            : std::min(elapsed, clipLength);
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
    // Ordinary hit testing only needs the row under the mouse. A marquee
    // needs every visible row while it is active or starting this frame.
    const bool collectAllKeyVisuals = document.marqueeActive || rightClicked;
    const bool collectHitRowVisuals = hovered && mouse.x >= labelRight;
    const auto sameKey = [](const TimelineKey& a, const TimelineKey& b)
    {
        return a.rigIndex == b.rigIndex && a.clipIndex == b.clipIndex &&
            a.nodeIndex == b.nodeIndex && std::abs(a.time - b.time) <= 1e-4f;
    };
    // Header keys can contain hundreds of bones. Index selection once so
    // drawing a large selection does not scan every selected key per bone.
    std::map<std::tuple<size_t, int, unsigned>, std::vector<float>> selectedTimes;
    for (const auto& key : document.selectedKeys)
        selectedTimes[{key.rigIndex, key.clipIndex, key.nodeIndex}].push_back(key.time);
    for (auto& [identity, times] : selectedTimes)
        std::sort(times.begin(), times.end());
    const auto isDrawSelected = [&](const TimelineKey& key)
    {
        const auto found = selectedTimes.find(
            {key.rigIndex, key.clipIndex, key.nodeIndex});
        if (found == selectedTimes.end()) return false;
        const auto& times = found->second;
        const auto next = std::lower_bound(times.begin(), times.end(), key.time);
        return (next != times.end() && std::abs(*next - key.time) <= 1e-4f) ||
            (next != times.begin() && std::abs(*std::prev(next) - key.time) <= 1e-4f);
    };
    // Input handling below mutates selection during this frame.
    const auto isSelected = [&](const TimelineKey& key)
    {
        return std::any_of(document.selectedKeys.begin(),
            document.selectedKeys.end(), [&](const TimelineKey& selected)
            { return sameKey(key, selected); });
    };
    const auto drawKey = [&](Row& row, float center, float time,
        const std::vector<TimelineKey>& keys, ImU32 color)
    {
        const bool selected = std::any_of(keys.begin(), keys.end(), isDrawSelected);
        float shownTime = time;
        if (document.keyDragging && selected &&
            std::all_of(keys.begin(), keys.end(), isDrawSelected))
            shownTime += document.keyDragDelta;
        const float x = timeX +
            (row.startSeconds + shownTime) * pixelsPerSecond;
        if (x < labelRight - 8.f || x > draw->GetClipRectMax().x + 8.f)
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
        if (collectAllKeyVisuals ||
            (collectHitRowVisuals &&
                std::abs(mouse.y - center) <= row.height * .5f))
            keyVisuals.push_back({ &row, time, x, center, keys });
    };
    using TimelineRow = AnimationDocument::TimelineRow;
    const auto rowIdentity = [](const Row& row)
    { return TimelineRow{row.track->index, row.clipIndex, row.nodeIndex}; };
    const auto sameRow = [](const TimelineRow& left,
        const TimelineRow& right)
    {
        return left.rigIndex == right.rigIndex &&
            left.clipIndex == right.clipIndex &&
            left.nodeIndex == right.nodeIndex;
    };
    const auto selectedRow = [&](const Row& row)
    {
        const auto identity = rowIdentity(row);
        return std::any_of(document.selectedRows.begin(),
            document.selectedRows.end(), [&](const auto& selected)
            { return sameRow(identity, selected); });
    };
    const auto selectRow = [&](const Row& row, bool toggle, bool range)
    {
        const auto identity = rowIdentity(row);
        if (!document.selectedRows.empty() &&
            document.selectedRows.front().rigIndex != identity.rigIndex)
        {
            document.selectedRows.clear();
            document.rowSelectionAnchor.reset();
        }
        if (range && document.rowSelectionAnchor &&
            document.rowSelectionAnchor->rigIndex == identity.rigIndex)
        {
            const auto locate = [&](const TimelineRow& target)
            {
                for (size_t i = 0; i < rows.size(); ++i)
                    if (rows[i].kind == Row::Kind::Bone &&
                        sameRow(rowIdentity(rows[i]), target)) return i;
                return rows.size();
            };
            const size_t first = locate(*document.rowSelectionAnchor);
            const size_t last = locate(identity);
            if (first < rows.size() && last < rows.size())
            {
                if (!toggle) document.selectedRows.clear();
                for (size_t i = std::min(first, last);
                    i <= std::max(first, last); ++i)
                {
                    if (rows[i].kind != Row::Kind::Bone ||
                        rows[i].track->index != identity.rigIndex) continue;
                    const auto candidate = rowIdentity(rows[i]);
                    if (std::none_of(document.selectedRows.begin(),
                            document.selectedRows.end(), [&](const auto& selected)
                            { return sameRow(candidate, selected); }))
                        document.selectedRows.push_back(candidate);
                }
                return;
            }
        }
        if (toggle)
        {
            auto found = std::find_if(document.selectedRows.begin(),
                document.selectedRows.end(), [&](const auto& selected)
                { return sameRow(identity, selected); });
            if (found == document.selectedRows.end())
                document.selectedRows.push_back(identity);
            else document.selectedRows.erase(found);
        }
        else
        {
            document.selectedRows.clear();
            document.selectedRows.push_back(identity);
        }
        document.rowSelectionAnchor = identity;
    };
    std::vector<TimelineKey> keyGroup;
    std::vector<TimelineKey> singleKey;
    singleKey.reserve(1);
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
            if (row.clip)
            {
                const float length = std::max(.01f, row.clip->duration);
                const int plays = row.repeat && row.repeatCount > 0
                    ? row.repeatCount : 1;
                const float barStart = timeX +
                    row.startSeconds * pixelsPerSecond;
                const float barEnd = timeX +
                    (row.startSeconds + length * plays) * pixelsPerSecond;
                draw->AddRectFilled({ barStart, bottom - 8.f },
                    { barEnd, bottom - 2.f }, color, 2.f);
                if (row.repeat && row.repeatCount == 0)
                    draw->AddText({ barEnd + 4.f, bottom - 16.f }, color,
                        "infinite");
                if (mouseInRow && mouse.y >= bottom - 9.f &&
                    mouse.x >= barStart && mouse.x <= barEnd &&
                    mouse.x >= labelRight)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    ImGui::SetTooltip("Drag to move animation start");
                    if (leftClicked)
                    {
                        document.clipDragging = true;
                        document.clipDragRig = row.track->index;
                        document.clipDragIndex = row.clipIndex;
                        document.clipDragMouseX = mouse.x;
                        document.clipDragStart = row.startSeconds;
                        document.playing = false;
                    }
                }
                if (document.clipDragging &&
                    document.clipDragRig == row.track->index &&
                    document.clipDragIndex == row.clipIndex)
                {
                    float& start = row.track->clipIndex == row.clipIndex
                        ? row.track->startSeconds
                        : std::find_if(row.track->stackedClips.begin(),
                            row.track->stackedClips.end(),
                            [&](const auto& layer)
                            { return layer.clipIndex == row.clipIndex; })
                            ->startSeconds;
                    const float moved = std::max(0.f,
                        document.clipDragStart +
                        (mouse.x - document.clipDragMouseX) /
                        pixelsPerSecond);
                    if (start != moved)
                    {
                        start = moved;
                        row.startSeconds = moved;
                        SampleAnimationDocument(document);
                    }
                }
            }
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
            if (row.summaryKeys && bottom >= visibleTop && top <= visibleBottom)
            {
                const float drag = document.keyDragging ? document.keyDragDelta : 0.f;
                const float firstTime = visibleStart - row.startSeconds -
                    8.f / pixelsPerSecond - std::max(0.f, drag);
                const float lastTime = visibleEnd - row.startSeconds +
                    8.f / pixelsPerSecond - std::min(0.f, drag);
                for (auto key = std::lower_bound(row.summaryKeys->begin(),
                         row.summaryKeys->end(), firstTime,
                         [](const auto& group, float time) { return group.time < time; });
                     key != row.summaryKeys->end() && key->time <= lastTime; ++key)
                {
                    keyGroup.clear();
                    keyGroup.reserve(key->keys.size());
                    for (const auto& [node, time] : key->keys)
                        keyGroup.push_back({row.track->index, row.clipIndex, node, time});
                    drawKey(row, center, key->time, keyGroup, color);
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
        const bool rowIsSelected = selectedRow(row);
        if (rowIsSelected || activeBone)
            draw->AddRectFilled({ origin.x, top },
                { origin.x + canvasWidth, bottom },
                ImGui::GetColorU32(RigTimelineTint(rigColor,
                    rowIsSelected ? .4f : .25f)));
        draw->AddLine({ origin.x, bottom },
            { origin.x + canvasWidth, bottom },
            IM_COL32(150, 160, 175, 28));

        const auto times = row.keyTimes
            ? row.keyTimes->find(row.nodeIndex)
            : std::unordered_map<unsigned, std::vector<float>>::const_iterator{};
        if (row.keyTimes && times != row.keyTimes->end())
        for (auto time = std::lower_bound(times->second.begin(),
                 times->second.end(), visibleStart - row.startSeconds -
                     8.f / pixelsPerSecond - std::max(0.f,
                         document.keyDragging ? document.keyDragDelta : 0.f));
             time != times->second.end() &&
                 *time <= visibleEnd - row.startSeconds + 8.f / pixelsPerSecond -
                     std::min(0.f, document.keyDragging ? document.keyDragDelta : 0.f); ++time)
        {
            singleKey.clear();
            singleKey.push_back({ row.track->index, row.clipIndex,
                row.nodeIndex, *time });
            drawKey(row, center, *time, singleKey, color);
        }
        draw->AddRectFilled({ pinnedX, top },
            { labelRight, bottom },
            rowIsSelected
                ? ImGui::GetColorU32(RigTimelineTint(rigColor, .5f))
                : ImGui::GetColorU32(ImGuiCol_WindowBg));
        draw->AddText({ pinnedX + 12.f, top + 4.f },
            activeBone || rowIsSelected ? color : ImGui::GetColorU32(ImGuiCol_Text),
            row.joint->name.c_str());
        if (mouseInRow && mouse.x < labelRight && leftClicked &&
            !document.playheadDragging)
        {
            activateClip(row);
            selectRow(row, ImGui::GetIO().KeyCtrl,
                ImGui::GetIO().KeyShift);
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
        !document.clipDragging &&
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
                if (!additive) selectRow(*hit->row, false, false);
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
            document.playheadSeconds = hit->row->startSeconds + hit->time;
            document.playing = false;
            SampleAnimationDocument(document);
        }
        else if (row && row->clip && row->mappingReady &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            activateClip(*row);
            document.playheadSeconds = hit
                ? hit->row->startSeconds + hit->time : timeAt(mouse.x);
            document.playing = false;
            SampleAnimationDocument(document);
            if (row->kind == Row::Kind::Bone && selectedRow(*row) &&
                document.selectedRows.size() > 1)
                AddAnimationRowKeys(document);
            else if (auto* model = row->track->poseSkeleton->ResolveModel())
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
                            AnimationLocalTime(document, row->track->index,
                                row->clipIndex), transform.position,
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
        else if (row && row->kind == Row::Kind::Bone)
        {
            activateClip(*row);
            selectRow(*row, ImGui::GetIO().KeyCtrl,
                ImGui::GetIO().KeyShift);
            document.selectedBone = static_cast<int>(row->boneIndex);
            document.poseScene->SetSelectedObject(row->joint);
            if (m_primaryHierarchy)
                m_primaryHierarchy->SetSelectedObject(row->joint);
            if (m_primaryProperties)
                m_primaryProperties->SetSelectedObject(row->joint);
            if (!additive) document.selectedKeys.clear();
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
        !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_C) &&
            !document.selectedKeys.empty())
            CopyAnimationKeys(document);
        if (ImGui::IsKeyPressed(ImGuiKey_V) &&
            !document.copiedKeys.empty())
            PasteAnimationKeys(document);
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
