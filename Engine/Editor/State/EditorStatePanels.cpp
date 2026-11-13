#include "pch.h"
#include "Engine/Editor/State/EditorState.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/SceneManager.h"
#include "Core/Rendering/RenderJobs.h"
#include "Engine/Editor/CodeEditorLink.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/View.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/SceneView.h"
#include "Core/View/Views/GameView.h"
#include "Core/View/Templates/EditModes/ViewportModeTemplate.h"
#include "Core/View/Views/AssetDocumentView.h"
#include "Core/View/Views/AssetsExplorerView.h"
#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Physics.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/PreparedPrefab.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Transform.h"
#include "Core/AssetRecord.h"
#include "Core/Importers/ModelImporter.h"

#ifndef ENGINE_ROOT_PATH
#define ENGINE_ROOT_PATH "."
#endif

#include "EditorStateSupport.h"

namespace Engine::Editor
{
using namespace StateSupport;

namespace
{
    bool ReplaceAllInText(std::string& text, const std::string& from,
        const std::string& to)
    {
        if (from.empty() || from == to)
            return false;
        bool replaced = false;
        size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos)
        {
            const size_t end = position + from.size();
            const bool embeddedBefore = position > 0 &&
                (std::isalnum(static_cast<unsigned char>(text[position - 1])) ||
                    text[position - 1] == '_');
            const bool embeddedAfter = end < text.size() &&
                (std::isalnum(static_cast<unsigned char>(text[end])) ||
                    text[end] == '_' || text[end] == '.' ||
                    text[end] == '-' || text[end] == ' ');
            if (embeddedBefore || embeddedAfter)
            {
                position = end;
                continue;
            }
            text.replace(position, from.size(), to);
            position += to.size();
            replaced = true;
        }
        return replaced;
    }

    bool ReplaceAssetStrings(Engine::Serialization::JsonValue& value,
        const std::vector<std::string>& from,
        const std::vector<std::string>& to)
    {
        if (value.IsString())
        {
            std::string text = value.AsString();
            bool changed = false;
            for (size_t i = 0; i < from.size(); ++i)
                changed = ReplaceAllInText(text, from[i], to[i]) || changed;
            if (changed) value = Engine::Serialization::JsonValue(std::move(text));
            return changed;
        }
        bool changed = false;
        if (value.IsArray())
            for (size_t i = 0; i < value.ArraySize(); ++i)
                changed = ReplaceAssetStrings(value.ArrayAt(i), from, to) || changed;
        else if (value.IsObject())
            for (size_t i = 0; i < value.ObjectSize(); ++i)
                changed = ReplaceAssetStrings(value[value.ObjectKey(i)], from, to) || changed;
        return changed;
    }

    std::string RemapPathPrefix(const std::string& value,
        const std::string& oldPrefix, const std::string& newPrefix)
    {
        if (value == oldPrefix)
            return newPrefix;
        if (value.size() <= oldPrefix.size() ||
            value.compare(0, oldPrefix.size(), oldPrefix) != 0)
            return value;

        const char separator = value[oldPrefix.size()];
        if (separator != '/' && separator != '\\')
            return value;

        std::string remapped = newPrefix;
        remapped += value.substr(oldPrefix.size());
        return remapped;
    }}

// ---------------------------------------------------------------------------
// EditorState::InitializePanels
// ---------------------------------------------------------------------------
void EditorState::InitializePanels()
{
    OutputDebugStringA("[EditorState::InitializePanels] Creating preferences view\n");
    m_preferences = std::make_unique<PreferencesView>();

    OutputDebugStringA("[EditorState::InitializePanels] Calling preferences->Init\n");
    m_preferences->Init(m_projectSettings, m_projectFilePath);
    m_preferences->SetSpatialDebugVisuals(
        m_scene && m_scene->settings.portalDebugVisuals);
    m_preferences->OnSettingsChanged = [this]() {
        m_projectSettings = m_preferences->GetSettings();
        if (m_primaryAssets)
            m_primaryAssets->SetCodeEditor(m_projectSettings.codeEditor);
        if (m_scene)
        {
            m_scene->GetPhysics().ConfigureStepBudget(
                m_projectSettings.physicsTargetMilliseconds,
                m_projectSettings.physicsMaximumSubsteps);
            m_scene->SetRealtimeShadowSettings(
                m_projectSettings.realtimeShadows);
            m_scene->SetDistanceLightingSettings(
                m_projectSettings.distanceLighting);
        }
        SetHistoryLimit(m_projectSettings.editorHistoryLimit);
        for (auto& panel : m_panels)
            if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
                hierarchy->SetDebugInteractionLogging(
                    m_projectSettings.debugHierarchyInteractions);
    };
    m_preferences->OnSpatialDebugVisualsChanged = [this](bool enabled) {
        if (!m_scene)
            return;
        m_scene->settings.portalDebugVisuals = enabled;
        MarkSceneEdited();
        if (m_renderer)
            m_renderer->MarkDirty();
    };

    OutputDebugStringA("[EditorState::InitializePanels] Checking view factory\n");
    if (m_viewFactory)
    {
        OutputDebugStringA("[EditorState::InitializePanels] Creating Scene view\n");
        auto scenePanel = m_viewFactory->Create("Scene");
        if (scenePanel)
        {
            if (auto* mainSceneView = dynamic_cast<SceneView*>(scenePanel.get()))
            {
                mainSceneView->UseGlobalToolbar = true;
                mainSceneView->OnMeshViewportInput = [this](IEditorUi& ui,
                    const EditorUiViewportInput& input, EditorTransformTool tool)
                {
                    return m_mainSkeletonEdit.enabled
                        ? HandleSkeletonViewport(ui, input, m_scene.get(),
                            m_mainSkeletonEdit)
                        : HandleMeshViewport(ui, input, m_scene.get(),
                            m_mainMeshEdit, tool);
                };
                mainSceneView->OnDrawDocumentTools = [this, mainSceneView](IEditorUi& ui)
                {
                    if (!ui.BeginViewportHeader("##SceneEditMode", 230.f))
                        return false;
                    int mode = m_mainSkeletonEdit.enabled ? 2
                        : m_mainMeshEdit.enabled ? 1
                        : mainSceneView->GetEditMode() == SceneEditMode::Terrain ? 3 : 0;
                    const ViewportModeControl controls = DrawViewportModeControl(ui, mode);
                    if (controls.changed)
                    {
                        SetActiveSceneAssetDocument(nullptr);
                        SetPrefabDocumentFocused(false);
                        SetSkeletonEditMode(m_scene.get(), m_mainSkeletonEdit,
                            m_mainMeshEdit, false);
                        SetMeshEditMode(m_scene.get(), m_mainMeshEdit, mode == 1);
                        if (mode == 2)
                            SetSkeletonEditMode(m_scene.get(), m_mainSkeletonEdit,
                                m_mainMeshEdit, true);
                        if (mode == 3)
                            mainSceneView->SetEditMode(SceneEditMode::Terrain);
                    }
                    if (m_mainMeshEdit.enabled)
                        SyncMeshEditSelection(m_scene.get(), m_mainMeshEdit);
                    if (m_mainSkeletonEdit.enabled)
                        SyncSkeletonEditSelection(m_scene.get(), m_mainSkeletonEdit);
                    if (controls.showSave)
                    {
                        ui.SameLine();
                        if (ui.Button("Save")) SaveScene();
                    }
                    const bool consumed = ui.EndViewportHeader();
                    return consumed;
                };
            }
            scenePanel->OnFocused = [this]()
            {
                m_gameViewFocused = false;
                SetActiveSceneAssetDocument(nullptr);
                SetPrefabDocumentFocused(false);
            };
            m_panels.push_back(std::move(scenePanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Scene panel is null\n");

        OutputDebugStringA("[EditorState::InitializePanels] Creating Game view\n");
        auto gamePanel = m_viewFactory->Create("Game");
        if (gamePanel)
        {
            gamePanel->OnFocused = [this]() { m_gameViewFocused = true; };
            m_panels.push_back(std::move(gamePanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Game panel is null\n");

        OutputDebugStringA("[EditorState::InitializePanels] Creating Hierarchy view\n");
        auto hierarchyPanel = m_viewFactory->Create("Hierarchy");
        if (hierarchyPanel)
        {
            if (auto* hierarchy = dynamic_cast<HierarchyView*>(hierarchyPanel.get()))
            {
                m_primaryHierarchy = hierarchy;
                hierarchy->OnPrefabRequested = [this](const std::string& prefabPath)
                {
                    QueueSceneAssetDocumentOpen(prefabPath);
                };
            }
            m_panels.push_back(std::move(hierarchyPanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Hierarchy panel is null\n");

        OutputDebugStringA("[EditorState::InitializePanels] Creating Properties view\n");
        auto properties = m_viewFactory->Create("Properties");
        if (properties)
        {
            OutputDebugStringA("[EditorState::InitializePanels] Storing properties pointer\n");
            m_primaryProperties = static_cast<PropertiesView*>(properties.get());
            m_panels.push_back(std::move(properties));
        }

        OutputDebugStringA("[EditorState::InitializePanels] Creating Assets view\n");
        auto assets = m_viewFactory->Create("Assets");
        if (assets)
        {
            m_primaryAssets = static_cast<AssetsExplorerView*>(assets.get());
            m_primaryAssets->SetCodeEditor(m_projectSettings.codeEditor);
            m_primaryAssets->OnCodeFileOpened = [this](const std::string& file) {
                const std::filesystem::path workspace = m_projectFilePath.empty()
                    ? std::filesystem::path(ENGINE_ROOT_PATH)
                    : std::filesystem::path(m_projectFilePath).parent_path();
                RememberCodeFile(workspace, file);
            };
            m_primaryAssets->OnObjectImportRequested =
                [this](const std::string& source, const std::string& directory)
                { StartObjectImport(source, false, directory); };
            m_panels.push_back(std::move(assets));
        }

        OutputDebugStringA("[EditorState::InitializePanels] Creating Console view\n");
        auto console = m_viewFactory->Create("Console");
        if (console)
        {
            OutputDebugStringA("[EditorState::InitializePanels] Storing console pointer\n");
            m_primaryConsole = static_cast<ConsoleView*>(console.get());
            m_panels.push_back(std::move(console));
        }

        OutputDebugStringA("[EditorState::InitializePanels] Creating Problems view\n");
        auto problems = m_viewFactory->Create("Problems");
        if (problems)
            m_panels.push_back(std::move(problems));

        OutputDebugStringA("[EditorState::InitializePanels] Creating Terminal view\n");
        auto terminal = m_viewFactory->Create("Terminal");
        if (terminal)
            m_panels.push_back(std::move(terminal));
        if (auto timeline = m_viewFactory->Create("Animation Timeline"))
            m_panels.push_back(std::move(timeline));
    }
    RefreshSceneDocumentTitle();
    OutputDebugStringA("[EditorState::InitializePanels] Complete\n");
}

// ---------------------------------------------------------------------------
// EditorState::WireupCallbacks
// ---------------------------------------------------------------------------
void EditorState::WireupCallbacks()
{
    if (!m_viewFactory)
        return;
    m_viewFactory->OnDrawAnimationTimeline = [this](IEditorUi& ui)
    { DrawAnimationTimelinePanel(ui); };
    m_viewFactory->OnMainDocumentFocused = [this]()
    {
        // Hierarchy and Properties are shared panels. Their focus alone must
        // not switch the active asset document.
    };

    // Wire up scene loading callback
    m_viewFactory->OnSceneRequested = [this](const std::string& scenePath) {
        QueueSceneAssetDocumentOpen(scenePath);
    };
    m_viewFactory->OnPrefabRequested = [this](const std::string& prefabPath) {
        QueueSceneAssetDocumentOpen(prefabPath);
    };
    m_viewFactory->OnAssetDocumentRequested = [this](const std::string& path,
        const std::string& editorType) {
        QueueAssetDocumentOpen(path, editorType);
    };

    m_viewFactory->OnAssetSelected = [this](const std::string& assetPath) {
        if (m_primaryHierarchy)
            m_primaryHierarchy->SetSelectedObject(nullptr);
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(nullptr);
        if (m_primaryProperties)
        {
            m_primaryProperties->SetSelectedObject(nullptr);
            m_primaryProperties->SetSelectedAsset(assetPath);
        }
        if (m_primaryAssets &&
            m_primaryAssets->GetSelectedPath() != assetPath)
            m_primaryAssets->SetSelectedPath(assetPath);
        MarkHistorySelectionChanged();
    };
    m_viewFactory->OnOpenAnimation = [this](const std::string& path)
    { QueueAnimationAssetOpen(path); };
    m_viewFactory->OnEditObjectAnimation = [this](Engine::Core::Object* object,
        const std::string& clipName) { QueueAnimationObjectOpen(object, clipName); };

    m_viewFactory->OnAssetRenamed = [this](const std::string& oldPath,
        const std::string& newPath) {
        const std::string oldNormalized = NormalizeAssetPath(oldPath);
        const std::string newNormalized = NormalizeAssetPath(newPath);
        if (m_primaryAssets && !m_primaryAssets->GetSelectedPath().empty())
        {
            const std::string selected = NormalizeAssetPath(
                m_primaryAssets->GetSelectedPath());
            const std::string remapped = RemapPathPrefix(selected,
                oldNormalized, newNormalized);
            if (remapped != selected)
                m_primaryAssets->SetSelectedPath(remapped);
        }

        if (m_primaryProperties &&
            !m_primaryProperties->GetSelectedAsset().empty())
        {
            const std::string selected = NormalizeAssetPath(
                m_primaryProperties->GetSelectedAsset());
            const std::string remapped = RemapPathPrefix(selected,
                oldNormalized, newNormalized);
            if (remapped != selected)
                m_primaryProperties->SetSelectedAsset(remapped);
        }
        for (AssetDocumentView* document : m_assetDocuments)
            if (document)
            {
                const std::string path = NormalizeAssetPath(document->GetPath());
                const std::string remapped = RemapPathPrefix(path,
                    oldNormalized, newNormalized);
                if (remapped != path)
                    document->HandleExternalRename(remapped);
            }

        const auto remapPath = [&](const std::string& value)
        {
            const std::string normalized = NormalizeAssetPath(value);
            const std::string remapped = RemapPathPrefix(normalized,
                oldNormalized, newNormalized);
            if (remapped != normalized)
                return remapped;
            return normalized == oldNormalized ? newNormalized : value;
        };

        bool sceneChanged = false;
        if (m_scene)
        {
            std::function<void(Engine::Core::Object*)> updatePrefab =
                [&](Engine::Core::Object* object) {
                if (!object)
                    return;
                if (object->Prefab)
                {
                    const std::string remapped =
                        remapPath(object->Prefab->GetPath());
                    if (remapped != object->Prefab->GetPath())
                    {
                        object->SetPrefab(remapped);
                        sceneChanged = true;
                    }
                }
                for (Engine::Core::Object* child : object->Children)
                    updatePrefab(child);
            };
            for (const auto& object : m_scene->GetObjects())
                if (object && !object->Parent)
                    updatePrefab(object.get());
        }

        if (!m_currentScenePath.empty())
            m_currentScenePath = remapPath(m_currentScenePath);
        RefreshSceneDocumentTitle();
        if (!m_activePrefabPath.empty())
            m_activePrefabPath = remapPath(m_activePrefabPath);

        bool assetReferenceChanged = false;
        std::error_code error;
        std::filesystem::path assetsDirectory =
            m_projectSettings.assetsDirectory.empty()
            ? std::filesystem::path("Assets")
            : std::filesystem::path(m_projectSettings.assetsDirectory);
        assetsDirectory = std::filesystem::weakly_canonical(assetsDirectory,
            error);
        if (!error && std::filesystem::exists(assetsDirectory))
        {
            std::vector<std::string> oldCandidates = {
                oldNormalized,
                std::filesystem::path(oldNormalized).make_preferred().string()
            };
            std::vector<std::string> newCandidates = {
                newNormalized,
                std::filesystem::path(newNormalized).make_preferred().string()
            };
            const auto oldRelative = std::filesystem::path(oldNormalized)
                .lexically_relative(assetsDirectory.parent_path());
            const auto newRelative = std::filesystem::path(newNormalized)
                .lexically_relative(assetsDirectory.parent_path());
            if (!oldRelative.empty() && !newRelative.empty() &&
                *oldRelative.begin() != ".." &&
                *newRelative.begin() != "..")
            {
                oldCandidates.push_back(oldRelative.generic_string());
                newCandidates.push_back(newRelative.generic_string());
                oldCandidates.push_back(std::filesystem::path(oldRelative)
                    .make_preferred().string());
                newCandidates.push_back(std::filesystem::path(newRelative)
                    .make_preferred().string());
            }

            for (const auto& entry :
                std::filesystem::recursive_directory_iterator(assetsDirectory,
                    error))
            {
                if (error)
                    break;
                if (!entry.is_regular_file())
                    continue;
                const std::string extension =
                    entry.path().extension().string();
                if (extension == ".meta")
                    continue;
                const std::string lowerExtension = [&]() {
                    std::string lower = extension;
                    std::transform(lower.begin(), lower.end(),
                        lower.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                    return lower;
                }();
                if (lowerExtension != ".scene" &&
                    lowerExtension != ".prefab" &&
                    lowerExtension != ".rig" &&
                    lowerExtension != ".anim" &&
                    lowerExtension != ".animation" &&
                    lowerExtension != ".material" &&
                    lowerExtension != ".spriteanim" &&
                    lowerExtension != ".spritesheet" &&
                    lowerExtension != ".xml")
                    continue;

                std::ifstream input(entry.path(), std::ios::binary);
                if (!input)
                    continue;
                std::string contents((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
                input.close();

                bool changed = false;
                const bool binary = contents.size() >= 5 &&
                    contents.compare(0, 4, "EBIN") == 0;
                if (binary)
                {
                    try
                    {
                        auto asset = Engine::Serialization::AssetParseFile(
                            entry.path().string());
                        changed = ReplaceAssetStrings(asset, oldCandidates,
                            newCandidates);
                        if (changed)
                            contents = Engine::Serialization::AssetWriteBinary(asset);
                    }
                    catch (const std::exception& error)
                    {
                        if (m_primaryConsole)
                            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                                "Could not update binary asset references: " +
                                entry.path().string() + " (" + error.what() + ")");
                        continue;
                    }
                }
                else
                    for (size_t index = 0; index < oldCandidates.size(); ++index)
                        changed = ReplaceAllInText(contents, oldCandidates[index],
                            newCandidates[index]) || changed;
                if (!changed)
                    continue;

                std::ofstream output(entry.path(),
                    std::ios::binary | std::ios::trunc);
                if (!output)
                    continue;
                output << contents;
                if (output.good())
                    assetReferenceChanged = true;
            }
        }

        if (sceneChanged || assetReferenceChanged)
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
        if (m_primaryAssets)
            m_primaryAssets->RecordKnownMove(oldPath, newPath);
    };

    m_viewFactory->OnAssetContentsChanged = [this](const std::string&) {
        auto refresh = [this](Engine::Scene::Scene* scene)
        {
            if (!scene) return;
            const auto snapshot = Engine::Serialization::SceneSerializer::CaptureSceneDocument(*scene);
            ::Engine::Scene::Scene::ObjectPath selectedPath;
            const bool hadSelection = scene->GetSelectedObject() &&
                scene->TryGetObjectPath(scene->GetSelectedObject(), selectedPath);
            if (!scene->LoadFromValue(snapshot) && m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not refresh a scene after asset contents changed.");
            if (hadSelection)
                if (Engine::Core::Object* selected = scene->FindObjectByPath(selectedPath))
                    scene->SetSelectedObject(selected);
            if (scene == GetActiveDocumentScene())
            {
                Engine::Core::Object* selected = scene->GetSelectedObject();
                if (m_primaryHierarchy)
                    m_primaryHierarchy->SetSelectedObject(selected);
                if (m_primaryProperties)
                    m_primaryProperties->SetSelectedObject(selected);
            }
        };
        refresh(m_scene.get());
        refresh(m_prefabScene.get());
        for (const auto& document : m_sceneAssetDocuments)
            if (document && !document->meshStage && !document->skeletonStage)
                refresh(document->scene.get());
    };

    m_viewFactory->OnPropertiesRequested = [this](Engine::Core::Object* obj) {
        if (m_primaryProperties)
        {
            m_primaryProperties->SetSelectedObject(obj);
            m_primaryProperties->SetOpen(true);
        }
    };

    // Wire up selection changed callback (for hierarchy -> properties)
    m_viewFactory->OnSelectionChanged = [this](Engine::Core::Object* obj) {
        if (Engine::Core::Component::CaptureEditorReferencePick(obj))
        {
            if (m_primaryProperties && m_primaryProperties->OnComponentsChanged)
                m_primaryProperties->OnComponentsChanged();
            return;
        }
        OutputDebugStringA(("[EditorState] Selection changed to: " + (obj ? obj->name : "nullptr") + "\n").c_str());
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(obj);
        CaptureImportReference(obj);
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath({});
        if (m_primaryProperties)
        {
            m_primaryProperties->SetSelectedObject(obj);
            OutputDebugStringA("[EditorState] Updated properties view\n");
        }
        else
        {
            OutputDebugStringA("[EditorState] WARNING: Properties view is null\n");
        }
        MarkHistorySelectionChanged();
    };
    m_viewFactory->OnSelectionSetChanged = [this](
        const std::vector<Engine::Core::Object*>& objects) {
        Engine::Core::Object* active = objects.empty() ? nullptr : objects.back();
        if (Engine::Core::Component::CaptureEditorReferencePick(active))
        {
            if (m_primaryProperties && m_primaryProperties->OnComponentsChanged)
                m_primaryProperties->OnComponentsChanged();
            return;
        }
        CaptureImportReference(active);
        OutputDebugStringA(("[EditorState] Selection set changed: " +
            std::to_string(objects.size()) + " object(s)\n").c_str());
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(active);
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath({});
        if (m_primaryProperties)
            m_primaryProperties->SetSelectedObjects(objects);
        MarkHistorySelectionChanged();
    };

    // Scene viewport click-selection -> hierarchy/properties + render selection state
    m_viewFactory->OnObjectSelected = [this](Engine::Core::Object* obj) {
        if (Engine::Core::Component::CaptureEditorReferencePick(obj))
        {
            if (m_primaryProperties && m_primaryProperties->OnComponentsChanged)
                m_primaryProperties->OnComponentsChanged();
            return;
        }
        if (m_primaryHierarchy)
            m_primaryHierarchy->SetSelectedObject(obj);
        if (m_primaryProperties)
            m_primaryProperties->SetSelectedObject(obj);
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath({});
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(obj);
        MarkHistorySelectionChanged();
    };
    m_viewFactory->OnObjectCreated = [this](Engine::Core::Object* obj) {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
        SelectObject(obj);
    };
    m_viewFactory->OnDeleteSelectionRequested = [this]() {
        // A selected terrain placement is a record in its layer, not an
        // object: delete the record and drop the selection proxy with it.
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            if (auto* selected = scene->GetSelectedObject())
                if (auto* handle = selected->GetComponent<
                        Engine::Components::TerrainPlacementHandle>())
                {
                    if (handle->RemovePlacement())
                    {
                        m_hasUnsavedChanges = true;
                        MarkSceneEdited();
                    }
                    SelectObject(nullptr);
                    return;
                }
        if (m_prefabDocumentFocused)
        {
            if (m_primaryHierarchy)
                m_primaryHierarchy->RequestDeleteSelectedObject();
            return;
        }
        if (m_primaryHierarchy)
            m_primaryHierarchy->RequestDeleteSelectedObject();
    };
    m_viewFactory->OnGizmoInteraction = [this](bool active) {
        ReportSceneEditInProgress(active);
        // Imported skeleton joints usually live below a linked-prefab root.
        // Invalidating that root on every mouse move makes the Properties view
        // serialize and diff the complete model hierarchy once per frame.  The
        // final transform is all the override cache needs, so defer its single
        // invalidation until the gizmo interaction ends.
        if (active)
        {
            m_mainSceneGizmoWasActive = true;
        }
        else if (m_mainSceneGizmoWasActive)
        {
            m_mainSceneGizmoWasActive = false;
            if (m_scene)
                if (Engine::Core::Object* selected = m_scene->GetSelectedObject();
                    selected && selected != selected->GetPrefabInstanceRoot())
                    selected->InvalidatePrefabOverrideCache();
        }
    };

    // Wire up focus (double-click) callback â€” frame the object in the scene camera
    m_viewFactory->OnFocusObject = [this](Engine::Core::Object* obj) {
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->FocusEditorCamera(obj);
    };
    m_viewFactory->OnHierarchyChanged = [this]() {
        if (m_activeAnimationDocument)
        {
            if (m_renderer) m_renderer->MarkDirty();
        }
        else if (m_activeSceneAssetDocument)
        {
            SceneAssetDocument& document = *m_activeSceneAssetDocument;
            if (document.objectStage || document.skeletonStage)
            {
                std::unordered_set<Engine::Core::Object*> liveObjects;
                for (const auto& object : document.scene->GetObjects())
                    if (object)
                    {
                        liveObjects.insert(object.get());
                        const bool inFocusGraph = document.objectStage && [&]()
                        {
                            for (Engine::Core::Object* current = object.get();
                                current; current = current->Parent)
                                if (current == document.subject) return true;
                            return false;
                        }();
                        const bool editableSubject =
                            document.selectableObjects.find(object.get()) !=
                                document.selectableObjects.end() || inFocusGraph;
                        if (!editableSubject) continue;
                        auto state = document.originalEnabledState.find(object.get());
                        if (state == document.originalEnabledState.end())
                            document.originalEnabledState.emplace(
                                object.get(), object->enabled);
                        else if (document.skeletonStage ||
                            (document.objectStage && document.showChildHierarchy) ||
                            object.get() == document.subject)
                            state->second = object->enabled;
                    }
                for (auto state = document.originalEnabledState.begin();
                    state != document.originalEnabledState.end();)
                    if (liveObjects.find(state->first) == liveObjects.end())
                        state = document.originalEnabledState.erase(state);
                    else
                        ++state;
                if (document.objectStage)
                    ApplyObjectStageVisibility(document);
            }
            m_renderer->MarkDirty();
        }
        else if (m_prefabDocumentFocused)
            SetPrefabDirty(true);
        else
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
    };
    m_viewFactory->OnPropertiesChanged = [this]() {
        if (m_activeAnimationDocument)
            m_renderer->MarkDirty();
        else if (m_activeSceneAssetDocument)
            m_renderer->MarkDirty();
        else if (m_prefabDocumentFocused)
            SetPrefabDirty(true);
        else
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
    };
    m_viewFactory->OnPropertiesAssetDropLog = [this](const std::string& message, bool error) {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(error ? ConsoleView::Level::Error : ConsoleView::Level::Info,
                message);
    };
    m_viewFactory->OnHierarchyInteraction = [this](const std::string& message) {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info, message);
    };

    m_viewFactory->OnAssetDropped = [this](const std::string& path) {
        SelectObject(InstantiateAsset(path));
    };
    m_viewFactory->OnAssetAssigned = [this](const std::string& path,
        Engine::Core::Object* target) {
        if (!target) return;
        std::string message;
        const bool assigned = PropertiesView::AssignAssetToObject(
            *target, path, message);
        if (m_primaryConsole)
            m_primaryConsole->AddLog(assigned ? ConsoleView::Level::Info :
                ConsoleView::Level::Error, message);
        if (assigned)
        {
            SelectObject(target);
            if (m_primaryProperties && m_primaryProperties->OnComponentsChanged)
                m_primaryProperties->OnComponentsChanged();
        }
    };
    m_viewFactory->OnAssetPreviewRequested = [this](const std::string& path) {
        m_assetPreviewActive = true;
        Engine::Core::Object* preview = InstantiateAsset(path, false, false);
        m_assetPreviewActive = preview != nullptr;
        return preview;
    };
    m_viewFactory->OnAssetPreviewCancelled = [this](Engine::Core::Object* object) {
        if (object && object->GetScene())
        {
            // The current command list can still reference the preview. Remove
            // it at the next scene commit, after this frame is submitted.
            object->runtimeOnly = true;
            object->GetScene()->RequestRemoveObject(object);
        }
        m_assetPreviewActive = false;
    };
    m_viewFactory->OnPreparedAssetPreviewRequested = [this](Engine::Scene::Scene* scene,
        Engine::Serialization::PreparedPrefab& prepared) -> Engine::Core::Object* {
        if (!scene) return nullptr;
        Engine::Serialization::PreparedPrefab::Publication publication(prepared);
        auto* preview = Engine::Serialization::SceneSerializer::InstantiatePrefabFromValue(
            *scene, prepared.path, prepared.document, scene->GetGraphicsProvider());
        if (preview) scene->QueueGraphicsPreparation(2.0);
        m_assetPreviewActive = preview != nullptr;
        return preview;
    };
    m_viewFactory->OnAssetPreviewCommitted = [this](Engine::Core::Object* object,
        const std::string& path) {
        m_assetPreviewActive = false;
        if (!object)
            return;
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Placed prefab in scene: " + path);
        SelectObject(object);
    };

    m_viewFactory->OnPrefabCreated = [this](Engine::Core::Object*, const std::string& path) {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info, "Prefab created: " + path);
    };

    if (m_window)
        m_window->OnFilesDropped = [this](const std::vector<std::string>& paths,
            int dropX, int dropY) {
            const bool overAssets = m_primaryAssets &&
                m_primaryAssets->ContainsScreenPoint(static_cast<float>(dropX),
                    static_cast<float>(dropY));
            bool overScene = false;
            for (const auto& panel : m_panels)
                if (auto* view = dynamic_cast<SceneView*>(panel.get());
                    view && view->GetScene() == GetActiveDocumentScene() &&
                    view->ContainsScreenPoint(static_cast<float>(dropX),
                        static_cast<float>(dropY)))
                    overScene = true;
            for (const std::string& path : paths)
            {
                std::error_code pathError;
                if (overAssets || std::filesystem::is_directory(path, pathError))
                {
                    if (m_primaryAssets)
                    {
                        const std::string assetsDirectory =
                            m_projectSettings.assetsDirectory.empty()
                            ? "Assets" : m_projectSettings.assetsDirectory;
                        m_primaryAssets->CopyExternalPath(path,
                            overAssets ? std::string() :
                                assetsDirectory);
                    }
                    continue;
                }
                std::string extension = std::filesystem::path(path).extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                    [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                if (extension == ".vmd" || extension == ".fbx" || extension == ".pmx" ||
                    extension == ".gltf" || extension == ".glb" ||
                    extension == ".obj" || extension == ".spriteanim" ||
                    extension == ".prefab")
                {
                    StartObjectImport(path, overScene);
                    continue;
                }
                StartObjectImport(path, overScene);
            }
        };
}

}

