#include "pch.h"
#include "EditorState.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/Window.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Assets/GltfImporter.h"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <commdlg.h>

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
EditorState::EditorState(HINSTANCE hInstance, const ProjectSettings& projectSettings,
    std::string projectFilePath)
    : m_projectSettings(projectSettings)
    , m_projectFilePath(std::move(projectFilePath))
{
    try
    {
        m_window = std::make_unique<Window>(hInstance, L"Engine Editor", 1280, 720);
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
        m_renderer = RendererFactory::CreateEditorRenderer(m_projectSettings);
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
    m_scene = std::make_unique<Scene>();
    if (!m_scene)
        return false;
    OutputDebugStringA("[EditorState] Scene created\n");
    
    OutputDebugStringA("[EditorState] Initializing scene...\n");
    IGraphicsProvider* graphicsProvider = m_renderer->GetGraphicsProvider();
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
        Object* selected = hierarchy->GetSelectedObject();
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

// ---------------------------------------------------------------------------
// EditorState::InitializePanels
// ---------------------------------------------------------------------------
void EditorState::InitializePanels()
{
    OutputDebugStringA("[EditorState::InitializePanels] Creating preferences view\n");
    m_preferences = std::make_unique<PreferencesView>();
    
    OutputDebugStringA("[EditorState::InitializePanels] Calling preferences->Init\n");
    m_preferences->Init(m_projectSettings, m_projectFilePath);
    
    OutputDebugStringA("[EditorState::InitializePanels] Checking view factory\n");
    if (m_viewFactory)
    {
        OutputDebugStringA("[EditorState::InitializePanels] Creating Scene view\n");
        auto scenePanel = m_viewFactory->Create("Scene");
        if (scenePanel) m_panels.push_back(std::move(scenePanel));
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Scene panel is null\n");
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Game view\n");
        auto gamePanel = m_viewFactory->Create("Game");
        if (gamePanel) m_panels.push_back(std::move(gamePanel));
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Game panel is null\n");
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Hierarchy view\n");
        auto hierarchyPanel = m_viewFactory->Create("Hierarchy");
        if (hierarchyPanel) m_panels.push_back(std::move(hierarchyPanel));
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
        m_panels.push_back(m_viewFactory->Create("Assets"));
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Console view\n");
        auto console = m_viewFactory->Create("Console");
        if (console)
        {
            OutputDebugStringA("[EditorState::InitializePanels] Storing console pointer\n");
            m_primaryConsole = static_cast<ConsoleView*>(console.get());
            m_panels.push_back(std::move(console));
        }
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

    // Wire up scene loading callback
    m_viewFactory->OnSceneRequested = [this](const std::string& scenePath) {
        OutputDebugStringA(("[EditorState] Scene requested: " + scenePath + "\n").c_str());
        LoadScene(scenePath);
    };

    // Wire up selection changed callback (for hierarchy -> properties)
    m_viewFactory->OnSelectionChanged = [this](Object* obj) {
        OutputDebugStringA(("[EditorState] Selection changed to: " + (obj ? obj->name : "nullptr") + "\n").c_str());
        if (m_scene)
            m_scene->SetSelectedObject(obj);
        if (m_primaryProperties)
        {
            m_primaryProperties->SetSelectedObject(obj);
            OutputDebugStringA("[EditorState] Updated properties view\n");
        }
        else
        {
            OutputDebugStringA("[EditorState] WARNING: Properties view is null\n");
        }
    };

    // Scene viewport click-selection -> hierarchy/properties + render selection state
    m_viewFactory->OnObjectSelected = [this](Object* obj) {
        for (auto& panel : m_panels)
            if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
            {
                hierarchy->SetSelectedObject(obj);
                break;
            }
        if (m_primaryProperties)
            m_primaryProperties->SetSelectedObject(obj);
        if (m_scene)
            m_scene->SetSelectedObject(obj);
    };

    // Wire up focus (double-click) callback — frame the object in the scene camera
    m_viewFactory->OnFocusObject = [this](Object* obj) {
        if (m_scene)
            m_scene->FocusEditorCamera(obj);
    };

    m_viewFactory->OnAssetDropped = [this](const std::string& path) {
        SelectObject(InstantiateAsset(path));
    };

    m_viewFactory->OnPrefabCreated = [this](Object*, const std::string& path) {
        m_hasUnsavedChanges = true;
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info, "Prefab created: " + path);
    };

    m_viewFactory->OnAssetImported = [this](const std::string& path) {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info, "Asset imported: " + path);
    };

    m_viewFactory->OnGltfImportRequested = [this](const std::string& path) {
        const std::string assetsDirectory = m_projectSettings.assetsDirectory.empty()
            ? std::string("Assets")
            : m_projectSettings.assetsDirectory;
        const GltfImportResult imported = GltfImporter::Import(path, assetsDirectory);
        if (m_primaryConsole)
            m_primaryConsole->AddLog(
                imported.success ? ConsoleView::Level::Info : ConsoleView::Level::Error,
                imported.success
                    ? "glTF imported: " + imported.prefabPath
                    : "glTF import failed: " + imported.message);
    };

    if (m_window)
        m_window->OnFilesDropped = [this](const std::vector<std::string>& paths) {
            Object* lastObject = nullptr;
            for (const std::string& path : paths)
                if (Object* object = InstantiateAsset(path))
                    lastObject = object;
            SelectObject(lastObject);
        };
}

Object* EditorState::InstantiateAsset(const std::string& path)
{
    if (!m_scene)
        return nullptr;

    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    Object* object = nullptr;
    try
    {
        if (extension == ".gltf" || extension == ".glb")
        {
            const std::string assetsDirectory = m_projectSettings.assetsDirectory.empty()
                ? std::string("Assets")
                : m_projectSettings.assetsDirectory;
            const GltfImportResult imported = GltfImporter::Import(path, assetsDirectory);
            if (!imported.success)
                throw std::runtime_error(imported.message);
            object = SceneSerializer::InstantiatePrefab(
                *m_scene, imported.prefabPath, m_scene->GetGraphicsProvider());
        }
        else if (extension == ".prefab")
        {
            object = SceneSerializer::InstantiatePrefab(
                *m_scene, path, m_scene->GetGraphicsProvider());
        }
        else if (extension == ".obj")
        {
            object = m_scene->AddObject(std::filesystem::path(path).stem().string());
            Mesh* mesh = object->AddComponent<Mesh>();
            mesh->LoadFromFile(path);
            if (m_scene->GetGraphicsProvider())
                mesh->CreateBuffer(m_scene->GetGraphicsProvider()->GetBufferFactory());
            object->AddComponent<Material>();
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

    if (object)
    {
        m_hasUnsavedChanges = true;
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Added to scene: " + path);
    }
    return object;
}

void EditorState::SelectObject(Object* object)
{
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
