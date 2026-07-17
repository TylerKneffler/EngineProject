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
#include <chrono>
#include <filesystem>
#include <fstream>

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
    m_editorLayout.Initialize(m_projectFilePath);
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
    m_editorLayout.Flush();
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
    if (m_scene)
    {
        // TODO: Implement scene serialization
        m_hasUnsavedChanges = false;
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
    for (auto& panel : m_panels)
        if (panel)
            panel->SetOpen(m_editorLayout.IsPanelOpen(panel->GetTitle(), true));
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

    // Wire up focus (double-click) callback — frame the object in the scene camera
    m_viewFactory->OnFocusObject = [this](Object* obj) {
        if (m_scene)
            m_scene->FocusEditorCamera(obj);
    };
}

// ---------------------------------------------------------------------------
// EditorState::GetDeltaTime
// ---------------------------------------------------------------------------
float EditorState::GetDeltaTime() const
{
    // TODO: Implement delta time calculation
    return 0.0f;
}

// ---------------------------------------------------------------------------
// EditorState::UpdateDeltaTime
// ---------------------------------------------------------------------------
void EditorState::UpdateDeltaTime()
{
    // TODO: Implement delta time update
}
