#include "pch.h"
#include "EditorState.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/View.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/SceneView.h"
#include "Core/View/Views/AssetDocumentView.h"
#include "Core/Window.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/AssetRecord.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Importers/ModelImporter.h"
#include "Engine/Editor/Core/View/Templates/Assets/AssetPreviewCache.h"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <commdlg.h>

namespace Engine::Editor
{
namespace
{
    void LogStartupFailure(const std::string& message)
    {
        std::ofstream log("editor-startup.log", std::ios::app);
        if (log)
            log << message << '\n';
    }

    std::string NormalizeAssetPath(const std::string& path)
    {
        std::error_code error;
        std::filesystem::path normalized =
            std::filesystem::weakly_canonical(path, error);
        if (error)
            normalized = std::filesystem::path(path).lexically_normal();
        return normalized.generic_string();
    }

    std::string AssetPathIdentity(const std::string& path)
    {
        std::string identity = NormalizeAssetPath(path);
#ifdef _WIN32
        std::transform(identity.begin(), identity.end(), identity.begin(),
            [](unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
#endif
        return identity;
    }

    bool ReplaceAllInText(std::string& text, const std::string& from,
        const std::string& to)
    {
        if (from.empty() || from == to)
            return false;
        bool replaced = false;
        size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos)
        {
            text.replace(position, from.size(), to);
            position += to.size();
            replaced = true;
        }
        return replaced;
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
    }
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
        m_window = std::make_unique<::Engine::Core::Window>(hInstance, L"Engine Editor", 1280, 720);
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
}

bool EditorState::HasUnsavedChanges() const
{
    if (m_hasUnsavedChanges || m_prefabHasUnsavedChanges)
        return true;
    for (const auto& document : m_sceneAssetDocuments)
        if (document && document->dirty)
            return true;
    for (AssetDocumentView* document : m_assetDocuments)
        if (document && document->IsDirty())
            return true;
    return false;
}

bool EditorState::HasActiveDocumentUnsavedChanges() const
{
    if (m_activeSceneAssetDocument)
        return m_activeSceneAssetDocument->dirty;
    if (m_prefabDocumentFocused && !m_activePrefabPath.empty())
        return m_prefabHasUnsavedChanges;
    if (m_activeAssetDocument)
        return m_activeAssetDocument->IsDirty();
    return m_hasUnsavedChanges;
}

bool EditorState::IsEditingPrefab() const
{
    return (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab) ||
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

// ---------------------------------------------------------------------------
// EditorState::SaveScene
// ---------------------------------------------------------------------------
void EditorState::SaveScene()
{
    if (!m_playModeSceneSnapshot.empty())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Scenes cannot be saved during Play mode. Stop the game first.");
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
        if (!root || !Engine::Serialization::SceneSerializer::SavePrefab(*root, m_activePrefabPath))
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

    if (m_activeAssetDocument)
    {
        if (!m_activeAssetDocument->Save() && m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Failed to save asset: " + m_activeAssetDocument->GetPath());
        return;
    }

    SaveMainScene();
}

bool EditorState::SaveMainScene()
{
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

    if (m_scene->Save(destination))
    {
        m_currentScenePath = NormalizeAssetPath(destination);
        m_hasUnsavedChanges = false;
        RefreshSceneDocumentTitle();
        m_savedSceneSnapshot = m_scene->SaveToString();
        AssetPreviewCache::CaptureScene(m_currentScenePath, *m_scene,
            m_scene->GetGraphicsProvider());
        if (m_primaryConsole)
            m_primaryConsole->AddLog(
                ConsoleView::Level::Info, "Scene saved: " + m_currentScenePath);
        return true;
    }
    else if (m_primaryConsole)
    {
        m_primaryConsole->AddLog(
            ConsoleView::Level::Error, "Failed to save scene: " + destination);
    }

    return false;
}

void EditorState::SaveAll()
{
    if (!m_playModeSceneSnapshot.empty())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Documents cannot be saved during Play mode. Stop the game first.");
        return;
    }

    for (AssetDocumentView* document : m_assetDocuments)
        if (document && document->IsDirty())
            document->Save();

    for (auto& document : m_sceneAssetDocuments)
        if (document && document->dirty)
            SaveSceneAssetDocument(*document);

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
    if (m_prefabHasUnsavedChanges)
        prefabName += " *";

    return sceneName + " | " + prefabName;
}

void EditorState::BakeLighting()
{
    if (!m_scene)
        return;
    const std::string assets = m_projectSettings.assetsDirectory.empty()
        ? "Assets" : m_projectSettings.assetsDirectory;
    std::string sceneName = m_currentScenePath.empty()
        ? m_projectSettings.name
        : std::filesystem::path(m_currentScenePath).stem().string();
    if (sceneName.empty())
        sceneName = "Scene";
    const auto result = m_scene->BakeLighting(
        assets, sceneName, m_projectSettings.bakedLighting);
    if (m_primaryConsole)
        m_primaryConsole->AddLog(result.succeeded
            ? ConsoleView::Level::Info : ConsoleView::Level::Error,
            result.message);
    if (result.succeeded)
    {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
    }
}

void EditorState::ClearBakedLighting()
{
    if (!m_scene)
        return;
    m_scene->ClearBakedLighting();
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            m_scene->GetLightingBakeStatus());
    m_hasUnsavedChanges = true;
    MarkSceneEdited();
}

// ---------------------------------------------------------------------------
// EditorState::LoadScene
// ---------------------------------------------------------------------------
void EditorState::LoadScene(const std::string& path)
{
    RequestSceneLoad(path);
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

    LoadSceneNow(path);
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
    LoadSceneNow(path);
    return true;
}

void EditorState::CancelSceneLoad()
{
    m_sceneToLoad.clear();
    m_showUnsavedWarning = false;
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::LoadSceneNow(const std::string& path)
{

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
        if (m_scene->Load(resolvedPath))
        {
            OutputDebugStringA("[EditorState::LoadScene] Scene loaded successfully\n");
            LogStartupFailure("Scene loaded successfully: " + resolvedPath);
            if (m_preferences)
                m_preferences->SetSpatialDebugVisuals(
                    m_scene->settings.portalDebugVisuals);
            m_hasUnsavedChanges = false;
            m_currentScenePath = NormalizeAssetPath(resolvedPath);
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

void EditorState::OpenPrefabStage(const std::string& path)
{
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
        root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            *prefabScene, normalized, prefabScene->GetGraphicsProvider());
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
    sceneView->OnFocused = [this]() { SetPrefabDocumentFocused(true); };
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

        auto document = std::make_unique<SceneAssetDocument>();
        document->path = normalized;
        document->identity = identity;
        std::string extension = std::filesystem::path(normalized).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
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
            const std::string assetsDirectory = m_projectSettings.assetsDirectory.empty()
                ? std::string("Assets") : m_projectSettings.assetsDirectory;
            const Engine::Model::ModelImportResult imported =
                ModelImporter::Import(normalized, assetsDirectory);
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
        if (loaded && document->prefab)
            RebuildSkeletonStageContext(*document);
        if (document->skeletonStage && !document->selectableObjects.empty())
        {
            if (!document->selectableObjects.contains(document->subject))
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
        raw->view->SetDocumentPath(normalized);
        RefreshSceneAssetDocumentTitle(*raw);
        view->OnFocused = [this, raw]() { SetActiveSceneAssetDocument(raw); };
        view->OnObjectSelected = [this, raw](Engine::Core::Object* object)
        {
            if (raw->skeletonStage && object &&
                !raw->selectableObjects.contains(object))
                return;
            raw->scene->SetSelectedObject(object);
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
        };
        view->OnObjectCreated = [this, raw](Engine::Core::Object* object)
        {
            raw->scene->SetSelectedObject(object);
            const std::string current = CaptureSceneAssetDocumentSnapshot(*raw);
            if (raw->baseline != current)
            {
                if (m_historyLimit > 0)
                {
                    if (raw->undo.size() >= m_historyLimit) raw->undo.pop_front();
                    raw->undo.push_back(raw->baseline);
                }
                raw->redo.clear();
                raw->baseline = current;
                raw->dirty = raw->baseline != raw->savedSnapshot;
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
            return !raw->skeletonStage || !object ||
                raw->selectableObjects.contains(
                    const_cast<Engine::Core::Object*>(object));
        };
        if (raw->skeletonStage)
            view->OnDrawDocumentTools = [raw](IEditorUi& ui)
            {
                ui.Separator();
                ui.ColoredLabel("Skeleton Edit Stage", { 0.35f, 0.75f, 1.f, 1.f });
                const std::string count = std::to_string(raw->selectableObjects.size());
                ui.ValueLabel("Editable joints", count.c_str());
                ui.DisabledLabel("Joint hierarchy is filtered to this skeleton.");
            };
        if (raw->meshStage)
        {
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
            view->AllowObjectTransform = false;
            view->OnDrawDocumentTools = [this, raw](IEditorUi& ui)
            {
                if (!raw->mesh || raw->mesh->GetVertices().empty())
                {
                    ui.ColoredLabel("Mesh has no editable triangle-list vertices.",
                        { 1.f, 0.6f, 0.25f, 1.f });
                    return;
                }
                std::vector<Engine::Components::Mesh::Vertex> vertices =
                    raw->mesh->GetVertices();
                ui.Separator();
                ui.ColoredLabel("Mesh Edit Stage", { 0.35f, 0.75f, 1.f, 1.f });
                ui.ValueLabel("Save target", raw->meshSavePath.c_str());
                const std::string count = std::to_string(vertices.size());
                ui.ValueLabel("Triangle-list vertices", count.c_str());
                int selected = static_cast<int>(std::min<uint32_t>(
                    raw->selectedVertex, static_cast<uint32_t>(vertices.size() - 1)));
                if (vertices.size() > 1 && ui.SliderInt("Vertex", &selected, 0,
                    static_cast<int>(vertices.size() - 1)))
                    raw->selectedVertex = static_cast<uint32_t>(selected);
                auto& vertex = vertices[raw->selectedVertex];
                bool changed = ui.DragFloat3("Position", vertex.pos, 0.01f);
                changed |= ui.DragFloat("UV U", &vertex.uv[0], 0.005f);
                changed |= ui.DragFloat("UV V", &vertex.uv[1], 0.005f);
                changed |= ui.ColorEdit4("Vertex Color", vertex.color);
                int selectedInfluence = static_cast<int>(
                    std::min(raw->selectedInfluence, 7u));
                if (ui.SliderInt("Skin influence", &selectedInfluence, 0, 7))
                    raw->selectedInfluence = static_cast<uint32_t>(selectedInfluence);
                const uint32_t influenceIndex = raw->selectedInfluence % 4u;
                float* joint = raw->selectedInfluence < 4u
                    ? &vertex.joints0[influenceIndex]
                    : &vertex.joints1[influenceIndex];
                float* weight = raw->selectedInfluence < 4u
                    ? &vertex.weights0[influenceIndex]
                    : &vertex.weights1[influenceIndex];
                changed |= ui.DragFloat("Joint palette index", joint, 1.f, 0.f, 255.f);
                changed |= ui.SliderFloat("Influence weight", weight, 0.f, 1.f);
                if (changed)
                {
                    float total = 0.f;
                    for (float value : vertex.weights0) total += value;
                    for (float value : vertex.weights1) total += value;
                    if (total > 0.f)
                    {
                        for (float& value : vertex.weights0) value /= total;
                        for (float& value : vertex.weights1) value /= total;
                    }
                }
                if (changed)
                {
                    raw->mesh->SetAuthoredVertices(std::move(vertices));
                    if (m_renderer) m_renderer->MarkDirty();
                }
                if (ui.Button("Save Mesh"))
                    SaveSceneAssetDocument(*raw);
            };
        }
        view->RequestFocusOnNextDraw();
        m_panels.push_back(std::move(view));
        m_sceneAssetDocuments.push_back(std::move(document));
        SetActiveSceneAssetDocument(raw);
    }
}

void EditorState::SetActiveSceneAssetDocument(SceneAssetDocument* document)
{
    m_activeSceneAssetDocument = document;
    m_activeAssetDocument = nullptr;
    m_prefabDocumentFocused = document && document->prefab;
    const auto scene = document ? document->scene.get() : m_scene.get();
    if (m_primaryHierarchy)
    {
        m_primaryHierarchy->Init(scene);
        m_primaryHierarchy->SetObjectFilter(document && document->skeletonStage
            ? std::function<bool(const Engine::Core::Object*)>(
                [document](const Engine::Core::Object* object)
                {
                    return document->selectableObjects.contains(
                        const_cast<Engine::Core::Object*>(object));
                })
            : std::function<bool(const Engine::Core::Object*)>{});
        Engine::Core::Object* selected = scene ? scene->GetSelectedObject() : nullptr;
        if (document && document->skeletonStage && selected &&
            !document->selectableObjects.contains(selected))
            selected = document->subject;
        m_primaryHierarchy->SetSelectedObject(selected);
    }
    if (m_primaryProperties)
    {
        m_primaryProperties->Init(scene);
        m_primaryProperties->SetSelectedObject(scene ? scene->GetSelectedObject() : nullptr);
        m_primaryProperties->SetSelectedAsset("");
    }
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::RefreshSceneAssetDocumentTitle(SceneAssetDocument& document)
{
    if (!document.view) return;
    std::string title = std::filesystem::path(document.path).filename().string();
    if (document.dirty) title += " *";
    document.view->SetTitle(title + "###SceneAssetDocument:" + document.identity);
}

bool EditorState::SaveSceneAssetDocument(SceneAssetDocument& document)
{
    bool saved = false;
    const std::string stageSavePath = document.stageDataPath.empty()
        ? document.path : document.stageDataPath;
    if (document.meshStage && document.mesh)
        saved = Engine::Components::Mesh::SaveNativeFile(
            document.meshSavePath, document.mesh->GetVertices());
    else if (document.scene && document.prefab)
    {
        Engine::Core::Object* root = nullptr;
        for (const auto& object : document.scene->GetObjects())
            if (object && !object->Parent)
            {
                if (root) { root = nullptr; break; }
                root = object.get();
            }
        saved = root && Engine::Serialization::SceneSerializer::SavePrefab(
            *root, stageSavePath);
    }
    else if (document.scene)
        saved = document.scene->Save(document.path);
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
    return true;
}

std::string EditorState::CaptureSceneAssetDocumentSnapshot(
    SceneAssetDocument& document)
{
    if (document.skeletonStage)
    {
        RestoreSkeletonStageVisibility(document);
        const std::string snapshot = document.scene
            ? document.scene->SaveToString() : std::string{};
        ApplySkeletonStageVisibility(document);
        return snapshot;
    }
    if (!document.meshStage || !document.mesh)
        return document.scene ? document.scene->SaveToString() : std::string{};
    const auto& vertices = document.mesh->GetVertices();
    return std::string(reinterpret_cast<const char*>(vertices.data()),
        vertices.size() * sizeof(Engine::Components::Mesh::Vertex));
}

bool EditorState::RestoreSceneAssetDocumentSnapshot(
    SceneAssetDocument& document, const std::string& snapshot)
{
    if (!document.meshStage)
    {
        if (!document.scene || !document.scene->LoadFromString(snapshot))
            return false;
        if (document.skeletonStage)
            RebuildSkeletonStageContext(document);
        return true;
    }
    if (!document.mesh || snapshot.empty() ||
        snapshot.size() % sizeof(Engine::Components::Mesh::Vertex) != 0)
        return false;
    std::vector<Engine::Components::Mesh::Vertex> vertices(
        snapshot.size() / sizeof(Engine::Components::Mesh::Vertex));
    std::memcpy(vertices.data(), snapshot.data(), snapshot.size());
    return document.mesh->SetAuthoredVertices(std::move(vertices));
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
                original->second && visible.contains(object.get());
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
        !document.selectableObjects.contains(document.subject))
        document.subject = *document.selectableObjects.begin();
    document.scene->SetSelectedObject(document.subject);
    ApplySkeletonStageVisibility(document);
}

void EditorState::HandleSceneAssetDocumentClosures()
{
    for (auto it = m_sceneAssetDocuments.begin(); it != m_sceneAssetDocuments.end();)
    {
        SceneAssetDocument* document = it->get();
        if (!document || document->view->IsOpen()) { ++it; continue; }
        if (document->dirty)
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
        (m_hasUnsavedChanges ? " *" : "") + "###SceneDocument:" +
        (m_currentScenePath.empty()
            ? std::string("untitled") : AssetPathIdentity(m_currentScenePath));
    for (const auto& panel : m_panels)
        if (auto* sceneView = dynamic_cast<SceneView*>(panel.get());
            sceneView && sceneView->GetScene() == m_scene.get())
        {
            sceneView->SetDocumentPath(m_currentScenePath);
            sceneView->SetTitle(title);
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

void EditorState::CapturePlayModeScene()
{
    if (!m_scene || !m_playModeSceneSnapshot.empty())
        return;

    // Finish any editor interaction before establishing the immutable play
    // baseline. Runtime changes must never enter the undo history.
    TrackSceneChanges(true, false);
    CommitPendingHistoryEdit();
    m_playModeSceneSnapshot = m_scene->SaveToString();
    m_prePlayHasUnsavedChanges = m_hasUnsavedChanges;
    m_prePlayHadObjectSelection = false;
    m_prePlaySelectionPath.clear();
    for (const auto& panel : m_panels)
    {
        const auto* hierarchy = dynamic_cast<const HierarchyView*>(panel.get());
        if (!hierarchy)
            continue;
        Engine::Core::Object* selected = hierarchy->GetSelectedObject();
        m_prePlayHadObjectSelection = selected &&
            m_scene->TryGetObjectPath(selected, m_prePlaySelectionPath);
        break;
    }
    OutputDebugStringA("[Play] Captured editor scene state.\n");
}

void EditorState::RestorePlayModeScene()
{
    if (!m_scene || m_playModeSceneSnapshot.empty())
        return;

    // Scene replacement invalidates object pointers held by editor panels.
    for (auto& panel : m_panels)
        if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
            hierarchy->SetSelectedObject(nullptr);
    if (m_primaryProperties)
        m_primaryProperties->SetSelectedObject(nullptr);

    if (m_scene->LoadFromString(m_playModeSceneSnapshot))
    {
        m_hasUnsavedChanges = m_prePlayHasUnsavedChanges;
        RefreshSceneDocumentTitle();
        SelectObject(m_prePlayHadObjectSelection
            ? m_scene->FindObjectByPath(m_prePlaySelectionPath)
            : nullptr);
        m_historyBaseline = CaptureHistoryEntry();
        m_historyCapturedRevision = m_sceneEditRevision;
        m_historySelectionDirty = false;
        m_sceneEditInProgress = false;
        if (m_renderer)
            m_renderer->MarkDirty();
        OutputDebugStringA("[Play] Restored editor scene state.\n");
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info, "[Play] Restored pre-play scene state.");
    }
    else
    {
        OutputDebugStringA("[Play] ERROR: Failed to restore editor scene state.\n");
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error, "[Play] Failed to restore pre-play scene state.");
    }

    m_playModeSceneSnapshot.clear();
    m_prePlaySelectionPath.clear();
    m_prePlayHadObjectSelection = false;
}

void EditorState::RefreshSelectionAfterReload(const ::Engine::Scene::Scene::ObjectPath& selectedPath)
{
    SelectObject(m_scene ? m_scene->FindObjectByPath(selectedPath) : nullptr);
}

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
        if (m_scene)
        {
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
            scenePanel->OnFocused = [this]()
            {
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
            gamePanel->OnFocused = [this]()
            {
                SetActiveSceneAssetDocument(nullptr);
                SetPrefabDocumentFocused(false);
            };
            m_panels.push_back(std::move(gamePanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Game panel is null\n");
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Hierarchy view\n");
        auto hierarchyPanel = m_viewFactory->Create("Hierarchy");
        if (hierarchyPanel)
        {
            hierarchyPanel->OnFocused = [this]() {};
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
            properties->OnFocused = [this]() {};
            m_panels.push_back(std::move(properties));
        }
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Assets view\n");
        auto assets = m_viewFactory->Create("Assets");
        if (assets)
        {
            m_primaryAssets = static_cast<AssetsExplorerView*>(assets.get());
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
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath(assetPath);
        MarkHistorySelectionChanged();
    };

    m_viewFactory->OnAssetRenamed = [this](const std::string& oldPath,
        const std::string& newPath) {
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath(newPath);

        const std::string oldNormalized = NormalizeAssetPath(oldPath);
        const std::string newNormalized = NormalizeAssetPath(newPath);

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
            const std::vector<std::string> oldCandidates = {
                oldNormalized,
                std::filesystem::path(oldNormalized).make_preferred().string()
            };
            const std::vector<std::string> newCandidates = {
                newNormalized,
                std::filesystem::path(newNormalized).make_preferred().string()
            };

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
                    lowerExtension != ".xml")
                    continue;

                std::ifstream input(entry.path(), std::ios::binary);
                if (!input)
                    continue;
                std::string contents((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
                input.close();

                bool changed = false;
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
    };

    m_viewFactory->OnAssetContentsChanged = [this](const std::string&) {
        auto refresh = [this](Engine::Scene::Scene* scene)
        {
            if (!scene) return;
            const std::string snapshot = scene->SaveToString();
            ::Engine::Scene::Scene::ObjectPath selectedPath;
            const bool hadSelection = scene->GetSelectedObject() &&
                scene->TryGetObjectPath(scene->GetSelectedObject(), selectedPath);
            if (!scene->LoadFromString(snapshot) && m_primaryConsole)
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
            if (document) refresh(document->scene.get());
    };

    // Wire up selection changed callback (for hierarchy -> properties)
    m_viewFactory->OnSelectionChanged = [this](Engine::Core::Object* obj) {
        OutputDebugStringA(("[EditorState] Selection changed to: " + (obj ? obj->name : "nullptr") + "\n").c_str());
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(obj);
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

    // Wire up focus (double-click) callback — frame the object in the scene camera
    m_viewFactory->OnFocusObject = [this](Engine::Core::Object* obj) {
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->FocusEditorCamera(obj);
    };
    m_viewFactory->OnHierarchyChanged = [this]() {
        if (m_activeSceneAssetDocument)
            m_renderer->MarkDirty();
        else if (m_prefabDocumentFocused)
            SetPrefabDirty(true);
        else
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
    };
    m_viewFactory->OnPropertiesChanged = [this]() {
        if (m_activeSceneAssetDocument)
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
    m_viewFactory->OnAssetPreviewRequested = [this](const std::string& path) {
        m_assetPreviewActive = true;
        Engine::Core::Object* preview = InstantiateAsset(path, false);
        m_assetPreviewActive = preview != nullptr;
        return preview;
    };
    m_viewFactory->OnAssetPreviewCancelled = [this](Engine::Core::Object* object) {
        if (m_scene && object)
            m_scene->RemoveObject(object);
        m_assetPreviewActive = false;
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
        m_window->OnFilesDropped = [this](const std::vector<std::string>& paths) {
            Engine::Core::Object* lastObject = nullptr;
            for (const std::string& path : paths)
            {
                const std::string importedPath = ImportAssetFile(path);
                if (Engine::Core::Object* object = importedPath.empty()
                    ? nullptr : InstantiateAsset(importedPath))
                    lastObject = object;
            }
            if (lastObject)
                SelectObject(lastObject);
        };
}

void EditorState::ImportAsset()
{
    wchar_t source[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_window ? m_window->GetHWND() : nullptr;
    dialog.lpstrFile = source;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter =
        L"Supported Assets (*.obj;*.gltf;*.glb;*.fbx;*.prefab;*.spriteanim;*.spritesheet;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2;*.wav;*.ogg;*.mp3;*.ttf;*.otf)\0"
        L"*.obj;*.gltf;*.glb;*.fbx;*.prefab;*.spriteanim;*.spritesheet;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2;*.wav;*.ogg;*.mp3;*.ttf;*.otf\0"
        L"3D Models (*.obj;*.gltf;*.glb;*.fbx)\0*.obj;*.gltf;*.glb;*.fbx\0"
        L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2)\0*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2\0"
        L"Audio (*.wav;*.ogg;*.mp3)\0*.wav;*.ogg;*.mp3\0"
        L"Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0"
        L"All Files (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog))
        ImportAssetFile(std::filesystem::path(source).string());
}

std::string EditorState::ImportAssetFile(const std::string& path)
{
    namespace fs = std::filesystem;
    const fs::path source(path);
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const fs::path assetsDirectory = m_projectSettings.assetsDirectory.empty()
        ? fs::path("Assets") : fs::path(m_projectSettings.assetsDirectory);

    try
    {
        fs::create_directories(assetsDirectory);
        if (ModelImporter::SupportsExtension(extension))
        {
            const Engine::Model::ModelImportResult imported =
                ModelImporter::Import(source.string(), assetsDirectory.string());
            if (!imported.success)
                throw std::runtime_error(imported.message);
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Info,
                    "Asset imported: " + imported.prefabPath);
            return imported.prefabPath;
        }

        std::error_code relativeError;
        const fs::path absoluteSource = fs::weakly_canonical(source, relativeError);
        const fs::path absoluteAssets = fs::weakly_canonical(assetsDirectory, relativeError);
        if (!relativeError)
        {
            const fs::path relative = absoluteSource.lexically_relative(absoluteAssets);
            if (!relative.empty() && *relative.begin() != "..")
            {
                Engine::Core::AssetRecord::Ensure(source, source,
                    { { "importer", std::string("native") } });
                return source.string();
            }
        }

        fs::path destination = assetsDirectory / source.filename();
        const std::string stem = destination.stem().string();
        const std::string suffix = destination.extension().string();
        for (unsigned index = 2; fs::exists(destination); ++index)
            destination = assetsDirectory /
                (stem + " " + std::to_string(index) + suffix);
        fs::copy_file(source, destination);
        Engine::Core::AssetRecord::Ensure(destination, source,
            { { "importer", std::string("copy") } });
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Asset imported: " + destination.string());
        return destination.string();
    }
    catch (const std::exception& error)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Asset import failed: " + std::string(error.what()));
        return {};
    }
}

Engine::Core::Object* EditorState::InstantiateAsset(const std::string& path, bool recordChange)
{
    Engine::Scene::Scene* targetScene = GetActiveDocumentScene();
    if (!targetScene)
        return nullptr;

    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    Engine::Core::Object* object = nullptr;
    try
    {
        if (ModelImporter::SupportsExtension(extension))
        {
            const std::string assetsDirectory = m_projectSettings.assetsDirectory.empty()
                ? std::string("Assets")
                : m_projectSettings.assetsDirectory;
            const Engine::Model::ModelImportResult imported = ModelImporter::Import(path, assetsDirectory);
            if (!imported.success)
                throw std::runtime_error(imported.message);
            object = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                *targetScene, imported.prefabPath, targetScene->GetGraphicsProvider());
        }
        else if (extension == ".prefab")
        {
            object = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                *targetScene, path, targetScene->GetGraphicsProvider());
        }
        else if (extension == ".obj")
        {
            object = targetScene->AddObject(std::filesystem::path(path).stem().string());
            Engine::Components::Mesh* mesh = object->AddComponent<Engine::Components::Mesh>();
            mesh->LoadFromFile(path);
            if (targetScene->GetGraphicsProvider())
                mesh->CreateBuffer(targetScene->GetGraphicsProvider()->GetBufferFactory());
            object->AddComponent<Engine::Components::Material>();
        }
        else if (extension == ".spriteanim")
        {
            object = targetScene->AddObject(std::filesystem::path(path).stem().string());
            Engine::Components::SpriteAnimationManager* manager = object->AddComponent<Engine::Components::SpriteAnimationManager>();
            Engine::Components::Sprite* sprite = object->AddComponent<Engine::Components::Sprite>();
            sprite->SetAnimationManager(manager);
            if (!manager->LoadFromFile(path) ||
                !sprite->Prepare(targetScene->GetGraphicsProvider()))
                throw std::runtime_error("Could not load sprite animation");
        }
        else if (extension == ".scene" || extension == ".xml")
        {
            QueueSceneAssetDocumentOpen(path);
            return nullptr;
        }
        else
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                    "Unsupported scene drop: " + path);
            return nullptr;
        }
    }
    catch (const std::exception& error)
    {
        if (object)
            targetScene->RemoveObject(object);
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not instantiate asset '" + path + "': " + error.what());
        return nullptr;
    }

    if (object && recordChange)
    {
        if (m_activeSceneAssetDocument)
        {
            if (m_renderer) m_renderer->MarkDirty();
        }
        else if (m_prefabDocumentFocused)
            SetPrefabDirty(true);
        else
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Added to scene: " + path);
    }
    return object;
}

Engine::Scene::Scene* EditorState::GetActiveDocumentScene() const
{
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->scene)
        return m_activeSceneAssetDocument->scene.get();
    if (m_prefabDocumentFocused && m_prefabScene)
        return m_prefabScene.get();
    return m_scene.get();
}

void EditorState::SetPrefabDocumentFocused(bool focused)
{
    m_activeAssetDocument = nullptr;
    const bool nextFocused = focused && m_prefabScene && m_prefabSceneView;
    if (nextFocused)
        m_activeSceneAssetDocument = nullptr;
    m_prefabDocumentFocused = nextFocused;
    Engine::Scene::Scene* activeScene = GetActiveDocumentScene();

    if (m_primaryHierarchy)
    {
        m_primaryHierarchy->Init(activeScene);
        m_primaryHierarchy->SetSelectedObject(nullptr);
    }
    if (m_primaryProperties)
    {
        m_primaryProperties->Init(activeScene);
        m_primaryProperties->SetSelectedObject(nullptr);
        m_primaryProperties->SetSelectedAsset("");
    }
    if (activeScene)
    {
        Engine::Core::Object* selected = activeScene->GetSelectedObject();
        if (m_primaryHierarchy)
            m_primaryHierarchy->SetSelectedObject(selected);
        if (m_primaryProperties)
            m_primaryProperties->SetSelectedObject(selected);
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

EditorState::HistoryEntry EditorState::CaptureHistoryEntry() const
{
    HistoryEntry entry;
    if (!m_scene)
        return entry;
    entry.scene = m_scene->SaveToString();
    CaptureHistorySelection(entry);
    return entry;
}

void EditorState::CaptureHistorySelection(HistoryEntry& entry) const
{
    entry.hasSelection = false;
    entry.selectionPath.clear();
    if (!m_scene || m_prefabDocumentFocused || !m_primaryHierarchy)
        return;
    Engine::Core::Object* selected = m_primaryHierarchy->GetSelectedObject();
    entry.hasSelection = selected &&
        m_scene->TryGetObjectPath(selected, entry.selectionPath);
}

void EditorState::MarkSceneEdited()
{
    ++m_sceneEditRevision;
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::TrackSceneChanges(bool allowHistory, bool editInProgress)
{
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->scene)
    {
        if (!allowHistory || editInProgress)
            return;
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        std::string current = CaptureSceneAssetDocumentSnapshot(document);
        if (current != document.baseline)
        {
            if (m_historyLimit > 0 && document.undo.size() >= m_historyLimit)
                document.undo.pop_front();
            if (m_historyLimit > 0)
                document.undo.push_back(document.baseline);
            document.redo.clear();
            document.baseline = std::move(current);
            document.dirty = document.baseline != document.savedSnapshot;
            RefreshSceneAssetDocumentTitle(document);
            if (m_renderer) m_renderer->MarkDirty();
        }
        return;
    }
    if (!m_scene || !allowHistory || m_assetPreviewActive)
        return;

    if (m_historySelectionDirty && !m_historyBaseline.scene.empty())
    {
        CaptureHistorySelection(m_historyBaseline);
        m_historySelectionDirty = false;
    }

    // Property drags and gizmos can update every rendered frame. Capturing the
    // complete scene here made large linked prefabs serialize and diff for
    // every intermediate mouse position. The existing baseline is already the
    // correct undo "before" state, so wait until the interaction ends and
    // capture its final value once.
    if (editInProgress)
        return;

    // Editor mutation callbacks advance the revision. If it has not changed,
    // the serialized scene must still match the existing history baseline, so
    // avoid rebuilding and comparing the complete scene on this idle frame.
    if (!m_historyBaseline.scene.empty() &&
        m_historyCapturedRevision == m_sceneEditRevision)
        return;

    HistoryEntry current = CaptureHistoryEntry();
    m_historyCapturedRevision = m_sceneEditRevision;
    m_historySelectionDirty = false;
    if (m_historyBaseline.scene.empty())
    {
        m_historyBaseline = std::move(current);
        if (m_savedSceneSnapshot.empty())
            m_savedSceneSnapshot = m_historyBaseline.scene;
        return;
    }

    if (current.scene != m_historyBaseline.scene)
    {
        if (m_historyLimit > 0 && !m_hasPendingHistoryEdit)
        {
            m_pendingHistoryBefore = m_historyBaseline;
            m_hasPendingHistoryEdit = true;
            m_redoHistory.clear();
        }
        m_historyBaseline = std::move(current);
        m_hasUnsavedChanges = m_historyBaseline.scene != m_savedSceneSnapshot;
        RefreshSceneDocumentTitle();
        if (m_hasPendingHistoryEdit && !editInProgress)
            CommitPendingHistoryEdit();
    }
    else
    {
        // Selection is editor state too, but changing it alone is not an undo action.
        m_historyBaseline.hasSelection = current.hasSelection;
        m_historyBaseline.selectionPath = std::move(current.selectionPath);
        if (m_hasPendingHistoryEdit && !editInProgress)
            CommitPendingHistoryEdit();
    }
}

void EditorState::CommitPendingHistoryEdit()
{
    if (!m_hasPendingHistoryEdit)
        return;
    if (m_historyLimit > 0 &&
        m_pendingHistoryBefore.scene != m_historyBaseline.scene)
        m_undoHistory.push_back(std::move(m_pendingHistoryBefore));
    m_pendingHistoryBefore = {};
    m_hasPendingHistoryEdit = false;
    TrimHistory();
}

void EditorState::Undo()
{
    if (m_activeSceneAssetDocument)
    {
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        if (document.undo.empty()) return;
        document.redo.push_back(document.baseline);
        document.baseline = std::move(document.undo.back());
        document.undo.pop_back();
        RestoreSceneAssetDocumentSnapshot(document, document.baseline);
        document.dirty = document.baseline != document.savedSnapshot;
        RefreshSceneAssetDocumentTitle(document);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(nullptr);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(nullptr);
        if (m_renderer) m_renderer->MarkDirty();
        return;
    }
    if (m_activeAssetDocument)
    {
        if (!m_activeAssetDocument->Undo() && m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "No asset edit to undo.");
        return;
    }
    TrackSceneChanges(true, false);
    CommitPendingHistoryEdit();
    if (!m_scene || m_undoHistory.empty())
        return;
    HistoryEntry target = std::move(m_undoHistory.back());
    m_undoHistory.pop_back();
    // TrackSceneChanges has already made the baseline an exact snapshot of
    // the current scene. Re-serializing it here made every undo pay for a
    // second complete walk of large prefab/model hierarchies.
    m_redoHistory.push_back(std::move(m_historyBaseline));
    TrimHistory();
    ApplyHistoryEntry(std::move(target), "Undo");
}

void EditorState::Redo()
{
    if (m_activeSceneAssetDocument)
    {
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        if (document.redo.empty()) return;
        if (m_historyLimit > 0)
            document.undo.push_back(document.baseline);
        document.baseline = std::move(document.redo.back());
        document.redo.pop_back();
        RestoreSceneAssetDocumentSnapshot(document, document.baseline);
        document.dirty = document.baseline != document.savedSnapshot;
        RefreshSceneAssetDocumentTitle(document);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(nullptr);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(nullptr);
        if (m_renderer) m_renderer->MarkDirty();
        return;
    }
    if (m_activeAssetDocument)
    {
        if (!m_activeAssetDocument->Redo() && m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "No asset edit to redo.");
        return;
    }
    TrackSceneChanges(true, false);
    CommitPendingHistoryEdit();
    if (!m_scene || m_redoHistory.empty())
        return;
    HistoryEntry target = std::move(m_redoHistory.back());
    m_redoHistory.pop_back();
    m_undoHistory.push_back(std::move(m_historyBaseline));
    TrimHistory();
    ApplyHistoryEntry(std::move(target), "Redo");
}

bool EditorState::CanUndo() const
{
    return m_activeSceneAssetDocument ? !m_activeSceneAssetDocument->undo.empty()
        : m_activeAssetDocument ? m_activeAssetDocument->CanUndo()
        : (m_hasPendingHistoryEdit || !m_undoHistory.empty());
}

bool EditorState::CanRedo() const
{
    return m_activeSceneAssetDocument ? !m_activeSceneAssetDocument->redo.empty()
        : m_activeAssetDocument ? m_activeAssetDocument->CanRedo()
        : !m_redoHistory.empty();
}

void EditorState::ApplyHistoryEntry(
    HistoryEntry entry, const char* operation)
{
    SelectObject(nullptr);
    if (!m_scene->LoadFromString(entry.scene))
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                std::string(operation) + " failed to restore the scene.");
        return;
    }

    SelectObject(entry.hasSelection
        ? m_scene->FindObjectByPath(entry.selectionPath) : nullptr);
    // The restored source is itself the canonical history snapshot. Keeping
    // it avoids serializing the newly rebuilt scene for a second time.
    m_historyBaseline = std::move(entry);
    m_historyCapturedRevision = m_sceneEditRevision;
    m_historySelectionDirty = false;
    m_hasUnsavedChanges = m_historyBaseline.scene != m_savedSceneSnapshot;
    RefreshSceneDocumentTitle();
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            std::string(operation) + " completed.");
}

void EditorState::ResetHistory(bool sceneIsSaved)
{
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_pendingHistoryBefore = {};
    m_hasPendingHistoryEdit = false;
    m_historyBaseline = CaptureHistoryEntry();
    m_historyCapturedRevision = m_sceneEditRevision;
    m_historySelectionDirty = false;
    if (sceneIsSaved)
        m_savedSceneSnapshot = m_historyBaseline.scene;
}

void EditorState::SetHistoryLimit(uint32_t limit)
{
    m_historyLimit = std::min(limit, 1000u);
    for (auto& document : m_sceneAssetDocuments)
        if (document)
        {
            while (document->undo.size() > m_historyLimit)
                document->undo.pop_front();
            while (document->redo.size() > m_historyLimit)
                document->redo.pop_front();
        }
    if (m_historyLimit == 0)
    {
        m_undoHistory.clear();
        m_redoHistory.clear();
        m_pendingHistoryBefore = {};
        m_hasPendingHistoryEdit = false;
    }
    else
        TrimHistory();
}

void EditorState::TrimHistory()
{
    while (m_undoHistory.size() > m_historyLimit)
        m_undoHistory.pop_front();
    while (m_redoHistory.size() > m_historyLimit)
        m_redoHistory.pop_front();
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
