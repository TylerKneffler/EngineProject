#pragma once
#include "pch.h"
#include "Core/ProjectLoader.h"
#include "Core/Scene/Scene.h"
#include "Core/Renderers/IEditorRenderer.h"
#include <memory>
#include <vector>
#include <functional>
#include <deque>

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
    EditorState(HINSTANCE hInstance, const ProjectSettings& projectSettings,
        std::string projectFilePath);
    ~EditorState();

    // ---- Initialization ----
    bool Init();
    void InitializeUiState();

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
    void ImportAsset();
    void CapturePlayModeScene();
    void RestorePlayModeScene();

    // ---- Undo/Redo ----
    void TrackSceneChanges(bool allowHistory = true, bool editInProgress = false);
    void Undo();
    void Redo();
    bool CanUndo() const { return m_hasPendingHistoryEdit || !m_undoHistory.empty(); }
    bool CanRedo() const { return !m_redoHistory.empty(); }
    void SetHistoryLimit(uint32_t limit);
    void ResetSceneEditInProgress() { m_sceneEditInProgress = false; }
    void ReportSceneEditInProgress(bool active)
    {
        m_sceneEditInProgress = m_sceneEditInProgress || active;
    }
    bool IsSceneEditInProgress() const { return m_sceneEditInProgress; }

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
    Object* InstantiateAsset(const std::string& path, bool recordChange = true);
    std::string ImportAssetFile(const std::string& path);
    void SelectObject(Object* object);
    struct HistoryEntry
    {
        std::string scene;
        bool hasSelection = false;
        Scene::ObjectPath selectionPath;
    };
    HistoryEntry CaptureHistoryEntry() const;
    void ApplyHistoryEntry(const HistoryEntry& entry, const char* operation);
    void ResetHistory(bool sceneIsSaved);
    void CommitPendingHistoryEdit();
    void TrimHistory();

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
    std::string m_projectFilePath;
    std::string m_currentScenePath;
    bool m_hasUnsavedChanges = false;
    bool m_showPreferences = false;
    std::string m_playModeSceneSnapshot;
    bool m_prePlayHasUnsavedChanges = false;
    bool m_prePlayHadObjectSelection = false;
    Scene::ObjectPath m_prePlaySelectionPath;

    std::deque<HistoryEntry> m_undoHistory;
    std::deque<HistoryEntry> m_redoHistory;
    HistoryEntry m_historyBaseline;
    HistoryEntry m_pendingHistoryBefore;
    std::string m_savedSceneSnapshot;
    uint32_t m_historyLimit = 100;
    bool m_hasPendingHistoryEdit = false;
    bool m_assetPreviewActive = false;
    bool m_sceneEditInProgress = false;

    std::string m_sceneToLoad;
    bool m_showUnsavedWarning = false;

    // Timing
    LARGE_INTEGER m_perfFreq, m_lastCounter;
    float m_deltaTime = 0.0f;
};
