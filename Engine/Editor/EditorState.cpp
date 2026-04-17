#include "pch.h"
#include "EditorState.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/Window.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include <chrono>

// ---------------------------------------------------------------------------
// EditorState::EditorState
// ---------------------------------------------------------------------------
EditorState::EditorState(HINSTANCE hInstance, const ProjectSettings& projectSettings)
    : m_projectSettings(projectSettings)
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
        return false;

    HWND hwnd = m_window->GetHWND();
    if (!hwnd)
        return false;

    // Initialize renderer
    m_renderer = RendererFactory::CreateEditorRenderer(m_projectSettings);
    if (!m_renderer)
        return false;
    
    if (!m_renderer->Init(hwnd, 1280, 720))
        return false;

    // Initialize scene
    m_scene = std::make_unique<Scene>();
    if (!m_scene)
        return false;

    // Initialize view factory
    m_viewFactory = std::make_unique<ViewFactory>(m_renderer.get(), m_scene.get(), m_projectSettings);
    if (!m_viewFactory)
        return false;

    // Initialize panels
    InitializePanels();

    // Wire up callbacks
    WireupCallbacks();

    return true;
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
    if (m_scene)
    {
        // TODO: Implement scene deserialization
    }
}

// ---------------------------------------------------------------------------
// EditorState::InitializePanels
// ---------------------------------------------------------------------------
void EditorState::InitializePanels()
{
    // TODO: Initialize editor panels
}

// ---------------------------------------------------------------------------
// EditorState::WireupCallbacks
// ---------------------------------------------------------------------------
void EditorState::WireupCallbacks()
{
    // TODO: Wire up callbacks
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
