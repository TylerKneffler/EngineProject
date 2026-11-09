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

void EditorState::OpenAnimationEditor()
{
    QueueAnimationDocumentOpen("", 0);
}

void EditorState::QueueAnimationObjectOpen(Engine::Core::Object* object,
    const std::string& clipName)
{
    const auto contains = [&](Engine::Scene::Scene* scene)
    {
        return scene && std::any_of(scene->GetObjects().begin(), scene->GetObjects().end(),
            [&](const auto& candidate) { return candidate.get() == object; });
    };
    bool live = contains(m_scene.get()) || contains(m_prefabScene.get());
    for (const auto& open : m_sceneAssetDocuments) live |= contains(open->scene.get());
    if (!live || !object) return;
    auto* root = object->GetPrefabInstanceRoot();
    if (!root)
    {
        auto* model = object->GetComponentInParent<Engine::Components::Model>();
        root = model ? model->Owner : object;
    }
    const auto rigs = AnimationEditorSource::Rigs(root);
    if (rigs.empty())
    {
        if (m_primaryConsole) m_primaryConsole->AddLog(ConsoleView::Level::Warning,
            "Drop an object with a skeleton and model into the Animation Editor.");
        return;
    }
    size_t ordinal = 0;
    if (auto* explicitRig = AnimationRigSelection::ExplicitRig(object))
        for (size_t i = 0; i < rigs.size(); ++i)
            if (rigs[i] == explicitRig) { ordinal = i; break; }
    const std::string key = "__SceneAnimation__" +
        std::to_string(reinterpret_cast<uintptr_t>(root));
    const auto identity = AnimationIdentity(key);
    if (!clipName.empty()) m_pendingAnimationClipSelections[identity] = clipName;
    const bool alreadyOpen = std::any_of(m_animationDocuments.begin(), m_animationDocuments.end(),
        [&](const auto& document) { return document->identity == identity; });
    if (!alreadyOpen && !m_pendingAnimationSources.count(key))
        m_pendingAnimationSources[key] = { AnimationEditorSource::Capture(*root),
            root->Prefab ? root->Prefab->GetPath() : "", root, "" };
    QueueAnimationDocumentOpen(key, ordinal);
}

void EditorState::HandleAnimationEditorDrop(IEditorUi& ui, AnimationDocument* target)
{
    const auto asset = ui.WindowDragDropTarget("ENGINE_ASSET_PATH");
    if (asset.data && asset.delivered && asset.size > 0)
    {
        const auto* bytes = static_cast<const char*>(asset.data);
        size_t size = 0;
        while (size < asset.size && bytes[size]) ++size;
        const std::string path(bytes, size);
        const auto extension = LowerText(std::filesystem::path(path).extension().string());
        if (extension == ".prefab")
        {
            m_animationDocumentOpenError.clear();
            QueueAnimationDocumentOpen(path, static_cast<size_t>(-1));
        }
        else if (extension == ".anim" || extension == ".animation" || extension == ".animpack")
        {
            if (!target) target = m_activeAnimationDocument ? m_activeAnimationDocument : m_timelineAnimationDocument;
            if (target && target->sourceManager)
                AddAnimationToDocument(*target, path);
            else QueueAnimationAssetOpen(path);
        }
    }
    const auto object = ui.WindowDragDropTarget("ENGINE_SCENE_OBJECT");
    if (object.data && object.delivered && object.size == sizeof(Engine::Core::Object*))
        QueueAnimationObjectOpen(*static_cast<Engine::Core::Object* const*>(object.data));
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
        {
            if (track.clipIndex < 0) track.clipIndex = document.clipIndex;
            else track.stackedClips.push_back({document.clipIndex, 1.f});
            break;
        }
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
            document->view->RequestFocusOnNextDraw();
            if (auto pending = m_pendingAnimationClipAssets.find(identity);
                pending != m_pendingAnimationClipAssets.end())
            {
                if (const auto asset = Engine::Model::AnimationClipAsset::Load(pending->second))
                {
                    document->clipAssetPaths[asset->clip.clipName] = pending->second;
                    for (auto& track : document->tracks)
                        if (track.index == rigIndex && !track.sourceManager->FindClip(asset->clip.clipName))
                        {
                            std::string error;
                            if (track.sourceManager->AddAnimationAsset(pending->second, error))
                            {
                                track.sourceManager->InstallEmbeddedClips(track.sourceManager->clips);
                                track.poseManager->clips = track.sourceManager->clips;
                                ++document->clipRevision;
                                document->clipCache.clear();
                            }
                            else document->error = error;
                        }
                }
                m_pendingAnimationClipAssets.erase(pending);
            }
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

void EditorState::SaveAnimationEditorSession() const
{
    namespace fs = std::filesystem;
    const fs::path projectDirectory = m_projectFilePath.empty()
        ? fs::current_path() : fs::path(m_projectFilePath).parent_path();
    const fs::path file = projectDirectory / ".local" /
        "animation-editor-session.json";
    auto documents = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& document : m_animationDocuments)
        if (document && document->view && document->view->IsOpen() && !document->sourceObject)
            documents.Push(Engine::Serialization::JsonValue::MakeObject()
                .Set("path", Engine::Serialization::JsonValue(document->standaloneAssetPath.empty()
                    ? document->path : document->standaloneAssetPath))
                .Set("rigIndex", Engine::Serialization::JsonValue(
                    static_cast<int>(document->rigIndex)))
                .Set("active", Engine::Serialization::JsonValue(
                    document.get() == m_activeAnimationDocument)));
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    if (error) return;
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (output)
        output << Engine::Serialization::JsonWrite(
            Engine::Serialization::JsonValue::MakeObject()
                .Set("version", Engine::Serialization::JsonValue(1))
                .Set("documents", std::move(documents)));
}

void EditorState::RestoreAnimationEditorSession()
{
    namespace fs = std::filesystem;
    const fs::path projectDirectory = m_projectFilePath.empty()
        ? fs::current_path() : fs::path(m_projectFilePath).parent_path();
    const fs::path file = projectDirectory / ".local" /
        "animation-editor-session.json";
    if (!fs::exists(file)) return;
    try
    {
        const auto session = Engine::Serialization::JsonParseFile(file.string());
        if (session["version"].AsInt() != 1) return;
        const auto& documents = session["documents"];
        std::optional<std::pair<std::string, size_t>> active;
        for (size_t i = 0; i < std::min<size_t>(documents.ArraySize(), 16); ++i)
        {
            const auto& entry = documents.ArrayAt(i);
            if (!entry["path"].IsString()) continue;
            const std::string path = entry["path"].AsString();
            if (!path.empty() && !fs::exists(path)) continue;
            const int rig = entry["rigIndex"].AsInt();
            if (rig < 0 || rig > 1024) continue;
            if (entry["active"].AsBool())
                active = { path, static_cast<size_t>(rig) };
            else if (LowerText(fs::path(path).extension().string()) == ".anim" ||
                LowerText(fs::path(path).extension().string()) == ".animation")
                QueueAnimationAssetOpen(path);
            else QueueAnimationDocumentOpen(path, static_cast<size_t>(rig));
        }
        if (active)
        {
            const auto extension = LowerText(fs::path(active->first).extension().string());
            if (extension == ".anim" || extension == ".animation" || extension == ".animpack") QueueAnimationAssetOpen(active->first);
            else QueueAnimationDocumentOpen(active->first, active->second);
        }
        ProcessPendingAnimationDocumentOpens();
    }
    catch (const std::exception& error)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Could not restore Animation Editor session: " +
                std::string(error.what()));
    }
}

void EditorState::QueueAnimationAssetOpen(const std::string& path)
{
    if (std::filesystem::path(path).extension() == ".animpack")
    {
        std::string error;
        const auto pack = Engine::Model::AnimationPackAsset::Load(path, error);
        if (!pack) { if (m_primaryConsole) m_primaryConsole->AddLog(ConsoleView::Level::Error, error); return; }
        const std::string sourceKey = "__AnimationAsset__" + AnimationIdentity(path);
        try
        {
            if (std::none_of(m_animationDocuments.begin(), m_animationDocuments.end(),
                    [&](const auto& document) { return document->identity == AnimationIdentity(sourceKey); }))
                m_pendingAnimationSources[sourceKey] = {
                AnimationEditorSource::PackPreview(*pack), "", nullptr, path };
            QueueAnimationDocumentOpen(sourceKey, 0);
        }
        catch (const std::exception& exception)
        { if (m_primaryConsole) m_primaryConsole->AddLog(ConsoleView::Level::Error, exception.what()); }
        return;
    }

    const auto asset = Engine::Model::AnimationClipAsset::Load(path);
    if (!asset)
    {
        if (m_primaryConsole) m_primaryConsole->AddLog(ConsoleView::Level::Error,
            "Could not read animation asset: " + path);
        return;
    }
    // An animation is its own document. Preview-prefab metadata is only a hint
    // for mesh previews, never the identity or save destination of a clip.
    const std::string sourceKey = "__AnimationAsset__" + AnimationIdentity(path);
    constexpr size_t rigIndex = 0;
    try
    {
        const bool open = std::any_of(m_animationDocuments.begin(), m_animationDocuments.end(),
            [&](const auto& document) { return document->identity == AnimationIdentity(sourceKey); });
        if (!open && !m_pendingAnimationSources.count(sourceKey))
            m_pendingAnimationSources[sourceKey] = {
                AnimationEditorSource::ClipPreview(*asset), "", nullptr, path };
    }
    catch (const std::exception& error)
    {
        if (m_primaryConsole) m_primaryConsole->AddLog(ConsoleView::Level::Error, error.what());
        return;
    }
    m_pendingAnimationClipSelections[AnimationIdentity(sourceKey)] = asset->clip.clipName;
    m_pendingAnimationClipAssets[AnimationIdentity(sourceKey)] = path;
    QueueAnimationDocumentOpen(sourceKey, rigIndex);
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
                    track.stackedClips.erase(std::remove_if(
                        track.stackedClips.begin(), track.stackedClips.end(),
                        [&](const auto& layer) { return layer.clipIndex == static_cast<int>(index); }),
                        track.stackedClips.end());
                    if (document.rigIndex != rigIndex)
                        FocusAnimationRig(document, rigIndex);
                    document.clipIndex = track.clipIndex;
                    const auto& controllers = track.sourceManager->clips[
                        index].controllers;
                    document.controllerSpace = !controllers.empty() &&
                        !controllers.front().keys.empty()
                        ? controllers.front().keys.front().space
                        : Engine::Model::AnimationClip::ControllerKey::
                            Space::Body;
                    document.playheadSeconds = 0.f;
                    document.selectedKeys.clear();
                    SampleAnimationDocument(document);
                    return;
                }
    document.error = "Animation is not referenced by this rig: " + clipName;
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
    if (document) m_timelineAnimationDocument = document;
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
            game->SetScene(m_scene.get());
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


}
