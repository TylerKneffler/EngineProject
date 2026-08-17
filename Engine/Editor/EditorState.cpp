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
#include "Core/Window.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/AssetRecord.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Importers/ModelImporter.h"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
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
    m_scene->SetEditorMode2D(
        m_projectSettings.editorMode == Engine::Model::ProjectSettings::EditorMode::TwoD);
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
        m_prefabHasUnsavedChanges = false;
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

void EditorState::SaveMainScene()
{
    if (!m_scene)
        return;

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
            return;
        destination = std::filesystem::path(filePath).string();
    }

    if (std::filesystem::path(destination).extension().empty())
        destination += ".scene";

    if (m_scene->Save(destination))
    {
        m_currentScenePath = std::filesystem::path(destination).lexically_normal().string();
        m_hasUnsavedChanges = false;
        m_savedSceneSnapshot = m_scene->SaveToString();
        if (m_primaryConsole)
            m_primaryConsole->AddLog(
                ConsoleView::Level::Info, "Scene saved: " + m_currentScenePath);
    }
    else if (m_primaryConsole)
    {
        m_primaryConsole->AddLog(
            ConsoleView::Level::Error, "Failed to save scene: " + destination);
    }
}

void EditorState::SaveAll()
{
    if (!m_activePrefabPath.empty())
    {
        const bool previousFocus = m_prefabDocumentFocused;
        m_prefabDocumentFocused = true;
        SaveScene();
        m_prefabDocumentFocused = previousFocus;
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
    const std::string sceneName = m_currentScenePath.empty()
        ? "Untitled" : std::filesystem::path(m_currentScenePath).stem().string();
    if (m_activePrefabPath.empty())
        return sceneName;
    return sceneName + " | " +
        std::filesystem::path(m_activePrefabPath).stem().string() + " (Prefab)";
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
    if (!m_scene)
    {
        OutputDebugStringA("[EditorState::LoadScene] ERROR: Scene is null\n");
        return;
    }

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
    
    // Load the scene
    try
    {
        if (m_scene->Load(resolvedPath))
        {
            OutputDebugStringA("[EditorState::LoadScene] Scene loaded successfully\n");
            LogStartupFailure("Scene loaded successfully: " + resolvedPath);
            m_hasUnsavedChanges = false;
            m_currentScenePath =
                std::filesystem::path(resolvedPath).lexically_normal().string();
            ResetHistory(true);
            
            if (m_primaryConsole)
            {
                m_primaryConsole->AddLog(ConsoleView::Level::Info, "Scene loaded: " + resolvedPath);
            }
        }
        else
        {
            OutputDebugStringA("[EditorState::LoadScene] Failed to load scene\n");
            LogStartupFailure("Failed to load scene: " + resolvedPath);
            if (m_primaryConsole)
            {
                m_primaryConsole->AddLog(ConsoleView::Level::Error, "Failed to load scene: " + resolvedPath);
            }
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
    const std::string normalized =
        std::filesystem::path(path).lexically_normal().string();
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
        prefabScene->SetEditorMode2D(
            m_projectSettings.editorMode == Engine::Model::ProjectSettings::EditorMode::TwoD);
        prefabScene->Init(m_renderer->GetGraphicsProvider());
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
        std::filesystem::path(normalized).stem().string();
    auto sceneView = m_viewFactory->CreateSceneView(
        prefabScene.get(), prefabName + " (Prefab)");
    if (!sceneView)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not create prefab editor viewport: " + normalized);
        return;
    }
    auto hierarchy = std::make_unique<HierarchyView>();
    hierarchy->SetTitle(prefabName + " Hierarchy");
    hierarchy->Init(prefabScene.get());
    auto properties = std::make_unique<PropertiesView>();
    properties->SetTitle(prefabName + " Properties");
    properties->Init(prefabScene.get());
    properties->SetShowChildHierarchy(false);
    properties->OnPrefabRequested = [this](const std::string& prefabPath)
    {
        m_pendingPrefabPath = prefabPath;
    };

    m_prefabScene = std::move(prefabScene);
    m_prefabSceneView = sceneView.get();
    m_prefabHierarchy = hierarchy.get();
    m_prefabProperties = properties.get();
    m_prefabHierarchy->OnSelectionChanged = [this](Engine::Core::Object* object)
    {
        if (m_prefabScene) m_prefabScene->SetSelectedObject(object);
        if (m_prefabProperties) m_prefabProperties->SetSelectedObject(object);
    };
    m_prefabHierarchy->OnFocusObject = [this](Engine::Core::Object* object)
    {
        if (m_prefabScene) m_prefabScene->FocusEditorCamera(object);
    };
    m_prefabHierarchy->OnHierarchyChanged = [this]()
    {
        m_prefabHasUnsavedChanges = true;
    };
    m_prefabProperties->OnComponentsChanged = [this]()
    {
        m_prefabHasUnsavedChanges = true;
    };
    m_prefabSceneView->OnObjectSelected = [this](Engine::Core::Object* object)
    {
        if (m_prefabScene) m_prefabScene->SetSelectedObject(object);
        if (m_prefabHierarchy) m_prefabHierarchy->SetSelectedObject(object);
        if (m_prefabProperties) m_prefabProperties->SetSelectedObject(object);
    };
    m_prefabSceneView->OnObjectCreated = [this](Engine::Core::Object* object)
    {
        m_prefabHasUnsavedChanges = true;
        if (m_prefabScene) m_prefabScene->SetSelectedObject(object);
        if (m_prefabHierarchy) m_prefabHierarchy->SetSelectedObject(object);
        if (m_prefabProperties) m_prefabProperties->SetSelectedObject(object);
    };
    m_prefabSceneView->OnGizmoInteraction = [this](bool active)
    {
        if (active) m_prefabHasUnsavedChanges = true;
    };
    sceneView->OnFocused = [this]() { m_prefabDocumentFocused = true; };
    hierarchy->OnFocused = [this]() { m_prefabDocumentFocused = true; };
    properties->OnFocused = [this]() { m_prefabDocumentFocused = true; };
    m_panels.push_back(std::move(sceneView));
    m_panels.push_back(std::move(hierarchy));
    m_panels.push_back(std::move(properties));
    m_activePrefabPath = normalized;
    m_prefabHasUnsavedChanges = false;
    m_prefabDocumentFocused = true;
    m_prefabHierarchy->SetSelectedObject(root);
    m_prefabProperties->SetSelectedObject(root);
    m_prefabScene->SetSelectedObject(root);
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            "Opened prefab stage: " + normalized);
}

void EditorState::ProcessPendingPrefabStageOpen()
{
    if (m_pendingPrefabPath.empty())
        return;

    // Opening a prefab creates three panels. Do it after the panel draw loop so
    // growing m_panels cannot invalidate the iterator currently drawing Assets.
    std::string path = std::move(m_pendingPrefabPath);
    m_pendingPrefabPath.clear();
    OpenPrefabStage(path);
}

void EditorState::ClosePrefabStage()
{
    if (m_activePrefabPath.empty()) return;
    if (m_prefabHasUnsavedChanges)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Save the prefab before closing its stage.");
        return;
    }

    RemovePrefabPanels();
    m_prefabScene.reset();
    m_activePrefabPath.clear();
    m_prefabHasUnsavedChanges = false;
    m_prefabDocumentFocused = false;
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            "Closed prefab stage.");
}

void EditorState::HandlePrefabPanelClosures()
{
    if (m_activePrefabPath.empty())
        return;
    const bool panelClosed =
        (m_prefabSceneView && !m_prefabSceneView->IsOpen()) ||
        (m_prefabHierarchy && !m_prefabHierarchy->IsOpen()) ||
        (m_prefabProperties && !m_prefabProperties->IsOpen());
    if (!panelClosed)
        return;

    if (m_prefabHasUnsavedChanges)
    {
        if (m_prefabSceneView) m_prefabSceneView->SetOpen(true);
        if (m_prefabHierarchy) m_prefabHierarchy->SetOpen(true);
        if (m_prefabProperties) m_prefabProperties->SetOpen(true);
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
        return panel.get() == m_prefabSceneView ||
            panel.get() == m_prefabHierarchy ||
            panel.get() == m_prefabProperties;
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
    m_prefabHierarchy = nullptr;
    m_prefabProperties = nullptr;
}

void EditorState::CapturePlayModeScene()
{
    if (!m_scene)
        return;

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
        SelectObject(m_prePlayHadObjectSelection
            ? m_scene->FindObjectByPath(m_prePlaySelectionPath)
            : nullptr);
        m_historyBaseline = CaptureHistoryEntry();
        m_historyCapturedRevision = m_sceneEditRevision;
        m_historySelectionDirty = false;
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
    m_preferences->OnSettingsChanged = [this]() {
        m_projectSettings = m_preferences->GetSettings();
        if (m_scene)
            m_scene->SetEditorMode2D(
                m_projectSettings.editorMode == Engine::Model::ProjectSettings::EditorMode::TwoD);
        SetHistoryLimit(m_projectSettings.editorHistoryLimit);
        for (auto& panel : m_panels)
            if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
                hierarchy->SetDebugInteractionLogging(
                    m_projectSettings.debugHierarchyInteractions);
    };
    
    OutputDebugStringA("[EditorState::InitializePanels] Checking view factory\n");
    if (m_viewFactory)
    {
        OutputDebugStringA("[EditorState::InitializePanels] Creating Scene view\n");
        auto scenePanel = m_viewFactory->Create("Scene");
        if (scenePanel)
        {
            scenePanel->OnFocused = [this]() { m_prefabDocumentFocused = false; };
            m_panels.push_back(std::move(scenePanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Scene panel is null\n");
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Game view\n");
        auto gamePanel = m_viewFactory->Create("Game");
        if (gamePanel)
        {
            gamePanel->OnFocused = [this]() { m_prefabDocumentFocused = false; };
            m_panels.push_back(std::move(gamePanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Game panel is null\n");
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Hierarchy view\n");
        auto hierarchyPanel = m_viewFactory->Create("Hierarchy");
        if (hierarchyPanel)
        {
            hierarchyPanel->OnFocused = [this]() { m_prefabDocumentFocused = false; };
            m_panels.push_back(std::move(hierarchyPanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Hierarchy panel is null\n");
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Properties view\n");
        auto properties = m_viewFactory->Create("Properties");
        if (properties)
        {
            OutputDebugStringA("[EditorState::InitializePanels] Storing properties pointer\n");
            m_primaryProperties = static_cast<PropertiesView*>(properties.get());
            properties->OnFocused = [this]() { m_prefabDocumentFocused = false; };
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
        m_prefabDocumentFocused = false;
    };

    // Wire up scene loading callback
    m_viewFactory->OnSceneRequested = [this](const std::string& scenePath) {
        OutputDebugStringA(("[EditorState] Scene requested: " + scenePath + "\n").c_str());
        LoadScene(scenePath);
    };
    m_viewFactory->OnPrefabRequested = [this](const std::string& prefabPath) {
        m_pendingPrefabPath = prefabPath;
    };

    m_viewFactory->OnAssetSelected = [this](const std::string& assetPath) {
        for (auto& panel : m_panels)
            if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
                hierarchy->SetSelectedObject(nullptr);
        if (m_scene)
            m_scene->SetSelectedObject(nullptr);
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
        if (!m_scene)
            return;
        std::function<void(Engine::Core::Object*)> updatePrefab = [&](Engine::Core::Object* object) {
            if (!object)
                return;
            if (object->Prefab &&
                std::filesystem::path(object->Prefab->GetPath()).lexically_normal() ==
                std::filesystem::path(oldPath).lexically_normal())
                object->SetPrefab(newPath);
            for (Engine::Core::Object* child : object->Children)
                updatePrefab(child);
        };
        for (const auto& object : m_scene->GetObjects())
            if (object && !object->Parent)
                updatePrefab(object.get());
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
    };

    m_viewFactory->OnAssetContentsChanged = [this](const std::string&) {
        if (!m_scene)
            return;
        const std::string sceneSnapshot = m_scene->SaveToString();
        m_scene->SetSelectedObject(nullptr);
        if (!m_scene->LoadFromString(sceneSnapshot) && m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not refresh scene after asset properties changed.");
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
    };

    // Wire up selection changed callback (for hierarchy -> properties)
    m_viewFactory->OnSelectionChanged = [this](Engine::Core::Object* obj) {
        OutputDebugStringA(("[EditorState] Selection changed to: " + (obj ? obj->name : "nullptr") + "\n").c_str());
        if (m_scene)
            m_scene->SetSelectedObject(obj);
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

    // Scene viewport click-selection -> hierarchy/properties + render selection state
    m_viewFactory->OnObjectSelected = [this](Engine::Core::Object* obj) {
        for (auto& panel : m_panels)
            if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
            {
                hierarchy->SetSelectedObject(obj);
                break;
            }
        if (m_primaryProperties)
            m_primaryProperties->SetSelectedObject(obj);
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath({});
        if (m_scene)
            m_scene->SetSelectedObject(obj);
        MarkHistorySelectionChanged();
    };
    m_viewFactory->OnObjectCreated = [this](Engine::Core::Object* obj) {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
        SelectObject(obj);
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
        if (m_scene)
            m_scene->FocusEditorCamera(obj);
    };
    m_viewFactory->OnHierarchyChanged = [this]() {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
    };
    m_viewFactory->OnPropertiesChanged = [this]() {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
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
        L"Supported Assets (*.obj;*.gltf;*.glb;*.fbx;*.prefab;*.spriteanim;*.spritesheet;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2;*.wav;*.ogg;*.mp3)\0"
        L"*.obj;*.gltf;*.glb;*.fbx;*.prefab;*.spriteanim;*.spritesheet;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2;*.wav;*.ogg;*.mp3\0"
        L"3D Models (*.obj;*.gltf;*.glb;*.fbx)\0*.obj;*.gltf;*.glb;*.fbx\0"
        L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2)\0*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2\0"
        L"Audio (*.wav;*.ogg;*.mp3)\0*.wav;*.ogg;*.mp3\0"
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
    if (!m_scene)
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
                *m_scene, imported.prefabPath, m_scene->GetGraphicsProvider());
        }
        else if (extension == ".prefab")
        {
            object = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                *m_scene, path, m_scene->GetGraphicsProvider());
        }
        else if (extension == ".obj")
        {
            object = m_scene->AddObject(std::filesystem::path(path).stem().string());
            Engine::Components::Mesh* mesh = object->AddComponent<Engine::Components::Mesh>();
            mesh->LoadFromFile(path);
            if (m_scene->GetGraphicsProvider())
                mesh->CreateBuffer(m_scene->GetGraphicsProvider()->GetBufferFactory());
            object->AddComponent<Engine::Components::Material>();
        }
        else if (extension == ".spriteanim")
        {
            object = m_scene->AddObject(std::filesystem::path(path).stem().string());
            Engine::Components::SpriteAnimationManager* manager = object->AddComponent<Engine::Components::SpriteAnimationManager>();
            Engine::Components::Sprite* sprite = object->AddComponent<Engine::Components::Sprite>();
            sprite->SetAnimationManager(manager);
            if (!manager->LoadFromFile(path) ||
                !sprite->Prepare(m_scene->GetGraphicsProvider()))
                throw std::runtime_error("Could not load sprite animation");
        }
        else if (extension == ".scene" || extension == ".xml")
        {
            LoadScene(path);
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
            m_scene->RemoveObject(object);
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not instantiate asset '" + path + "': " + error.what());
        return nullptr;
    }

    if (object && recordChange)
    {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Added to scene: " + path);
    }
    return object;
}

void EditorState::SelectObject(Engine::Core::Object* object)
{
    if (m_primaryAssets)
        m_primaryAssets->SetSelectedPath({});
    for (auto& panel : m_panels)
        if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
        {
            hierarchy->SetSelectedObject(object);
            break;
        }
    if (m_primaryProperties)
        m_primaryProperties->SetSelectedObject(object);
    if (m_scene)
        m_scene->SetSelectedObject(object);
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
    if (!m_scene)
        return;
    for (const auto& panel : m_panels)
    {
        const auto* hierarchy = dynamic_cast<const HierarchyView*>(panel.get());
        if (!hierarchy)
            continue;
        Engine::Core::Object* selected = hierarchy->GetSelectedObject();
        entry.hasSelection = selected &&
            m_scene->TryGetObjectPath(selected, entry.selectionPath);
        break;
    }
}

void EditorState::MarkSceneEdited()
{
    ++m_sceneEditRevision;
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::TrackSceneChanges(bool allowHistory, bool editInProgress)
{
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
