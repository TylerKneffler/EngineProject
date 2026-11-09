#include "pch.h"
#include "EditorState.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/SceneManager.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/View.h"
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
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/PreparedPrefab.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Importers/ModelImporter.h"
#include "Core/Threading/JobScheduler.h"
#include "Engine/Editor/Core/View/Templates/Assets/AssetPreviewCache.h"

#include "EditorStateSupport.h"

namespace Engine::Editor
{
using namespace StateSupport;

void EditorState::OpenPrefabStage(const std::string& path)
{
    if (path.empty()) return;
    m_prefabReadPath = NormalizeAssetPath(path);
    m_prefabRead = Engine::Rendering::RenderJobs::Get().Submit(
        [path = m_prefabReadPath] {
            return Engine::Serialization::SceneSerializer::ReadPrefabFile(path);
        });
}

void EditorState::OpenPrefabStageWithDocument(const std::string& path,
    const Engine::Serialization::JsonValue& document)
{
    FrameActivity::Mark(FrameActivity::Load);
    if (!m_scene || !m_viewFactory || path.empty()) return;
    const std::string normalized = NormalizeAssetPath(path);
    if (normalized == m_activePrefabPath) return;
    if (!m_activePrefabPath.empty() && m_prefabHasUnsavedChanges)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Save the active prefab before opening another prefab.");
        return;
    }

    if (!m_activePrefabPath.empty())
        ClosePrefabStage();

    auto prefabScene = std::make_unique<Engine::Scene::Scene>();
    Engine::Core::Object* root = nullptr;
    try
    {
        prefabScene->SetEditorMode2D(m_scene->IsEditorMode2D());
        prefabScene->Init(m_renderer->GetGraphicsProvider());
        prefabScene->SetRealtimeShadowSettings(
            m_projectSettings.realtimeShadows);
        prefabScene->SetDistanceLightingSettings(
            m_projectSettings.distanceLighting);
        root = Engine::Serialization::SceneSerializer::InstantiatePrefabFromValue(
            *prefabScene, normalized, document, prefabScene->GetGraphicsProvider());
    }
    catch (const std::exception& error)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not open prefab editor: " + std::string(error.what()));
        return;
    }
    if (!root)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not open prefab stage: " + normalized);
        return;
    }

    // In the isolated stage this is the editable asset root, not an instance.
    root->Prefab.reset();
    const std::string prefabName =
        std::filesystem::path(normalized).filename().string();
    auto sceneView = m_viewFactory->CreateSceneView(
        prefabScene.get(), prefabName, EditorPanelDockArea::MainDocument);
    if (!sceneView)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not create prefab editor viewport: " + normalized);
        return;
    }

    m_prefabScene = std::move(prefabScene);
    m_prefabSceneView = sceneView.get();
    m_prefabSceneView->SetDocumentPath(normalized);
    m_prefabSceneView->SetTitle(prefabName + "###PrefabDocument:" + normalized);
    m_prefabSceneView->OnObjectSelected = [this](Engine::Core::Object* object)
    {
        if (Engine::Core::Component::CaptureEditorReferencePick(object))
        {
            SetPrefabDirty(true);
            if (m_primaryProperties && m_primaryProperties->OnComponentsChanged)
                m_primaryProperties->OnComponentsChanged();
            return;
        }
        if (m_prefabScene) m_prefabScene->SetSelectedObject(object);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
    };
    m_prefabSceneView->OnObjectCreated = [this](Engine::Core::Object* object)
    {
        SetPrefabDirty(true);
        if (m_prefabScene) m_prefabScene->SetSelectedObject(object);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
    };
    m_prefabSceneView->OnDeleteSelectionRequested = [this]()
    {
        if (m_primaryHierarchy)
            m_primaryHierarchy->RequestDeleteSelectedObject();
    };
    m_prefabSceneView->OnGizmoInteraction = [this](bool active)
    {
        if (active) SetPrefabDirty(true);
    };
    m_prefabSceneView->OnMeshViewportInput = [this](IEditorUi& ui,
        const EditorUiViewportInput& input, EditorTransformTool tool)
    {
        return m_prefabSkeletonEdit.enabled
            ? HandleSkeletonViewport(ui, input, m_prefabScene.get(),
                m_prefabSkeletonEdit)
            : HandleMeshViewport(ui, input, m_prefabScene.get(),
                m_prefabMeshEdit, tool);
    };
    m_prefabSceneView->UseGlobalToolbar = true;
    m_prefabSceneView->OnDrawDocumentTools = [this](IEditorUi& ui)
    {
        if (!ui.BeginViewportHeader("##PrefabEditMode", 230.f)) return false;
        int mode = m_prefabSkeletonEdit.enabled ? 2
            : m_prefabMeshEdit.enabled ? 1
            : m_prefabSceneView->GetEditMode() == SceneEditMode::Terrain ? 3 : 0;
        const ViewportModeControl controls = DrawViewportModeControl(ui, mode);
        if (controls.changed)
        {
            SetPrefabDocumentFocused(true);
            SetSkeletonEditMode(m_prefabScene.get(), m_prefabSkeletonEdit,
                m_prefabMeshEdit, false);
            SetMeshEditMode(m_prefabScene.get(), m_prefabMeshEdit, mode == 1);
            if (mode == 2)
                SetSkeletonEditMode(m_prefabScene.get(), m_prefabSkeletonEdit,
                    m_prefabMeshEdit, true);
            if (mode == 3)
                m_prefabSceneView->SetEditMode(SceneEditMode::Terrain);
        }
        if (m_prefabMeshEdit.enabled)
            SyncMeshEditSelection(m_prefabScene.get(), m_prefabMeshEdit);
        if (m_prefabSkeletonEdit.enabled)
            SyncSkeletonEditSelection(m_prefabScene.get(), m_prefabSkeletonEdit);
        if (controls.showSave)
        {
            ui.SameLine();
            if (ui.Button("Save")) SaveScene();
        }
        const bool consumed = ui.EndViewportHeader();
        return consumed;
    };
    sceneView->OnFocused = [this]()
    {
        m_gameViewFocused = false;
        SetPrefabDocumentFocused(true);
    };
    sceneView->RequestFocusOnNextDraw();
    m_panels.push_back(std::move(sceneView));
    m_activePrefabPath = normalized;
    SetPrefabDirty(false);
    RefreshPrefabDocumentTitle();
    m_prefabScene->SetSelectedObject(root);
    SetPrefabDocumentFocused(true);
    if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(root);
    if (m_primaryProperties) m_primaryProperties->SetSelectedObject(root);
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            "Opened prefab stage: " + normalized);
}

void EditorState::ProcessPendingPrefabStageOpen()
{
    if (m_pendingPrefabPath.empty())
        return;

    // Opening a prefab adds a Scene panel. Do it after the panel draw loop so
    // growing m_panels cannot invalidate the iterator currently drawing Assets.
    std::string path = std::move(m_pendingPrefabPath);
    m_pendingPrefabPath.clear();
    OpenPrefabStage(path);
}

void EditorState::QueueAssetDocumentOpen(const std::string& path,
    const std::string& editorType)
{
    if (path.empty())
        return;
    if (editorType == "Scene Editor")
    {
        QueueSceneAssetDocumentOpen(path);
        return;
    }
    if (editorType == "Object Editor")
    {
        QueueSceneAssetDocumentOpen(path);
        return;
    }
    if (editorType == "Mesh Editor")
    {
        QueueSceneAssetDocumentOpen(path);
        return;
    }
    if (editorType == "Model Editor")
    {
        QueueSceneAssetDocumentOpen(path);
        return;
    }
    if (editorType == "Animation Editor")
    {
        QueueAnimationAssetOpen(path);
        return;
    }
    const std::string identity = AssetPathIdentity(path);
    for (AssetDocumentView* document : m_assetDocuments)
        if (document && AssetPathIdentity(document->GetPath()) == identity)
        {
            document->SetOpen(true);
            SetActiveAssetDocument(document);
            return;
        }
    for (const auto& pending : m_pendingAssetDocuments)
        if (AssetPathIdentity(pending.first) == identity)
            return;
    m_pendingAssetDocuments.emplace_back(path, editorType);
}

void EditorState::QueueSceneAssetDocumentOpen(const std::string& path)
{
    if (path.empty())
        return;
    const std::string identity = AssetPathIdentity(path);
    // Scene files use the persistent Scene view; other scene-backed assets
    // keep separate document tabs.
    if (std::filesystem::path(identity).extension() != ".scene")
        for (const auto& document : m_sceneAssetDocuments)
            if (document && document->identity == identity)
            {
                document->view->SetOpen(true);
                SetActiveSceneAssetDocument(document.get());
                return;
            }
    for (const std::string& pending : m_pendingSceneAssetDocuments)
        if (AssetPathIdentity(pending) == identity)
            return;
    m_pendingSceneAssetDocuments.push_back(path);
}

void EditorState::ProcessPendingSceneAssetDocumentOpens()
{
    while (!m_pendingSceneAssetDocuments.empty())
    {
        std::string path = std::move(m_pendingSceneAssetDocuments.front());
        m_pendingSceneAssetDocuments.pop_front();
        const std::string normalized = NormalizeAssetPath(path);
        const std::string identity = AssetPathIdentity(normalized);
        std::string extension = std::filesystem::path(normalized).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension == ".scene")
        {
            if (m_currentScenePath.empty() ||
                AssetPathIdentity(m_currentScenePath) != identity)
                RequestSceneLoad(normalized);
            SetActiveSceneAssetDocument(nullptr);
            bool foundSceneView = false;
            for (auto& panel : m_panels)
                if (auto* sceneView = dynamic_cast<SceneView*>(panel.get());
                    sceneView && sceneView->GetScene() == m_scene.get())
                {
                    sceneView->SetOpen(true);
                    sceneView->RequestFocusOnNextDraw();
                    foundSceneView = true;
                    break;
                }
            if (!foundSceneView && m_viewFactory)
                if (auto sceneView = m_viewFactory->Create("Scene"))
                {
                    sceneView->OnFocused = [this]()
                    {
                        SetActiveSceneAssetDocument(nullptr);
                        SetPrefabDocumentFocused(false);
                    };
                    if (auto* createdSceneView = dynamic_cast<SceneView*>(
                        sceneView.get()))
                        createdSceneView->RequestFocusOnNextDraw();
                    m_panels.push_back(std::move(sceneView));
                    RefreshSceneDocumentTitle();
                }
            if (m_renderer) m_renderer->MarkDirty();
            continue;
        }
        auto existing = std::find_if(m_sceneAssetDocuments.begin(),
            m_sceneAssetDocuments.end(), [&](const auto& document)
            {
                return document && document->identity == identity;
            });
        if (existing != m_sceneAssetDocuments.end())
        {
            (*existing)->view->SetOpen(true);
            SetActiveSceneAssetDocument(existing->get());
            continue;
        }

        std::optional<Engine::Model::ModelImportResult> stageImport;
        if (ModelImporter::SupportsExtension(extension))
        {
            if (!m_stageImport.valid())
            {
                const std::string assetsDirectory =
                    m_projectSettings.assetsDirectory.empty()
                    ? std::string("Assets") : m_projectSettings.assetsDirectory;
                m_stageImportPath = normalized;
                m_stageImportControl = std::make_shared<AssetJobControl>();
                const auto control = m_stageImportControl;
                m_stageImport = Engine::Threading::JobScheduler::Get().Submit(
                    [normalized, assetsDirectory, control] {
                        AssetJobScope scope(control.get());
                        AssetJobCheckpoint(0.01f);
                        auto imported = ModelImporter::Import(
                            normalized, assetsDirectory);
                        if (control->cancelled.load(std::memory_order_acquire) &&
                            !imported.outputDirectory.empty())
                        {
                            std::error_code ignored;
                            std::filesystem::remove_all(
                                imported.outputDirectory, ignored);
                        }
                        AssetJobCheckpoint(1.f);
                        return imported;
                    });
                m_pendingSceneAssetDocuments.push_front(std::move(path));
                return;
            }
            if (m_stageImportPath != normalized ||
                m_stageImport.wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready)
            {
                m_pendingSceneAssetDocuments.push_front(std::move(path));
                return;
            }
            try { stageImport = m_stageImport.get(); }
            catch (const std::exception& error)
            {
                if (m_primaryConsole &&
                    !(m_stageImportControl && m_stageImportControl->cancelled.load()))
                    m_primaryConsole->AddLog(ConsoleView::Level::Error,
                        "Could not import model for Asset Stage: " +
                        std::string(error.what()));
            }
            const bool cancelled = m_stageImportControl &&
                m_stageImportControl->cancelled.load();
            if (cancelled && stageImport &&
                !stageImport->outputDirectory.empty())
            {
                const auto output = std::filesystem::path(
                    stageImport->outputDirectory);
                const auto assets = std::filesystem::path(
                    m_projectSettings.assetsDirectory.empty()
                        ? "Assets" : m_projectSettings.assetsDirectory);
                Engine::Threading::JobScheduler::Get().Submit(
                    [output, assets] {
                        const auto relative = output.lexically_relative(assets);
                        if (relative.empty() || *relative.begin() == "..") return;
                        std::error_code ignored;
                        std::filesystem::remove_all(output, ignored);
                    });
            }
            m_stageImportControl.reset();
            m_stageImportPath.clear();
            if (cancelled || !stageImport) continue;
        }

        auto document = std::make_unique<SceneAssetDocument>();
        document->path = normalized;
        document->identity = identity;
        document->prefab = extension == ".prefab";
        document->meshStage = extension == ".mesh" || extension == ".obj";
        document->modelStage = ModelImporter::SupportsExtension(extension);
        if (document->meshStage)
        {
            std::filesystem::path nativePath(normalized);
            nativePath.replace_extension(".mesh");
            document->meshSavePath = nativePath.string();
        }
        document->scene = std::make_unique<Engine::Scene::Scene>();
        document->scene->SetEditorMode2D(m_scene && m_scene->IsEditorMode2D());
        document->scene->Init(m_renderer->GetGraphicsProvider());
        document->scene->SetRealtimeShadowSettings(m_projectSettings.realtimeShadows);
        document->scene->SetDistanceLightingSettings(m_projectSettings.distanceLighting);
        bool loaded = false;
        if (document->meshStage)
        {
            Engine::Core::Object* subject = document->scene->AddObject(
                std::filesystem::path(normalized).stem().string());
            document->mesh = subject
                ? subject->AddComponent<Engine::Components::Mesh>() : nullptr;
            if (document->mesh)
            {
                document->mesh->LoadFromFile(normalized);
                if (document->scene->GetGraphicsProvider())
                    document->mesh->CreateBuffer(document->scene->
                        GetGraphicsProvider()->GetBufferFactory());
                subject->AddComponent<Engine::Components::Material>();
                document->subject = subject;
                document->scene->SetSelectedObject(subject);
                loaded = true;
            }
        }
        else if (document->modelStage)
        {
            const Engine::Model::ModelImportResult& imported = *stageImport;
            if (imported.success)
            {
                document->prefab = true;
                document->stageDataPath = NormalizeAssetPath(imported.prefabPath);
                Engine::Core::Object* root = Engine::Serialization::
                    SceneSerializer::InstantiatePrefab(*document->scene,
                        document->stageDataPath,
                        document->scene->GetGraphicsProvider());
                if (root)
                {
                    root->Prefab.reset();
                    document->subject = root;
                    document->scene->SetSelectedObject(root);
                    loaded = true;
                }
            }
            else if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not import model for Asset Stage: " + imported.message);
        }
        else if (document->prefab)
        {
            Engine::Core::Object* root = Engine::Serialization::SceneSerializer::
                InstantiatePrefab(*document->scene,
                    document->stageDataPath.empty() ? normalized : document->stageDataPath,
                    document->scene->GetGraphicsProvider());
            if (root)
            {
                root->Prefab.reset();
                document->subject = root;
                document->scene->SetSelectedObject(root);
                loaded = true;
            }
        }
        else
        {
            loaded = document->scene->Load(normalized);
        }
        // Prefab documents use the regular scene editing controls for now.
        // Keep the specialized object/skeleton stages for model assets only.
        if (loaded && document->prefab && document->modelStage)
        {
            RebuildSkeletonStageContext(*document);
            if (!document->skeletonStage)
                RebuildObjectStageContext(*document);
            else
            {
                document->skeletonEdit.enabled = true;
                document->scene->SetEditorMeshEditPose(true);
                SyncSkeletonEditSelection(document->scene.get(),
                    document->skeletonEdit);
            }
        }
        if (loaded && document->meshStage)
        {
            document->meshEdit.enabled = true;
            document->scene->SetEditorMeshEditPose(true);
            ApplyMeshStageVisibility(*document);
        }
        if (document->skeletonStage && !document->selectableObjects.empty())
        {
            if (document->selectableObjects.find(document->subject) ==
                document->selectableObjects.end())
                document->subject = *document->selectableObjects.begin();
            document->scene->SetSelectedObject(document->subject);
        }
        if (!loaded)
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not open scene document: " + normalized);
            continue;
        }
        document->savedSnapshot = CaptureSceneAssetDocumentSnapshot(*document);
        document->baseline = document->savedSnapshot;
        const std::string filename = std::filesystem::path(normalized).filename().string();
        auto view = m_viewFactory->CreateSceneView(document->scene.get(), filename,
            EditorPanelDockArea::MainDocument);
        if (!view)
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not create scene document viewport: " + normalized);
            continue;
        }
        SceneAssetDocument* raw = document.get();
        raw->view = view.get();
        view->UseGlobalToolbar = true;
        view->SetEditMode(raw->skeletonEdit.enabled
            ? SceneEditMode::Skeleton : (raw->meshEdit.enabled
                ? SceneEditMode::Mesh : SceneEditMode::Object));
        raw->view->SetDocumentPath(normalized);
        view->OnMeshViewportInput = [this, raw](IEditorUi& ui,
            const EditorUiViewportInput& input, EditorTransformTool tool)
        {
            return raw->skeletonEdit.enabled
                ? HandleSkeletonViewport(ui, input, raw->scene.get(),
                    raw->skeletonEdit)
                : HandleMeshViewport(ui, input, raw->scene.get(),
                    raw->meshEdit, tool);
        };
        RefreshSceneAssetDocumentTitle(*raw);
        view->OnFocused = [this, raw]()
        {
            m_gameViewFocused = false;
            SetActiveSceneAssetDocument(raw);
        };
        view->OnObjectSelected = [this, raw](Engine::Core::Object* object)
        {
            if (Engine::Core::Component::CaptureEditorReferencePick(object))
            {
                if (m_primaryProperties && m_primaryProperties->OnComponentsChanged)
                    m_primaryProperties->OnComponentsChanged();
                return;
            }
            if (raw->skeletonStage && object &&
                raw->selectableObjects.find(object) ==
                    raw->selectableObjects.end())
                return;
            if (raw->objectStage && object)
            {
                bool inFocusedGraph = false;
                for (Engine::Core::Object* current = object; current;
                    current = current->Parent)
                    if (current == raw->subject)
                    {
                        inFocusedGraph = true;
                        break;
                    }
                if (!inFocusedGraph || (!raw->showChildHierarchy &&
                    object != raw->subject))
                    return;
            }
            raw->propertiesProjection = nullptr;
            raw->scene->SetSelectedObject(object);
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
        };
        view->OnObjectCreated = [this, raw](Engine::Core::Object* object)
        {
            raw->scene->SetSelectedObject(object);
            const std::string current = CaptureSceneAssetDocumentSnapshot(*raw);
            if (HistoryDiffers(raw->baseline, current))
            {
                if (m_historyLimit > 0)
                {
                    if (raw->undo.size() >= m_historyLimit) raw->undo.pop_front();
                    raw->undo.push_back(raw->baseline);
                }
                raw->redo.clear();
                raw->baseline = current;
                raw->dirty = HistoryDiffers(raw->baseline, raw->savedSnapshot);
                RefreshSceneAssetDocumentTitle(*raw);
            }
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
        };
        view->OnDeleteSelectionRequested = [this]()
        {
            if (m_primaryHierarchy) m_primaryHierarchy->RequestDeleteSelectedObject();
        };
        view->OnGizmoInteraction = [this, raw](bool active)
        {
            if (m_activeSceneAssetDocument == raw)
            {
                m_sceneEditInProgress = m_sceneEditInProgress || active;
                if (m_renderer) m_renderer->MarkDirty();
            }
        };
        view->CanSelectObject = [raw](const Engine::Core::Object* object)
        {
            if (!object) return true;
            if (raw->skeletonStage)
                return raw->selectableObjects.find(
                    const_cast<Engine::Core::Object*>(object)) !=
                    raw->selectableObjects.end();
            if (raw->meshStage)
                return object == raw->subject;
            if (raw->objectStage)
            {
                if (!raw->showChildHierarchy && object != raw->subject)
                    return false;
                for (const Engine::Core::Object* current = object; current;
                    current = current->Parent)
                    if (current == raw->subject) return true;
                return false;
            }
            return true;
        };
        const auto drawAssetFocusPicker = [this, raw](IEditorUi& ui)
        {
            if (!raw->prefab)
                return;
            std::vector<Engine::Components::Mesh*> meshes;
            Engine::Components::Skeleton* skeleton = nullptr;
            for (const auto& object : raw->scene->GetObjects())
            {
                if (!object) continue;
                for (Engine::Core::Component* component : object->Components)
                {
                    if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component);
                        mesh && !mesh->GetVertices().empty())
                        meshes.push_back(mesh);
                    if (auto* candidate = dynamic_cast<Engine::Components::Skeleton*>(component))
                        skeleton = candidate;
                }
            }
            std::vector<const char*> choices{ "Object" };
            const int objectMode = 0;
            int meshMode = -1;
            int skeletonMode = -1;
            if (!meshes.empty())
            {
                meshMode = static_cast<int>(choices.size());
                choices.push_back("Mesh");
            }
            if (skeleton)
            {
                skeletonMode = static_cast<int>(choices.size());
                choices.push_back("Skeleton");
            }
            int selectedMode = raw->meshStage ? meshMode
                : (raw->skeletonStage ? skeletonMode : objectMode);
            if (selectedMode < 0) selectedMode = objectMode;
            if (ui.Combo("Mode", &selectedMode, choices.data(),
                static_cast<int>(choices.size())))
            {
                FinishSkeletonPaintStroke(raw->skeletonEdit);
                if (raw->previewAnimation)
                    StopSkeletonAnimationPreview(*raw);
                RestoreSkeletonStageVisibility(*raw);
                raw->meshStage = false;
                raw->objectStage = false;
                raw->skeletonStage = false;
                raw->propertiesProjection = nullptr;
                raw->selectedVertex = 0;
                if (selectedMode == meshMode && meshMode >= 0)
                {
                    raw->mesh = meshes.front();
                    raw->subject = raw->mesh->Owner;
                    raw->meshStage = raw->subject != nullptr;
                    raw->meshSavePath = raw->mesh->GetFilePath();
                    if (raw->meshSavePath.empty())
                        raw->meshSavePath = (std::filesystem::path(raw->path).parent_path() /
                            (raw->subject->name + ".mesh")).string();
                    raw->scene->SetSelectedObject(raw->subject);
                    ApplyMeshStageVisibility(*raw);
                }
                else if (selectedMode == skeletonMode && skeletonMode >= 0)
                    RebuildSkeletonStageContext(*raw);
                else
                    RebuildObjectStageContext(*raw);
                raw->meshEdit.enabled = raw->meshStage;
                raw->skeletonEdit.enabled = raw->skeletonStage;
                raw->skeletonEdit.painting = false;
                raw->skeletonEdit.observedSubmode = -1;
                raw->scene->SetEditorSelectedMesh(nullptr);
                raw->scene->SetEditorMeshEditPose(raw->meshStage ||
                    raw->skeletonStage);
                if (raw->view)
                {
                    raw->view->AllowObjectCreation = false;
                    raw->view->AllowAssetDrops = false;
                    raw->view->AllowObjectTransform = !raw->meshStage &&
                        (!raw->skeletonStage || raw->skeletonEdit.submode == 0);
                    raw->view->SetEditMode(raw->skeletonStage
                        ? SceneEditMode::Skeleton : (raw->meshStage
                            ? SceneEditMode::Mesh : SceneEditMode::Object));
                }
                if (raw->skeletonStage)
                    SyncSkeletonEditSelection(raw->scene.get(),
                        raw->skeletonEdit);
                else raw->scene->SetEditorWeightPaint(nullptr, -1);
                SetActiveSceneAssetDocument(raw, true);
                if (m_renderer) m_renderer->MarkDirty();
            }
        };
        raw->focusPicker = drawAssetFocusPicker;
        if (raw->prefab && raw->modelStage)
            raw->objectStageTools = [this, raw](IEditorUi& ui)
            {
                DrawObjectStageTools(ui, *raw);
            };
        if (raw->prefab && raw->modelStage && raw->skeleton)
        {
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
            raw->skeletonStageTools = [this](IEditorUi& ui)
            { DrawSkeletonEditTools(ui); };
        }
        if (raw->meshStage || raw->prefab)
        {
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
            view->AllowObjectTransform = !raw->meshStage;
            raw->meshStageTools = [this, raw](IEditorUi& ui)
            {
                DrawMeshStageTools(ui, *raw);
            };
        }
        if (raw->prefab && raw->modelStage)
            view->OnDrawDocumentTools = [this, raw, drawAssetFocusPicker](IEditorUi& ui)
            {
                ui.PushId(raw);
                const std::string headerId = "##AssetHeader:" + raw->identity;
                if (!ui.BeginViewportHeader(headerId.c_str(), 380.f))
                {
                    ui.PopId();
                    return false;
                }
                const float contentWidth = ui.AvailableContentWidth();
                ui.SetNextItemWidth(contentWidth >= 280.f ? 100.f : 72.f);
                drawAssetFocusPicker(ui);
                ui.SameLine();
                const float toolsWidth = std::clamp(
                    ui.AvailableContentWidth() - 62.f, 88.f, 130.f);
                const char* toolsLabel = raw->meshStage ? "Mesh Tools"
                    : (raw->skeletonStage ? "Skeleton Tools" : "Object Tools");
                if (ui.BeginViewportHeaderDropdown("##AssetStageTools",
                    toolsLabel, toolsWidth, 320.f))
                {
                    if (raw->objectStage && raw->objectStageTools)
                        raw->objectStageTools(ui);
                    else if (raw->meshStage && raw->meshStageTools)
                        raw->meshStageTools(ui);
                    else if (raw->skeletonStage && raw->skeletonStageTools)
                        raw->skeletonStageTools(ui);
                    ui.EndViewportHeaderDropdown();
                }
                ui.SameLine();
                if (ui.AvailableContentWidth() >= 54.f && ui.Button("Save"))
                    SaveSceneAssetDocument(*raw);
                const bool consumedClick = ui.EndViewportHeader();
                ui.PopId();
                return consumedClick;
            };
        else if (raw->prefab)
            view->OnDrawDocumentTools = [this, raw](IEditorUi& ui)
            {
                ui.PushId(raw);
                const std::string headerId = "##PrefabHeader:" + raw->identity;
                if (!ui.BeginViewportHeader(headerId.c_str(), 230.f))
                {
                    ui.PopId();
                    return false;
                }
                int mode = raw->skeletonEdit.enabled ? 2
                    : raw->meshEdit.enabled ? 1
                    : raw->view->GetEditMode() == SceneEditMode::Terrain ? 3 : 0;
                const ViewportModeControl controls = DrawViewportModeControl(ui, mode);
                if (controls.changed)
                {
                    SetSkeletonEditMode(raw->scene.get(), raw->skeletonEdit,
                        raw->meshEdit, false);
                    SetMeshEditMode(raw->scene.get(), raw->meshEdit, mode == 1);
                    if (mode == 2)
                        SetSkeletonEditMode(raw->scene.get(), raw->skeletonEdit,
                            raw->meshEdit, true);
                    SetActiveSceneAssetDocument(raw, true);
                    if (mode == 3)
                        raw->view->SetEditMode(SceneEditMode::Terrain);
                }
                if (raw->meshEdit.enabled)
                    SyncMeshEditSelection(raw->scene.get(), raw->meshEdit);
                if (raw->skeletonEdit.enabled)
                    SyncSkeletonEditSelection(raw->scene.get(),
                        raw->skeletonEdit);
                if (controls.showSave)
                {
                    ui.SameLine();
                    if (ui.Button("Save")) SaveSceneAssetDocument(*raw);
                }
                const bool consumedClick = ui.EndViewportHeader();
                ui.PopId();
                return consumedClick;
            };
        else if (!raw->meshStage)
            view->OnDrawDocumentTools = [this, raw](IEditorUi& ui)
            {
                ui.PushId(raw);
                if (!ui.BeginViewportHeader("##SceneAssetEditMode", 230.f))
                {
                    ui.PopId();
                    return false;
                }
                int mode = raw->skeletonEdit.enabled ? 2
                    : raw->meshEdit.enabled ? 1
                    : raw->view->GetEditMode() == SceneEditMode::Terrain ? 3 : 0;
                const ViewportModeControl controls = DrawViewportModeControl(ui, mode);
                if (controls.changed)
                {
                    SetSkeletonEditMode(raw->scene.get(), raw->skeletonEdit,
                        raw->meshEdit, false);
                    SetMeshEditMode(raw->scene.get(), raw->meshEdit, mode == 1);
                    if (mode == 2)
                        SetSkeletonEditMode(raw->scene.get(), raw->skeletonEdit,
                            raw->meshEdit, true);
                    SetActiveSceneAssetDocument(raw, true);
                    if (mode == 3)
                        raw->view->SetEditMode(SceneEditMode::Terrain);
                }
                if (raw->meshEdit.enabled)
                    SyncMeshEditSelection(raw->scene.get(), raw->meshEdit);
                if (raw->skeletonEdit.enabled)
                    SyncSkeletonEditSelection(raw->scene.get(),
                        raw->skeletonEdit);
                if (controls.showSave)
                {
                    ui.SameLine();
                    if (ui.Button("Save")) SaveSceneAssetDocument(*raw);
                }
                const bool consumed = ui.EndViewportHeader();
                ui.PopId();
                return consumed;
            };
        else if (raw->meshStage)
            view->OnDrawDocumentTools = [this, raw](IEditorUi& ui)
            {
                ui.PushId(raw);
                const std::string headerId = "##MeshHeader:" + raw->identity;
                if (!ui.BeginViewportHeader(headerId.c_str(), 300.f))
                {
                    ui.PopId();
                    return false;
                }
                ui.Label("Mesh");
                ui.SameLine();
                const float toolsWidth = std::min(150.f,
                    std::max(100.f, ui.AvailableContentWidth() - 70.f));
                if (ui.BeginViewportHeaderDropdown("##MeshStageTools",
                    "Mesh Tools", toolsWidth, 320.f))
                {
                    if (raw->meshStageTools)
                        raw->meshStageTools(ui);
                    ui.EndViewportHeaderDropdown();
                }
                ui.SameLine();
                if (ui.AvailableContentWidth() >= 54.f && ui.Button("Save"))
                    SaveSceneAssetDocument(*raw);
                const bool consumedClick = ui.EndViewportHeader();
                ui.PopId();
                return consumedClick;
            };
        if (raw->objectStage)
        {
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
        }
        view->RequestFocusOnNextDraw();
        m_panels.push_back(std::move(view));
        m_sceneAssetDocuments.push_back(std::move(document));
        SetActiveSceneAssetDocument(raw);
    }
}

void EditorState::SetActiveSceneAssetDocument(SceneAssetDocument* document,
    bool refresh)
{
    if (!refresh && m_activeSceneAssetDocument == document &&
        !m_activeAssetDocument &&
        !m_activeAnimationDocument &&
        m_prefabDocumentFocused == (document && document->prefab))
        return;
    m_activeSceneAssetDocument = document;
    m_activeAnimationDocument = nullptr;
    m_activeAssetDocument = nullptr;
    m_prefabDocumentFocused = document && document->prefab;
    const auto scene = document ? document->scene.get() : m_scene.get();
    for (auto& panel : m_panels)
        if (auto* gameView = dynamic_cast<GameView*>(panel.get()))
            gameView->SetScene(m_scene.get());
    if (m_primaryHierarchy)
    {
        m_primaryHierarchy->Init(scene);
        m_primaryHierarchy->SetObjectFilter(document &&
            (document->skeletonStage || document->meshStage || document->objectStage)
            ? std::function<bool(const Engine::Core::Object*)>(
                [document](const Engine::Core::Object* object)
                {
                    return document->meshStage
                        ? object == document->subject
                        : (document->objectStage
                            ? (object == document->subject ||
                                (document->showChildHierarchy && [&]()
                                {
                                    for (const Engine::Core::Object* current = object;
                                        current; current = current->Parent)
                                        if (current == document->subject) return true;
                                    return false;
                                }()))
                            : document->selectableObjects.find(
                            const_cast<Engine::Core::Object*>(object)) !=
                            document->selectableObjects.end());
                })
            : std::function<bool(const Engine::Core::Object*)>{});
        m_primaryHierarchy->SetAllowDelete(!document ||
            (!document->meshStage && !document->skeletonStage &&
                !document->objectStage));
        m_primaryHierarchy->SetFilteredObjectContextActions(document &&
            document->objectStage && !document->modelStage,
            document ? document->subject : nullptr);
        m_primaryHierarchy->SetSkeletonContextActions(document &&
            document->skeletonStage,
            document ? document->subject : nullptr);
        Engine::Core::Object* selected = scene ? scene->GetSelectedObject() : nullptr;
        if (document && document->meshStage && selected != document->subject)
            selected = document->subject;
        if (document && document->objectStage && !document->showChildHierarchy &&
            selected != document->subject)
            selected = document->subject;
        if (document && document->skeletonStage && selected &&
            document->selectableObjects.find(selected) ==
                document->selectableObjects.end())
            selected = document->subject;
        m_primaryHierarchy->SetSelectedObject(selected);
    }
    if (m_primaryProperties)
    {
        m_primaryProperties->Init(scene);
        m_primaryProperties->SetAllowComponentStructureEdits(!document ||
            (!document->meshStage && !document->skeletonStage));
        m_primaryProperties->SetSelectedObject(document &&
            document->propertiesProjection
            ? document->propertiesProjection
            : (scene ? scene->GetSelectedObject() : nullptr));
        m_primaryProperties->SetSelectedAsset("");
    }
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::RefreshSceneAssetDocumentTitle(SceneAssetDocument& document)
{
    if (!document.view) return;
    std::string title = std::filesystem::path(document.path).filename().string();
    if (document.dirty || document.meshEdit.dirty) title += " *";
    document.view->SetTitle(title + "###SceneAssetDocument:" + document.identity);
}

bool EditorState::SaveSceneAssetDocument(SceneAssetDocument& document)
{
    FrameActivity::Mark(FrameActivity::Save);
    FinishSkeletonPaintStroke(document.skeletonEdit);
    if (document.meshEdit.dirty &&
        !SaveMeshEditSession(document.meshEdit)) return false;
    if (!SavePendingMeshEdits()) return false;
    bool saved = false;
    const std::string stageSavePath = document.stageDataPath.empty()
        ? document.path : document.stageDataPath;
    if (document.previewAnimation)
        StopSkeletonAnimationPreview(document);
    if (document.skeletonStage || document.objectStage)
        RestoreSkeletonStageVisibility(document);
    if (document.meshStage && document.mesh)
        saved = Engine::Components::Mesh::SaveNativeFile(
            document.meshSavePath, document.mesh->GetVertices(),
            document.mesh->GetIndices());
    else if (document.scene && document.prefab)
    {
        Engine::Core::Object* root = nullptr;
        for (const auto& object : document.scene->GetObjects())
            if (object && !object->Parent)
            {
                if (root) { root = nullptr; break; }
                root = object.get();
            }
        saved = root && Engine::Rendering::RenderJobs::Get().Submit(
            Engine::Serialization::SceneSerializer::PreparePrefabSave(
                *root, stageSavePath)).get();
    }
    else if (document.scene)
    {
        auto value = Engine::Serialization::SceneSerializer::CaptureSceneDocument(*document.scene);
        saved = Engine::Rendering::RenderJobs::Get().Submit(
            [value = std::move(value), path = document.path] {
                const auto xml = Engine::Serialization::SceneSerializer::EncodeSceneFile(value);
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                if (!file) return false;
                file.write(xml.data(), static_cast<std::streamsize>(xml.size()));
                file.flush();
                return file.good();
            }).get();
    }
    if (document.skeletonStage)
        ApplySkeletonStageVisibility(document);
    else if (document.objectStage)
        ApplyObjectStageVisibility(document);
    if (!saved)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Failed to save scene document: " + document.path);
        return false;
    }
    document.savedSnapshot = CaptureSceneAssetDocumentSnapshot(document);
    document.baseline = document.savedSnapshot;
    document.dirty = false;
    RefreshSceneAssetDocumentTitle(document);
    if (!document.meshStage)
        AssetPreviewCache::CaptureScene(document.path, *document.scene,
            document.scene->GetGraphicsProvider());
    if (document.prefab && m_scene)
        Engine::Serialization::SceneSerializer::RefreshPrefabInstances(*m_scene,
            stageSavePath, m_scene->GetGraphicsProvider());
    if (document.prefab)
        for (const auto& other : m_sceneAssetDocuments)
            if (other && other.get() != &document && other->scene)
                Engine::Serialization::SceneSerializer::RefreshPrefabInstances(
                    *other->scene, stageSavePath,
                    other->scene->GetGraphicsProvider());
    if (document.meshStage && m_viewFactory &&
        m_viewFactory->OnAssetContentsChanged)
        m_viewFactory->OnAssetContentsChanged(document.meshSavePath);
    if (document.meshStage)
    {
        // Skeleton stages retain live component pointers, so the general scene
        // reload callback intentionally skips them. Refresh matching meshes in
        // place after the replacement file becomes visible.
        for (const auto& other : m_sceneAssetDocuments)
            if (other && other.get() != &document && other->skeletonStage &&
                other->scene)
                for (const auto& object : other->scene->GetObjects())
                    if (object)
                        for (Engine::Core::Component* component : object->Components)
                            if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(
                                    component);
                                mesh && AssetPathIdentity(mesh->GetFilePath()) ==
                                    AssetPathIdentity(document.meshSavePath))
                            {
                                mesh->LoadFromFile(mesh->GetFilePath());
                                if (auto* provider = other->scene->GetGraphicsProvider())
                                    mesh->CreateBuffer(provider->GetBufferFactory());
                            }
    }
    return true;
}

std::string EditorState::CaptureSceneAssetDocumentSnapshot(
    SceneAssetDocument& document)
{
    if (document.skeletonStage || document.objectStage)
    {
        RestoreSkeletonStageVisibility(document);
        auto value = document.scene
            ? Engine::Serialization::SceneSerializer::CaptureSceneDocument(*document.scene)
            : Engine::Serialization::JsonValue{};
        if (document.skeletonStage)
            ApplySkeletonStageVisibility(document);
        else
            ApplyObjectStageVisibility(document);
        if (!document.scene) return {};
        return Engine::Rendering::RenderJobs::Get().Submit(
            [value = std::move(value)] {
                return Engine::Serialization::SceneSerializer::EncodeSceneFile(value);
            }).get();
    }
    if (!document.meshStage || !document.mesh)
    {
        if (!document.scene) return {};
        auto value = Engine::Serialization::SceneSerializer::CaptureSceneDocument(*document.scene);
        return Engine::Rendering::RenderJobs::Get().Submit(
            [value = std::move(value)] {
                return Engine::Serialization::SceneSerializer::EncodeSceneFile(value);
            }).get();
    }
    const auto& vertices = document.mesh->GetVertices();
    const auto& indices = document.mesh->GetIndices();
    const uint32_t counts[] = { static_cast<uint32_t>(vertices.size()),
        static_cast<uint32_t>(indices.size()) };
    std::string snapshot(reinterpret_cast<const char*>(counts), sizeof(counts));
    snapshot.append(reinterpret_cast<const char*>(vertices.data()),
        vertices.size() * sizeof(Engine::Components::Mesh::Vertex));
    snapshot.append(reinterpret_cast<const char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    return snapshot;
}

bool EditorState::RestoreSceneAssetDocumentSnapshot(
    SceneAssetDocument& document, const std::string& snapshot)
{
    if (!document.meshStage)
    {
        if (!document.scene || !document.scene->LoadFromString(snapshot))
            return false;
        document.skeletonEdit.skeleton = nullptr;
        document.skeletonEdit.bindingMesh = nullptr;
        document.skeletonEdit.mesh = nullptr;
        document.skeletonEdit.adjacency.reset();
        document.skeletonEdit.observedBindTransforms.clear();
        document.scene->SetEditorWeightPaint(nullptr, -1);
        document.scene->SetEditorMeshEditPose(document.meshEdit.enabled ||
            document.skeletonEdit.enabled);
        if (document.skeletonStage)
            RebuildSkeletonStageContext(document);
        else if (document.objectStage)
            RebuildObjectStageContext(document);
        return true;
    }
    if (!document.mesh || snapshot.size() < 2 * sizeof(uint32_t))
        return false;
    uint32_t counts[2]{};
    std::memcpy(counts, snapshot.data(), sizeof(counts));
    const size_t vertexBytes = static_cast<size_t>(counts[0]) *
        sizeof(Engine::Components::Mesh::Vertex);
    const size_t indexBytes = static_cast<size_t>(counts[1]) * sizeof(uint32_t);
    if (counts[0] == 0 || vertexBytes > snapshot.size() - sizeof(counts) ||
        indexBytes != snapshot.size() - sizeof(counts) - vertexBytes)
        return false;
    std::vector<Engine::Components::Mesh::Vertex> vertices(counts[0]);
    std::vector<uint32_t> indices(counts[1]);
    std::memcpy(vertices.data(), snapshot.data() + sizeof(counts), vertexBytes);
    std::memcpy(indices.data(), snapshot.data() + sizeof(counts) + vertexBytes,
        indexBytes);
    return indices.empty()
        ? document.mesh->SetAuthoredVertices(std::move(vertices))
        : document.mesh->SetIndexedGeometry(std::move(vertices),
            std::move(indices));
}

void EditorState::ApplySkeletonStageVisibility(SceneAssetDocument& document)
{
    if (!document.scene || !document.skeletonStage)
        return;
    if (document.originalEnabledState.empty())
        for (const auto& object : document.scene->GetObjects())
            if (object)
                document.originalEnabledState.emplace(object.get(), object->enabled);
    for (const auto& [object, enabled] : document.originalEnabledState)
        if (object) object->enabled = enabled;

    std::unordered_set<Engine::Core::Object*> visible;
    const auto includeAncestors = [&visible](Engine::Core::Object* object)
    {
        for (Engine::Core::Object* current = object; current; current = current->Parent)
            visible.insert(current);
    };
    for (Engine::Core::Object* bone : document.selectableObjects)
        includeAncestors(bone);
    if (document.showSkeletonMesh)
        for (Engine::Core::Object* mesh : document.skeletonMeshObjects)
            includeAncestors(mesh);

    for (const auto& object : document.scene->GetObjects())
        if (object)
        {
            const auto original = document.originalEnabledState.find(object.get());
            object->enabled = original != document.originalEnabledState.end() &&
                original->second && visible.find(object.get()) != visible.end();
            for (Engine::Core::Component* component : object->Components)
            {
                if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component))
                {
                    if (!document.originalMeshVisibility.count(mesh))
                        document.originalMeshVisibility.emplace(mesh,
                            mesh->IsEditorVisible());
                    const bool associatedGhost = document.showSkeletonMesh &&
                        document.skeletonMeshObjects.count(object.get()) != 0;
                    mesh->SetEditorVisible(associatedGhost);
                }
                if (auto* sprite = dynamic_cast<Engine::Components::Sprite*>(component))
                {
                    if (!document.originalSpriteVisibility.count(sprite))
                        document.originalSpriteVisibility.emplace(sprite,
                            sprite->IsEditorVisible());
                    sprite->SetEditorVisible(false);
                }
            }
        }

    if (document.ghostMaterialStates.empty())
        for (Engine::Core::Object* object : document.skeletonMeshObjects)
            if (object)
                for (Engine::Core::Component* component : object->Components)
                    if (auto* material = dynamic_cast<Engine::Components::Material*>(component))
                        document.ghostMaterialStates.push_back({ material,
                            material->baseColorAlpha, material->alphaMode,
                            material->castsShadows });
    for (auto& state : document.ghostMaterialStates)
        if (state.material)
        {
            state.material->baseColorAlpha = document.showSkeletonMesh
                ? 0.24f : state.alpha;
            state.material->alphaMode = document.showSkeletonMesh
                ? "Blend" : state.alphaMode;
            state.material->castsShadows = document.showSkeletonMesh
                ? false : state.castsShadows;
        }
}

void EditorState::RestoreSkeletonStageVisibility(SceneAssetDocument& document)
{
    for (const auto& [object, enabled] : document.originalEnabledState)
        if (object) object->enabled = enabled;
    for (auto& state : document.ghostMaterialStates)
        if (state.material)
        {
            state.material->baseColorAlpha = state.alpha;
            state.material->alphaMode = state.alphaMode;
            state.material->castsShadows = state.castsShadows;
        }
    for (const auto& [mesh, visible] : document.originalMeshVisibility)
        if (mesh) mesh->SetEditorVisible(visible);
    document.originalMeshVisibility.clear();
    for (const auto& [sprite, visible] : document.originalSpriteVisibility)
        if (sprite) sprite->SetEditorVisible(visible);
    document.originalSpriteVisibility.clear();
}

void EditorState::RebuildSkeletonStageContext(SceneAssetDocument& document)
{
    document.skeletonStage = false;
    document.skeleton = nullptr;
    document.selectableObjects.clear();
    document.skeletonMeshObjects.clear();
    document.originalEnabledState.clear();
    document.ghostMaterialStates.clear();
    if (!document.scene)
        return;
    for (const auto& object : document.scene->GetObjects())
        for (Engine::Core::Component* component : object->Components)
            if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
            {
                document.skeletonStage = true;
                document.skeleton = skeleton;
                for (Engine::Components::AnimationBone* bone : skeleton->ResolveBones())
                    if (bone && bone->Owner)
                        document.selectableObjects.insert(bone->Owner);
                document.subject = skeleton->GetHierarchyRoot();
                break;
            }
    if (!document.skeletonStage)
        return;
    for (const auto& object : document.scene->GetObjects())
        for (Engine::Core::Component* component : object->Components)
            if (auto* skinned = dynamic_cast<Engine::Components::SkinnedMesh*>(component))
            {
                Engine::Components::Skeleton* bound =
                    skinned->skeletonReference.IsAssigned()
                    ? Engine::Core::ResolveComponentReference<
                        Engine::Components::Skeleton>(skinned->Owner,
                            skinned->skeletonReference)
                    : nullptr;
                if (bound == document.skeleton ||
                    (!bound && skinned->skinIndex >= 0 &&
                        static_cast<unsigned>(skinned->skinIndex) ==
                            document.skeleton->skinIndex))
                    document.skeletonMeshObjects.insert(object.get());
            }
    if (!document.selectableObjects.empty() &&
        document.selectableObjects.find(document.subject) ==
            document.selectableObjects.end())
        document.subject = *document.selectableObjects.begin();
    document.scene->SetSelectedObject(document.subject);
    ApplySkeletonStageVisibility(document);
}

void EditorState::RebuildObjectStageContext(SceneAssetDocument& document)
{
    document.objectStage = false;
    document.originalEnabledState.clear();
    document.ghostMaterialStates.clear();
    if (!document.scene || !document.prefab || document.skeletonStage)
        return;
    Engine::Core::Object* root = nullptr;
    for (const auto& object : document.scene->GetObjects())
        if (object && !object->Parent)
        {
            if (root) return;
            root = object.get();
        }
    if (!root) return;
    document.objectStage = true;
    document.subject = root;
    document.scene->SetSelectedObject(root);
    ApplyObjectStageVisibility(document);
}

void EditorState::ApplyObjectStageVisibility(SceneAssetDocument& document)
{
    if (!document.scene || !document.objectStage || !document.subject)
        return;
    if (document.originalEnabledState.empty())
        for (const auto& object : document.scene->GetObjects())
            if (object)
                document.originalEnabledState.emplace(object.get(), object->enabled);
    std::unordered_set<Engine::Core::Object*> visible;
    visible.insert(document.subject);
    if (document.showChildHierarchy)
    {
        std::vector<Engine::Core::Object*> pending(
            document.subject->Children.begin(), document.subject->Children.end());
        while (!pending.empty())
        {
            Engine::Core::Object* child = pending.back();
            pending.pop_back();
            if (!child || !visible.insert(child).second) continue;
            pending.insert(pending.end(), child->Children.begin(), child->Children.end());
        }
    }
    for (const auto& object : document.scene->GetObjects())
        if (object)
        {
            const auto original = document.originalEnabledState.find(object.get());
            object->enabled = original != document.originalEnabledState.end() &&
                original->second && visible.find(object.get()) != visible.end();
            for (Engine::Core::Component* component : object->Components)
            {
                if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component))
                {
                    if (!document.originalMeshVisibility.count(mesh))
                        document.originalMeshVisibility.emplace(mesh,
                            mesh->IsEditorVisible());
                    mesh->SetEditorVisible(visible.count(object.get()) != 0);
                }
                if (auto* sprite = dynamic_cast<Engine::Components::Sprite*>(component))
                {
                    if (!document.originalSpriteVisibility.count(sprite))
                        document.originalSpriteVisibility.emplace(sprite,
                            sprite->IsEditorVisible());
                    sprite->SetEditorVisible(visible.count(object.get()) != 0);
                }
            }
        }
}

void EditorState::ApplyMeshStageVisibility(SceneAssetDocument& document)
{
    if (!document.scene || !document.meshStage || !document.mesh ||
        !document.subject)
        return;
    if (document.originalEnabledState.empty())
        for (const auto& object : document.scene->GetObjects())
            if (object)
                document.originalEnabledState.emplace(object.get(), object->enabled);
    std::unordered_set<Engine::Core::Object*> visible;
    for (Engine::Core::Object* current = document.subject; current;
        current = current->Parent)
        visible.insert(current);
    for (Engine::Core::Component* component : document.subject->Components)
        if (auto* skinned = dynamic_cast<Engine::Components::SkinnedMesh*>(component))
        {
            Engine::Components::Skeleton* bound =
                skinned->skeletonReference.IsAssigned()
                ? Engine::Core::ResolveComponentReference<
                    Engine::Components::Skeleton>(skinned->Owner,
                        skinned->skeletonReference)
                : nullptr;
            if (!bound)
                for (const auto& candidate : document.scene->GetObjects())
                    if (candidate)
                        if (auto* skeleton = candidate->GetComponent<
                            Engine::Components::Skeleton>(); skeleton &&
                            skinned->skinIndex >= 0 && skeleton->skinIndex ==
                                static_cast<unsigned>(skinned->skinIndex))
                        {
                            bound = skeleton;
                            break;
                        }
            if (!bound) continue;
            document.skeleton = bound;
            for (Engine::Components::AnimationBone* bone : bound->ResolveBones())
                if (bone && bone->Owner)
                    for (Engine::Core::Object* current = bone->Owner; current;
                        current = current->Parent)
                        visible.insert(current);
        }
    for (const auto& object : document.scene->GetObjects())
        if (object)
        {
            const auto original = document.originalEnabledState.find(object.get());
            object->enabled = original != document.originalEnabledState.end() &&
                original->second && visible.count(object.get()) != 0;
            for (Engine::Core::Component* component : object->Components)
            {
                if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component))
                {
                    if (!document.originalMeshVisibility.count(mesh))
                        document.originalMeshVisibility.emplace(mesh,
                            mesh->IsEditorVisible());
                    mesh->SetEditorVisible(mesh == document.mesh);
                }
                if (auto* sprite = dynamic_cast<Engine::Components::Sprite*>(component))
                {
                    if (!document.originalSpriteVisibility.count(sprite))
                        document.originalSpriteVisibility.emplace(sprite,
                            sprite->IsEditorVisible());
                    sprite->SetEditorVisible(false);
                }
            }
        }
}

void EditorState::StopSkeletonAnimationPreview(SceneAssetDocument& document)
{
    if (!document.previewAnimation) return;
    document.previewAnimation = false;
    document.previewAnimationManager = nullptr;
    const std::string snapshot = std::move(document.previewRestoreSnapshot);
    document.previewRestoreSnapshot.clear();
    if (!snapshot.empty())
        RestoreSceneAssetDocumentSnapshot(document, snapshot);
    if (m_activeSceneAssetDocument == &document)
    {
        if (m_primaryHierarchy)
        {
            m_primaryHierarchy->Init(document.scene.get());
            m_primaryHierarchy->SetSelectedObject(document.subject);
        }
        if (m_primaryProperties)
        {
            m_primaryProperties->Init(document.scene.get());
            m_primaryProperties->SetSelectedObject(document.subject);
        }
    }
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::HandleSceneAssetDocumentClosures()
{
    for (auto it = m_sceneAssetDocuments.begin(); it != m_sceneAssetDocuments.end();)
    {
        SceneAssetDocument* document = it->get();
        if (!document || document->view->IsOpen()) { ++it; continue; }
        if (document->dirty || document->meshEdit.dirty)
        {
            document->view->SetOpen(true);
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                    "Save the scene document before closing its editor.");
            ++it;
            continue;
        }
        if (m_activeSceneAssetDocument == document)
            SetActiveSceneAssetDocument(nullptr);
        for (auto panel = m_panels.begin(); panel != m_panels.end(); ++panel)
            if (panel->get() == document->view)
            {
                if ((*panel)->NeedsRender() && m_viewFactory)
                    if (auto* view = dynamic_cast<View*>(panel->get()))
                        m_viewFactory->FreeSrvSlot(view->GetSrvSlotIndex());
                if (m_viewFactory) m_viewFactory->NotifyPanelRemoved(panel->get());
                m_panels.erase(panel);
                break;
            }
        it = m_sceneAssetDocuments.erase(it);
    }
}

void EditorState::ProcessPendingAssetDocumentOpens()
{
    while (!m_pendingAssetDocuments.empty())
    {
        auto [path, editorType] = std::move(m_pendingAssetDocuments.front());
        m_pendingAssetDocuments.pop_front();
        const std::string identity = AssetPathIdentity(path);
        bool alreadyOpen = false;
        for (AssetDocumentView* document : m_assetDocuments)
            if (document && AssetPathIdentity(document->GetPath()) == identity)
            {
                document->SetOpen(true);
                SetActiveAssetDocument(document);
                alreadyOpen = true;
                break;
            }
        if (alreadyOpen)
            continue;

        auto document = std::make_unique<AssetDocumentView>(path,
            editorType, m_scene.get());
        AssetDocumentView* raw = document.get();
        raw->OnFocused = [this, raw]() { SetActiveAssetDocument(raw); };
        raw->OnChanged = [this, raw]()
        {
            if (m_viewFactory && m_viewFactory->OnAssetContentsChanged)
                m_viewFactory->OnAssetContentsChanged(raw->GetPath());
            if (m_renderer)
                m_renderer->MarkDirty();
        };
        raw->OnRenamed = [this](const std::string& oldPath,
            const std::string& newPath)
        {
            if (m_primaryAssets)
                m_primaryAssets->SetSelectedPath(newPath);
            if (m_viewFactory && m_viewFactory->OnAssetRenamed)
                m_viewFactory->OnAssetRenamed(oldPath, newPath);
        };
        m_assetDocuments.push_back(raw);
        m_panels.push_back(std::move(document));
        SetActiveAssetDocument(raw);
    }
}

void EditorState::SetActiveAssetDocument(AssetDocumentView* document)
{
    m_gameViewFocused = false;
    SetPrefabDocumentFocused(false);
    SetActiveSceneAssetDocument(nullptr);
    m_activeAssetDocument = document;
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::RefreshSceneDocumentTitle()
{
    const std::string filename = m_currentScenePath.empty()
        ? "Untitled.scene"
        : std::filesystem::path(m_currentScenePath).filename().string();
    const std::string title = filename +
        (m_hasUnsavedChanges ? " *" : "");
    for (const auto& panel : m_panels)
        if (auto* sceneView = dynamic_cast<SceneView*>(panel.get());
            sceneView && sceneView->GetScene() == m_scene.get())
        {
            sceneView->SetDocumentPath(m_currentScenePath);
            // The main viewport survives scene changes. Keep its window ID
            // (including the slot for additional views) so docking survives too.
            const std::string& previousTitle = sceneView->GetTitle();
            const size_t idStart = previousTitle.find("###");
            const std::string windowId = idStart == std::string::npos
                ? "###" + previousTitle : previousTitle.substr(idStart);
            sceneView->SetTitle(title + windowId);
        }
}

void EditorState::HandleAssetDocumentClosures()
{
    for (auto it = m_assetDocuments.begin(); it != m_assetDocuments.end();)
    {
        AssetDocumentView* document = *it;
        if (!document || document->IsOpen())
        {
            ++it;
            continue;
        }
        if (document->IsDirty())
        {
            document->SetOpen(true);
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                    "Save the asset before closing its editor.");
            ++it;
            continue;
        }
        if (m_activeAssetDocument == document)
            m_activeAssetDocument = nullptr;
        it = m_assetDocuments.erase(it);
    }
}

void EditorState::ClosePrefabStage()
{
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab)
    {
        m_activeSceneAssetDocument->view->SetOpen(false);
        HandleSceneAssetDocumentClosures();
        return;
    }
    if (m_activePrefabPath.empty()) return;
    if (m_prefabHasUnsavedChanges)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Save the prefab before closing its stage.");
        return;
    }

    SetPrefabDocumentFocused(false);
    RemovePrefabPanels();
    m_prefabScene.reset();
    m_prefabMeshEdit = {};
    m_prefabSkeletonEdit = {};
    m_activePrefabPath.clear();
    SetPrefabDirty(false);
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            "Closed prefab stage.");
}

void EditorState::HandlePrefabPanelClosures()
{
    if (m_activePrefabPath.empty())
        return;
    const bool panelClosed =
        (m_prefabSceneView && !m_prefabSceneView->IsOpen());
    if (!panelClosed)
        return;

    if (m_prefabHasUnsavedChanges)
    {
        if (m_prefabSceneView) m_prefabSceneView->SetOpen(true);
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Save the prefab before closing its editor.");
        return;
    }
    ClosePrefabStage();
}

void EditorState::RemovePrefabPanels()
{
    auto isPrefabPanel = [this](const std::unique_ptr<IEditorPanel>& panel)
    {
        return panel.get() == m_prefabSceneView;
    };
    for (auto it = m_panels.begin(); it != m_panels.end();)
    {
        if (!isPrefabPanel(*it))
        {
            ++it;
            continue;
        }
        if ((*it)->NeedsRender() && m_viewFactory)
            if (auto* view = dynamic_cast<View*>(it->get()))
                m_viewFactory->FreeSrvSlot(view->GetSrvSlotIndex());
        if (m_viewFactory)
            m_viewFactory->NotifyPanelRemoved(it->get());
        it = m_panels.erase(it);
    }
    m_prefabSceneView = nullptr;
}

}
