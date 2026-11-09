#include "pch.h"
#include "Engine/Editor/State/EditorState.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/SceneManager.h"
#include "Core/Rendering/RenderJobs.h"
#include "Engine/Editor/CodeEditorLink.h"
#include "Engine/Editor/Startup/EditorSceneSession.h"
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
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Physics.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/PreparedPrefab.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Sprite/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/AssetRecord.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Importers/ModelImporter.h"
#include "Core/Importers/VmdSceneImporter.h"
#include "Core/Importers/MmdDanceAssetBinder.h"
#include "Core/Importers/PortableAssetCopy.h"
#include "Core/Threading/JobScheduler.h"
#include "Core/Mesh/MeshEditGeometry.h"
#include "Core/Mesh/MeshEditSnapshot.h"
#include "Core/SnapshotHistory.h"
#include "Engine/Editor/Core/View/Templates/Assets/AssetPreviewCache.h"
#include "Engine/Editor/Core/Paint/PaintInput.h"
#include "Engine/Editor/Core/Paint/ImagePaint.h"
#include "Engine/Editor/Core/Paint/BrushTipMask.h"
#include "Engine/Editor/Core/Paint/MeshTexturePaint.h"
#include "Engine/Editor/Core/Paint/PngPaintWriter.h"
#include <commdlg.h>
#include <glm/gtc/matrix_inverse.hpp>

#ifndef ENGINE_ROOT_PATH
#define ENGINE_ROOT_PATH "."
#endif

#include "State/EditorStateSupport.h"

namespace Engine::Editor
{
using namespace StateSupport;

void EditorState::OpenCodeEditor()
{
    if (m_projectSettings.codeEditor == "None")
    {
        m_showPreferences = true;
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Select a code editor in Project Preferences > Editor.");
        return;
    }
    const std::filesystem::path workspace = m_projectFilePath.empty()
        ? std::filesystem::path(ENGINE_ROOT_PATH)
        : std::filesystem::path(m_projectFilePath).parent_path();
    if (!OpenInCodeEditor(workspace, m_projectSettings.codeEditor, true))
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not open the selected code editor. Check Project Preferences.");
        return;
    }

    if (m_projectSettings.codeEditor != "VSCode")
        return;
    std::filesystem::path file = LastCodeFile(workspace);
    if (file.empty())
        file = workspace / ".vscode" / "launch.json";
    std::error_code error;
    if (std::filesystem::is_regular_file(file, error))
        OpenInCodeEditor(file, m_projectSettings.codeEditor);
    RequestCodeEditorAttach();
}

bool EditorState::RequestCodeEditorAttach(bool openWorkspace)
{
    if (m_projectSettings.codeEditor == "None")
        return false;
    const std::filesystem::path workspace = m_projectFilePath.empty()
        ? std::filesystem::path(ENGINE_ROOT_PATH)
        : std::filesystem::path(m_projectFilePath).parent_path();
    const bool requested = m_projectSettings.codeEditor == "VSCode" &&
        Engine::Editor::RequestCodeEditorAttach(workspace);
    if (openWorkspace)
        OpenInCodeEditor(workspace, m_projectSettings.codeEditor, true);
    return requested;
}


// ---------------------------------------------------------------------------
// EditorState::EditorState
// ---------------------------------------------------------------------------
EditorState::EditorState(HINSTANCE hInstance, const Engine::Model::ProjectSettings& projectSettings,
    std::string projectFilePath)
    : m_projectSettings(projectSettings)
    , m_projectFilePath(std::move(projectFilePath))
    , m_historyLimit(std::min(projectSettings.editorHistoryLimit, 1000u))
{
    try
    {
        m_window = std::make_unique<::Engine::Core::Window>(hInstance,
            L"Engine Editor", std::max(320u, projectSettings.viewportWidth),
            std::max(240u, projectSettings.viewportHeight));
    }
    catch (const std::exception&)
    {
        // Window creation failed
    }
}

// ---------------------------------------------------------------------------
// EditorState::~EditorState
// ---------------------------------------------------------------------------
EditorState::~EditorState()
{
    if (m_importJobControl)
        m_importJobControl->cancelled.store(true, std::memory_order_release);
    if (m_stageImportControl)
        m_stageImportControl->cancelled.store(true, std::memory_order_release);
    if (m_bakeJobControl)
        m_bakeJobControl->cancelled.store(true, std::memory_order_release);
    if (m_bakePublication)
    {
        const auto root = m_bakePublication->outputRoot;
        Engine::Threading::JobScheduler::Get().Submit([root] {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        });
        m_bakePublication.reset();
    }
}

bool EditorState::HasUnsavedChanges() const
{
    if (m_hasUnsavedChanges || m_prefabHasUnsavedChanges)
        return true;
    if (m_mainMeshEdit.dirty || m_prefabMeshEdit.dirty)
        return true;
    for (const auto& [path, edit] : m_meshEditCache)
        if (edit.dirty) return true;
    for (const auto& document : m_sceneAssetDocuments)
        if (document && (document->dirty || document->meshEdit.dirty))
            return true;
    for (const auto& document : m_animationDocuments)
        if (document && document->dirty) return true;
    for (AssetDocumentView* document : m_assetDocuments)
        if (document && document->IsDirty())
            return true;
    return false;
}

bool EditorState::HasActiveDocumentUnsavedChanges() const
{
    if (m_activeAnimationDocument)
        return m_activeAnimationDocument->dirty;
    if (m_activeSceneAssetDocument)
        return m_activeSceneAssetDocument->dirty ||
            m_activeSceneAssetDocument->meshEdit.dirty;
    if (m_prefabDocumentFocused && !m_activePrefabPath.empty())
        return m_prefabHasUnsavedChanges || m_prefabMeshEdit.dirty;
    if (m_activeAssetDocument)
        return m_activeAssetDocument->IsDirty();
    return m_hasUnsavedChanges || m_mainMeshEdit.dirty;
}

bool EditorState::IsEditingPrefab() const
{
    return m_activeAnimationDocument ||
        (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab) ||
        (!m_activePrefabPath.empty() && m_prefabDocumentFocused);
}

// ---------------------------------------------------------------------------
// EditorState::Init
// ---------------------------------------------------------------------------
bool EditorState::Init()
{
    // Window was created in constructor; get its handle
    if (!m_window)
    {
        LogStartupFailure("EditorState: window creation failed");
        return false;
    }

    HWND hwnd = m_window->GetHWND();
    if (!hwnd)
    {
        LogStartupFailure("EditorState: window handle is invalid");
        return false;
    }

    // Initialize renderer
    OutputDebugStringA("[EditorState] Creating renderer...\n");
    try
    {
        m_renderer = ::Engine::Renderers::RendererFactory::CreateEditorRenderer(m_projectSettings);
    }
    catch (const std::exception& e)
    {
        std::string api = m_projectSettings.editorRenderingAPI;
        std::string msg = "Failed to create the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU supports it.\n\n"
            "Details: " + e.what();
        MessageBoxA(hwnd, msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!m_renderer)
    {
        std::string api = m_projectSettings.editorRenderingAPI;
        std::string msg = "Failed to create the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU supports it.";
        MessageBoxA(hwnd, msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }

    OutputDebugStringA("[EditorState] Initializing renderer...\n");
    if (!m_renderer->Init(hwnd, 1280, 720))
    {
        LogStartupFailure("EditorState: renderer Init returned false");
        std::string api = m_projectSettings.editorRenderingAPI;
        std::string msg = "Failed to initialize the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU driver is up to date.";
        MessageBoxA(hwnd, msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    OutputDebugStringA("[EditorState] Renderer initialized\n");

    // Initialize scene
    OutputDebugStringA("[EditorState] Creating scene...\n");
    m_scene = std::make_unique<Engine::Scene::Scene>();
    if (!m_scene)
        return false;
    OutputDebugStringA("[EditorState] Scene created\n");

    OutputDebugStringA("[EditorState] Initializing scene...\n");
    Engine::Graphics::IGraphicsProvider* graphicsProvider = m_renderer->GetGraphicsProvider();
    if (!graphicsProvider)

    {
        LogStartupFailure("EditorState: renderer did not provide a graphics provider");
        OutputDebugStringA("[EditorState] ERROR: Failed to get graphics provider from renderer\n");
        return false;
    }

    try
    {
        m_scene->Init(graphicsProvider);
        m_scene->GetPhysics().ConfigureStepBudget(
            m_projectSettings.physicsTargetMilliseconds,
            m_projectSettings.physicsMaximumSubsteps);
        m_scene->SetRealtimeShadowSettings(m_projectSettings.realtimeShadows);
        m_scene->SetDistanceLightingSettings(m_projectSettings.distanceLighting);
        OutputDebugStringA("[EditorState] Scene initialized\n");
    }
    catch (const std::exception& e)
    {
        LogStartupFailure(std::string("EditorState: scene initialization failed: ") + e.what());
        std::string errorMsg = "[EditorState] ERROR: Scene initialization failed: ";
        errorMsg += e.what();
        errorMsg += "\n";
        OutputDebugStringA(errorMsg.c_str());
        return false;
    }

    // Initialize view factory
    OutputDebugStringA("[EditorState] Creating view factory...\n");
    m_viewFactory = std::make_unique<ViewFactory>(m_renderer.get(), m_scene.get(), m_projectSettings);
    if (!m_viewFactory)
        return false;
    OutputDebugStringA("[EditorState] View factory created\n");

    // Initialize timing
    QueryPerformanceFrequency(&m_perfFreq);
    QueryPerformanceCounter(&m_lastCounter);
    m_deltaTime = 0.0f;
    OutputDebugStringA("[EditorState] Timing initialized\n");

    OutputDebugStringA("[EditorState] Init completed\n");
    return true;
}

void EditorState::InitializeUiState()
{
    if (!m_panels.empty() || m_preferences)
        return;
    WireupCallbacks();
    InitializePanels();
}

void EditorState::ReleaseUiPanels()
{
    m_primaryConsole = nullptr;
    m_primaryHierarchy = nullptr;
    m_primaryProperties = nullptr;
    m_primaryAssets = nullptr;
    m_prefabSceneView = nullptr;
    m_panels.clear();
    m_viewFactory.reset();
}

void EditorState::CheckAssetLocations()
{
    if (m_primaryAssets)
        m_primaryAssets->CheckForExternalMoves();
}

bool EditorState::PollBackgroundWork()
{
    const bool saved = PollSceneSave();
    const bool imported = PollImportJob();
    const bool baked = PollBakeJob();
    bool openedPrefab = false;
    if (m_prefabRead.valid() &&
        m_prefabRead.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const std::string path = std::exchange(m_prefabReadPath, std::string{});
        try
        {
            auto document = m_prefabRead.get();
            OpenPrefabStageWithDocument(path, document);
        }
        catch (const std::exception& error)
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    std::string("Failed to read prefab: ") + error.what());
        }
        openedPrefab = true;
    }
    const bool assets = m_primaryAssets && m_primaryAssets->PollDirectoryCache();
    bool preparingPrefab = false;
    for (const auto& panel : m_panels)
        if (const auto* view = dynamic_cast<const SceneView*>(panel.get()))
            preparingPrefab |= view->HasPendingPrefabPreparation() ||
                (view->IsRenderVisible() && view->GetScene() &&
                    view->GetScene()->HasPendingGraphicsPreparation());
    return saved || imported || baked || m_stageImport.valid() ||
        openedPrefab || assets || preparingPrefab;
}

void EditorState::StartSceneSave(SceneSaveRequest request)
{
    m_sceneSave = Engine::Rendering::RenderJobs::Get().Submit(
        [request = std::move(request)]() mutable {
            SceneSaveResult result;
            result.path = std::move(request.path);
            result.revision = request.revision;
            result.generation = request.generation;
            try
            {
                result.snapshot = Engine::Serialization::SceneSerializer::EncodeSceneSnapshot(request.document);
                const std::string xml = Engine::Serialization::SceneSerializer::EncodeSceneFile(request.document);
                std::ofstream file(result.path, std::ios::binary | std::ios::trunc);
                if (file)
                {
                    file.write(xml.data(), static_cast<std::streamsize>(xml.size()));
                    file.flush();
                    result.succeeded = file.good();
                }
            }
            catch (...) { result.succeeded = false; }
            return result;
        });
}

bool EditorState::PollSceneSave()
{
    if (!m_sceneSave.valid() || m_sceneSave.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) return false;
    SceneSaveResult result = m_sceneSave.get();
    FrameActivity::Mark(FrameActivity::Save);
    if (result.generation != m_sceneDocumentGeneration)
    {
        m_nextSceneSave.reset();
        return true;
    }
    if (result.succeeded)
    {
        m_currentScenePath = NormalizeAssetPath(result.path);
        RememberLastScene(m_projectFilePath, m_currentScenePath);
        m_savedSceneSnapshot = std::move(result.snapshot);
        m_savedSceneRevision = result.revision;
        m_hasUnsavedChanges = result.revision != m_sceneEditRevision;
        RefreshSceneDocumentTitle();
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Scene saved: " + m_currentScenePath);
        if (!m_hasUnsavedChanges && m_scene)
            AssetPreviewCache::CaptureScene(m_currentScenePath, *m_scene,
                m_scene->GetGraphicsProvider());
    }
    else if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Error,
            "Failed to save scene: " + result.path);
    if (m_nextSceneSave)
    {
        auto next = std::move(*m_nextSceneSave);
        m_nextSceneSave.reset();
        StartSceneSave(std::move(next));
    }
    else if (!m_loadAfterSceneSave.empty())
    {
        if (result.succeeded)
            QueueSceneLoad(std::exchange(m_loadAfterSceneSave, {}));
        else
        {
            m_sceneToLoad = std::exchange(m_loadAfterSceneSave, {});
            m_showUnsavedWarning = true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Mesh editing is implemented in State/EditorStateMeshEditing.cpp.
// ---------------------------------------------------------------------------
void EditorState::SaveScene()
{
    FrameActivity::Mark(FrameActivity::Save);
    if (!m_playModeSceneSnapshot.empty())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Scenes cannot be saved during Play mode. Stop the game first.");
        return;
    }

    if (m_activeAnimationDocument)
    {
        SaveAnimationDocument(*m_activeAnimationDocument);
        return;
    }

    if (SkeletonEditSession* skeleton = ActiveSkeletonEditSession())
        FinishSkeletonPaintStroke(*skeleton);

    if (MeshEditSession* meshEdit = ActiveMeshEditSession();
        meshEdit && (meshEdit->enabled ||
            (ActiveSkeletonEditSession() &&
                ActiveSkeletonEditSession()->enabled)) && meshEdit->dirty &&
        !SaveMeshEditSession(*meshEdit))
        return;
    if (!SavePendingMeshEdits()) return;

    if (m_activeAssetDocument && !m_activeSceneAssetDocument &&
        (m_activePrefabPath.empty() || !m_prefabDocumentFocused))
    {
        if (!m_activeAssetDocument->Save() && m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Failed to save asset: " + m_activeAssetDocument->GetPath());
        return;
    }

    if (m_primaryProperties && !m_primaryProperties->CommitPendingAssetRename())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not rename selected asset. Check its Filename in Properties.");
        return;
    }

    if (m_activeSceneAssetDocument)
    {
        SaveSceneAssetDocument(*m_activeSceneAssetDocument);
        return;
    }

    if (!m_activePrefabPath.empty() && m_prefabDocumentFocused)
    {
        if (!m_prefabScene)
            return;
        Engine::Core::Object* root = nullptr;
        for (const auto& object : m_prefabScene->GetObjects())
            if (object && !object->Parent)
            {
                if (root)
                {
                    if (m_primaryConsole)
                        m_primaryConsole->AddLog(ConsoleView::Level::Error,
                            "Prefab stage must contain exactly one root object.");
                    return;
                }
                root = object.get();
            }
        const bool prefabSaved = root && Engine::Rendering::RenderJobs::Get().Submit(
            Engine::Serialization::SceneSerializer::PreparePrefabSave(
                *root, m_activePrefabPath)).get();
        if (!prefabSaved)
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Failed to save prefab: " + m_activePrefabPath);
            return;
        }
        SetPrefabDirty(false);
        if (m_scene)
        {
            ::Engine::Scene::Scene::ObjectPath selectionPath;
            const bool hadSelection = m_scene->GetSelectedObject() &&
                m_scene->TryGetObjectPath(m_scene->GetSelectedObject(), selectionPath);
            if (!Engine::Serialization::SceneSerializer::RefreshPrefabInstances(*m_scene,
                m_activePrefabPath, m_scene->GetGraphicsProvider()) && m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Prefab saved, but scene instances could not be refreshed.");
            if (hadSelection)
                RefreshSelectionAfterReload(selectionPath);
        }
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Prefab saved: " + m_activePrefabPath);
        return;
    }

    SaveMainScene();
}

bool EditorState::SaveMainScene()
{
    FrameActivity::Mark(FrameActivity::Save);
    if (!m_scene || !m_playModeSceneSnapshot.empty())
        return false;

    std::string destination = m_currentScenePath;
    if (destination.empty() && !m_projectSettings.defaultScene.empty())
        destination = m_projectSettings.defaultScene;

    if (destination.empty())
    {
        wchar_t filePath[MAX_PATH] = {};
        std::filesystem::path initialDirectory = m_projectSettings.sceneDirectory.empty()
            ? std::filesystem::current_path()
            : std::filesystem::path(m_projectSettings.sceneDirectory);
        std::wstring initial = initialDirectory.wstring();
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window ? m_window->GetHWND() : nullptr;
        dialog.lpstrFile = filePath;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrFilter = L"Scene Files (*.scene;*.xml)\0*.scene;*.xml\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"scene";
        dialog.lpstrInitialDir = initial.c_str();
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog))
            return false;
        destination = std::filesystem::path(filePath).string();
    }

    if (std::filesystem::path(destination).extension().empty())
        destination += ".scene";

    try
    {
        SceneSaveRequest request{Engine::Serialization::SceneSerializer::CaptureSceneDocument(*m_scene),
            destination, m_sceneEditRevision, m_sceneDocumentGeneration};
        if (m_sceneSave.valid()) m_nextSceneSave = std::move(request);
        else StartSceneSave(std::move(request));
        return true;
    }
    catch (const std::exception& error)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Failed to capture scene for save: " + std::string(error.what()));
    }

    return false;
}

void EditorState::SaveAll()
{
    FrameActivity::Mark(FrameActivity::Save);
    if (!m_playModeSceneSnapshot.empty())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Documents cannot be saved during Play mode. Stop the game first.");
        return;
    }

    if (!SavePendingMeshEdits()) return;

    for (AssetDocumentView* document : m_assetDocuments)
        if (document && (document->IsDirty() || document->HasPendingRename()) &&
            !document->Save())
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not save asset: " + document->GetPath());
            return;
        }
    if (m_primaryProperties && !m_primaryProperties->CommitPendingAssetRename())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not rename selected asset. Check its Filename in Properties.");
        return;
    }

    for (auto& document : m_sceneAssetDocuments)
        if (document && (document->dirty || document->meshEdit.dirty))
            SaveSceneAssetDocument(*document);

    for (auto& document : m_animationDocuments)
        if (document && document->dirty)
            SaveAnimationDocument(*document);

    if (!m_activePrefabPath.empty())
    {
        const bool previousFocus = m_prefabDocumentFocused;
        AssetDocumentView* previousAssetDocument = m_activeAssetDocument;
        m_activeAssetDocument = nullptr;
        m_prefabDocumentFocused = true;
        SaveScene();
        m_prefabDocumentFocused = previousFocus;
        m_activeAssetDocument = previousAssetDocument;
    }
    SaveMainScene();
    const bool projectSaved = !m_preferences || m_projectFilePath.empty()
        ? true : m_preferences->SaveSettings();
    if (m_primaryConsole)
        m_primaryConsole->AddLog(projectSaved
            ? ConsoleView::Level::Info : ConsoleView::Level::Error,
            projectSaved
                ? "Saved all open documents and project settings."
                : "Save All completed with errors.");
}

std::string EditorState::GetActiveDocumentName() const
{
    if (m_activeAnimationDocument)
    {
        if (m_activeAnimationDocument->path.empty())
            return "Animation Editor";
        std::string title = "Animation: " + std::filesystem::path(
            m_activeAnimationDocument->path).filename().string();
        if (m_activeAnimationDocument->dirty) title += " *";
        return title;
    }
    if (m_activeSceneAssetDocument)
    {
        std::string title = std::filesystem::path(
            m_activeSceneAssetDocument->path).filename().string();
        if (m_activeSceneAssetDocument->dirty) title += " *";
        return title;
    }
    if (m_activeAssetDocument)
    {
        std::string title = std::filesystem::path(
            m_activeAssetDocument->GetPath()).filename().string();
        if (m_activeAssetDocument->IsDirty())
            title += " *";
        return title;
    }
    std::string sceneName = m_currentScenePath.empty()
        ? "Untitled" : std::filesystem::path(m_currentScenePath).filename().string();
    if (m_hasUnsavedChanges)
        sceneName += " *";
    if (m_activePrefabPath.empty())
        return sceneName;

    std::string prefabName =
        std::filesystem::path(m_activePrefabPath).filename().string();
    if (m_prefabHasUnsavedChanges || m_prefabMeshEdit.dirty)
        prefabName += " *";

    return sceneName + " | " + prefabName;
}

// ---------------------------------------------------------------------------
// EditorState::LoadScene
// ---------------------------------------------------------------------------
void EditorState::LoadScene(const std::string& path)
{
    RequestSceneLoad(path);
}

void EditorState::LoadStartupScene(const std::string& path, bool rememberScene)
{
    try
    {
        auto parsed = Engine::Rendering::RenderJobs::Get().Submit([path] {
            return Engine::Serialization::SceneSerializer::ReadSceneFile(path);
        }).get();
        LoadSceneNow(path, &parsed, rememberScene);
    }
    catch (const std::exception& error)
    {
        LogStartupFailure(std::string("Failed to read startup scene: ") + error.what());
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error, error.what());
    }
}

void EditorState::RequestSceneLoad(const std::string& path)
{
    if (!m_scene)
    {
        OutputDebugStringA("[EditorState::LoadScene] ERROR: Scene is null\n");
        return;
    }

    if (m_hasUnsavedChanges)
    {
        m_sceneToLoad = path;
        m_showUnsavedWarning = true;
        if (m_renderer)
            m_renderer->MarkDirty();
        return;
    }

    QueueSceneLoad(path);
}

bool EditorState::ConfirmSceneLoad(bool saveCurrentScene)
{
    if (saveCurrentScene && !SaveMainScene())
        return false;

    const std::string path = std::move(m_sceneToLoad);
    m_sceneToLoad.clear();
    m_showUnsavedWarning = false;
    if (m_renderer)
        m_renderer->MarkDirty();
    if (saveCurrentScene) m_loadAfterSceneSave = path;
    else QueueSceneLoad(path);
    return true;
}

void EditorState::QueueSceneLoad(const std::string& path)
{
    if (path.empty()) return;
    m_queuedScenePath = path;
    m_sceneLoadingMessage = "Loading scene: " + std::filesystem::path(path).filename().string();
    m_queuedSceneDocument = Engine::Rendering::RenderJobs::Get().Submit([path] {
        return Engine::Serialization::SceneSerializer::ReadSceneFile(path);
    });
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::ProcessPendingSceneLoad()
{
    if (m_queuedScenePath.empty() || !m_queuedSceneDocument.valid() ||
        m_queuedSceneDocument.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready) return;
    const std::string path = std::move(m_queuedScenePath);
    m_queuedScenePath.clear();
    try
    {
        auto parsed = m_queuedSceneDocument.get();
        LoadSceneNow(path, &parsed);
    }
    catch (const std::exception& error)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                std::string("Failed to read scene: ") + error.what());
    }
    m_sceneLoadingMessage.clear();
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::CancelSceneLoad()
{
    m_sceneToLoad.clear();
    m_showUnsavedWarning = false;
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::LoadSceneNow(const std::string& path,
    const Engine::Serialization::JsonValue* parsed, bool rememberScene)
{
    FrameActivity::Mark(FrameActivity::Load);

    OutputDebugStringA(("[EditorState::LoadScene] Loading scene: " + path + "\n").c_str());

    // Check if file exists
    std::string resolvedPath = path;
    if (!std::filesystem::exists(resolvedPath))
    {
        OutputDebugStringA(("[EditorState::LoadScene] File not found at: " + resolvedPath + "\n").c_str());
        OutputDebugStringA(("[EditorState::LoadScene] Current directory: " + std::filesystem::current_path().string() + "\n").c_str());

        if (m_primaryConsole)
        {
            m_primaryConsole->AddLog(ConsoleView::Level::Error, "Scene file not found: " + resolvedPath);
            m_primaryConsole->AddLog(ConsoleView::Level::Info, "Current directory: " + std::filesystem::current_path().string());
        }
        return;
    }

    // Loading another editor document is a hard runtime boundary. Let the
    // host perform its normal Stop flow before this scene replaces the active
    // graph, so Playing/Paused state and the pre-play snapshot cannot leak
    // into the newly loaded scene.
    if (OnSceneLoadRequested)
        OnSceneLoadRequested(resolvedPath);

    // Load the scene
    try
    {
        if (parsed ? m_scene->LoadFromValue(*parsed) : m_scene->Load(resolvedPath))
        {
            ++m_sceneDocumentGeneration;
            m_nextSceneSave.reset();
            m_loadAfterSceneSave.clear();
            m_mainMeshEdit = {};
            m_mainSkeletonEdit = {};
            m_scene->SetEditorSelectedMesh(nullptr);
            m_scene->SetEditorWeightPaint(nullptr, -1);
            m_scene->SetEditorMeshEditPose(false);
            OutputDebugStringA("[EditorState::LoadScene] Scene loaded successfully\n");
            LogStartupFailure("Scene loaded successfully: " + resolvedPath);
            if (m_preferences)
                m_preferences->SetSpatialDebugVisuals(
                    m_scene->settings.portalDebugVisuals);
            m_hasUnsavedChanges = false;
            m_currentScenePath = NormalizeAssetPath(resolvedPath);
            if (rememberScene) RememberLastScene(m_projectFilePath, m_currentScenePath);
            RefreshSceneDocumentTitle();
            ResetHistory(true);
            if (OnSceneLoadConfirmed)
                OnSceneLoadConfirmed();

            if (m_primaryConsole)
            {
                m_primaryConsole->AddLog(ConsoleView::Level::Info, "Scene loaded: " + resolvedPath);
            }
            return;
        }
        else
        {
            OutputDebugStringA("[EditorState::LoadScene] Failed to load scene\n");
            LogStartupFailure("Failed to load scene: " + resolvedPath);
            if (m_primaryConsole)
            {
                m_primaryConsole->AddLog(ConsoleView::Level::Error, "Failed to load scene: " + resolvedPath);
            }
            return;
        }
    }
    catch (const std::exception& e)
    {
        std::string errorMsg = "[EditorState::LoadScene] Exception during scene loading: ";
        errorMsg += e.what();
        errorMsg += "\n";
        OutputDebugStringA(errorMsg.c_str());

        if (m_primaryConsole)
        {
            m_primaryConsole->AddLog(ConsoleView::Level::Error, "Exception loading scene: " + std::string(e.what()));
        }
    }
}

bool EditorState::CapturePlayModeScene()
{
    if (!m_scene || !m_playModeSceneSnapshot.empty()) return false;

    FinishSkeletonPaintStroke(m_mainSkeletonEdit);
    TrackSceneChanges(true, false);
    FlushHistoryCapture();
    CommitPendingHistoryEdit();
    m_playModeSceneSnapshot = !m_activeSceneAssetDocument &&
        !m_activeAnimationDocument && !m_historyBaseline.scene.empty() &&
        m_historyCapturedRevision == m_sceneEditRevision
        ? m_historyBaseline.scene : CaptureHistoryEntry().scene;
    // Parse the captured binary tree while Play runs. The scene itself stays
    // on its owner thread; only immutable snapshot bytes cross this boundary.
    try
    {
        m_playModeParsedSnapshot = std::async(std::launch::async,
            [snapshot = m_playModeSceneSnapshot] {
                return Engine::Serialization::AssetParse(snapshot);
            });
    }
    catch (const std::system_error&)
    {
        // Stop can still use the ordinary snapshot loader if no worker starts.
    }
    m_prePlayHasUnsavedChanges = m_hasUnsavedChanges;
    m_prePlayHadObjectSelection = false;
    m_prePlaySelectionPath.clear();
    for (const auto& panel : m_panels)
    {
        const auto* hierarchy = dynamic_cast<const HierarchyView*>(panel.get());
        if (!hierarchy) continue;
        auto* selected = hierarchy->GetSelectedObject();
        m_prePlayHadObjectSelection = selected &&
            m_scene->TryGetObjectPath(selected, m_prePlaySelectionPath);
        break;
    }

    OutputDebugStringA("[Play] Captured editor scene state.\n");
    return true;
}

void EditorState::RestorePlayModeScene()
{
    if (!m_scene || m_playModeSceneSnapshot.empty()) return;
    const auto restoreBegin = std::chrono::steady_clock::now();

    for (auto& panel : m_panels)
        if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
            hierarchy->SetSelectedObject(nullptr);
    if (m_primaryProperties) m_primaryProperties->SetSelectedObject(nullptr);

    bool restored = false;
    bool usedCachedSnapshot = false;
    if (m_playModeParsedSnapshot.valid())
    {
        try
        {
            const auto snapshot = m_playModeParsedSnapshot.get();
            restored = m_scene->RestoreTrustedSnapshot(snapshot);
            usedCachedSnapshot = restored;
        }
        catch (const std::exception& error)
        {
            OutputDebugStringA((std::string("[Play] Cached snapshot unavailable: ") +
                error.what() + "\n").c_str());
        }
    }
    if (!restored)
        restored = m_scene->LoadFromString(m_playModeSceneSnapshot);

    if (restored)
    {
        m_mainSkeletonEdit.skeleton = nullptr;
        m_mainSkeletonEdit.bindingMesh = nullptr;
        m_mainSkeletonEdit.mesh = nullptr;
        m_mainSkeletonEdit.adjacency.reset();
        m_mainSkeletonEdit.observedBindTransforms.clear();
        m_mainSkeletonEdit.observedSubmode = -1;
        m_mainMeshEdit.activeMesh = nullptr;
        m_scene->SetEditorWeightPaint(nullptr, -1);
        m_scene->SetEditorMeshEditPose(m_mainMeshEdit.enabled ||
            m_mainSkeletonEdit.enabled);
        m_hasUnsavedChanges = m_prePlayHasUnsavedChanges;
        RefreshSceneDocumentTitle();
        SelectObject(m_prePlayHadObjectSelection
            ? m_scene->FindObjectByPath(m_prePlaySelectionPath) : nullptr);
        if (m_mainSkeletonEdit.enabled)
        {
            SyncSkeletonEditSelection(m_scene.get(), m_mainSkeletonEdit);
            if (m_mainMeshEdit.dirty && m_mainSkeletonEdit.mesh &&
                RestoreMeshSnapshot(*m_mainSkeletonEdit.mesh,
                    m_mainMeshEdit.baseline))
                m_mainMeshEdit.activeMesh = m_mainSkeletonEdit.mesh;
        }
        m_historyBaseline.scene = m_playModeSceneSnapshot;
        CaptureHistorySelection(m_historyBaseline);
        m_historyCapturedRevision = m_sceneEditRevision;
        m_historySelectionDirty = false;
        m_sceneEditInProgress = false;
        if (m_renderer) m_renderer->MarkDirty();
        OutputDebugStringA("[Play] Restored editor scene state.\n");
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "[Play] Restored pre-play scene state in " +
                std::to_string(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - restoreBegin).count()) +
                " ms" + (usedCachedSnapshot ? " from cached snapshot." : "."));
    }
    else
    {
        OutputDebugStringA("[Play] ERROR: Failed to restore editor scene state.\n");
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "[Play] Failed to restore pre-play scene state.");
    }

    m_playModeSceneSnapshot.clear();
    m_prePlaySelectionPath.clear();
    m_prePlayHadObjectSelection = false;
}

void EditorState::RefreshSelectionAfterReload(const ::Engine::Scene::Scene::ObjectPath& selectedPath)
{
    SelectObject(m_scene ? m_scene->FindObjectByPath(selectedPath) : nullptr);
}

Engine::Scene::Scene* EditorState::GetActiveDocumentScene() const
{
    if (m_activeAnimationDocument && m_activeAnimationDocument->poseScene)
        return m_activeAnimationDocument->poseScene.get();
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->scene)
        return m_activeSceneAssetDocument->scene.get();
    if (m_prefabDocumentFocused && m_prefabScene)
        return m_prefabScene.get();
    return m_scene.get();
}

void EditorState::SetPrefabDocumentFocused(bool focused)
{
    const bool nextFocused = focused && m_prefabScene && m_prefabSceneView;
    const bool contextChanged = m_prefabDocumentFocused != nextFocused ||
        m_activeAssetDocument != nullptr ||
        m_activeAnimationDocument != nullptr ||
        (nextFocused && m_activeSceneAssetDocument != nullptr);
    m_activeAssetDocument = nullptr;
    m_activeAnimationDocument = nullptr;
    if (nextFocused)
        m_activeSceneAssetDocument = nullptr;
    m_prefabDocumentFocused = nextFocused;
    if (!contextChanged)
        return;
    Engine::Scene::Scene* activeScene = GetActiveDocumentScene();
    Engine::Core::Object* selected = activeScene
        ? activeScene->GetSelectedObject() : nullptr;

    if (m_primaryHierarchy)
    {
        m_primaryHierarchy->Init(activeScene);
        m_primaryHierarchy->SetSelectedObject(selected);
    }
    if (m_primaryProperties)
    {
        m_primaryProperties->Init(activeScene);
        m_primaryProperties->SetSelectedObject(selected);
        m_primaryProperties->SetSelectedAsset("");
    }
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::SetPrefabDirty(bool dirty)
{
    if (m_prefabHasUnsavedChanges == dirty)
        return;
    m_prefabHasUnsavedChanges = dirty;
    RefreshPrefabDocumentTitle();
}

void EditorState::RefreshPrefabDocumentTitle()
{
    if (!m_prefabSceneView || m_activePrefabPath.empty())
        return;
    std::string title = std::filesystem::path(
        m_activePrefabPath).filename().string();
    if (m_prefabHasUnsavedChanges)
        title += " *";
    m_prefabSceneView->SetTitle(title + "###PrefabDocument:" +
        AssetPathIdentity(m_activePrefabPath));
}

void EditorState::SelectObject(Engine::Core::Object* object)
{
    if (m_primaryAssets)
        m_primaryAssets->SetSelectedPath({});
    if (m_primaryHierarchy)
        m_primaryHierarchy->SetSelectedObject(object);
    if (m_primaryProperties)
        m_primaryProperties->SetSelectedObject(object);
    if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
        scene->SetSelectedObject(object);
    MarkHistorySelectionChanged();
}

// ---------------------------------------------------------------------------
// EditorState::GetDeltaTime
// ---------------------------------------------------------------------------
float EditorState::GetDeltaTime() const
{
    return m_deltaTime;
}

// ---------------------------------------------------------------------------
// EditorState::UpdateDeltaTime
// ---------------------------------------------------------------------------
void EditorState::UpdateDeltaTime()
{
    LARGE_INTEGER currentCounter;
    QueryPerformanceCounter(&currentCounter);

    // Calculate elapsed time in seconds
    LONGLONG elapsedCounts = currentCounter.QuadPart - m_lastCounter.QuadPart;
    m_deltaTime = static_cast<float>(elapsedCounts) / static_cast<float>(m_perfFreq.QuadPart);

    // Update last counter for next frame
    m_lastCounter = currentCounter;
}
}
