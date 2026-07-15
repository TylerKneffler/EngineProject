#pragma once
#include "pch.h"
#include "Core/ProjectLoader.h"
#include "Core/Scene/Scene.h"
#include "Core/Renderers/IEditorRenderer.h"
#include <memory>
#include <vector>
#include <functional>

class Window;
class ViewFactory;
class IEditorPanel;
class PreferencesView;
class ConsoleView;
class PropertiesView;
class HierarchyView;
class AssetsExplorerView;
struct ProjectSettings;

// ---------------------------------------------------------------------------
// EditorState — Encapsulates all editor application state
// ---------------------------------------------------------------------------
class EditorState
{
public:
    EditorState(HINSTANCE hInstance, const ProjectSettings& projectSettings);
    ~EditorState();

    // ---- Initialization ----
    bool Init();

    // ---- Access ----
    Window* GetWindow() const { return m_window.get(); }
    IEditorRenderer* GetRenderer() const { return m_renderer.get(); }
    Scene* GetScene() const { return m_scene.get(); }
    ViewFactory* GetViewFactory() const { return m_viewFactory.get(); }
    ConsoleView* GetConsole() const { return m_primaryConsole; }
    PreferencesView* GetPreferences() const { return m_preferences.get(); }

    std::vector<std::unique_ptr<IEditorPanel>>& GetPanels() { return m_panels; }
    const std::vector<std::unique_ptr<IEditorPanel>>& GetPanels() const { return m_panels; }

    // ---- Save/Load State ----
    bool HasUnsavedChanges() const { return m_hasUnsavedChanges; }
    void SetHasUnsavedChanges(bool dirty) { m_hasUnsavedChanges = dirty; }

    void SaveScene();
    void LoadScene(const std::string& path);
    void CapturePlayModeScene();
    void RestorePlayModeScene();

    bool IsShowingPreferences() const { return m_showPreferences; }
    void SetShowPreferences(bool show) { m_showPreferences = show; }

    // ---- Scene Transition Callbacks ----
    std::function<void(const std::string&)> OnSceneLoadRequested;
    std::function<void()> OnSceneLoadConfirmed;

    // ---- Frame Timing ----
    float GetDeltaTime() const;
    void UpdateDeltaTime();

private:
    void InitializePanels();
    void WireupCallbacks();

    // Core objects
    std::unique_ptr<Window> m_window;
    std::unique_ptr<IEditorRenderer> m_renderer;
    std::unique_ptr<Scene> m_scene;
    std::unique_ptr<ViewFactory> m_viewFactory;
    std::unique_ptr<PreferencesView> m_preferences;

    // Panels
    std::vector<std::unique_ptr<IEditorPanel>> m_panels;
    ConsoleView* m_primaryConsole = nullptr;
    PropertiesView* m_primaryProperties = nullptr;

    // State
    ProjectSettings m_projectSettings;
    bool m_hasUnsavedChanges = false;
    bool m_showPreferences = false;
    std::string m_playModeSceneSnapshot;
    bool m_prePlayHasUnsavedChanges = false;

    std::string m_sceneToLoad;
    bool m_showUnsavedWarning = false;

    // Timing
    LARGE_INTEGER m_perfFreq, m_lastCounter;
};
