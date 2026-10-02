#pragma once
#include "Core/Model/ProjectSettings.h"
#include "pch.h"
#include "Core/ProjectLoader.h"
#include "Core/Scene/Scene.h"
#include "Core/Renderers/IEditorRenderer.h"
#include <memory>
#include <vector>
#include <functional>
#include <deque>
#include <cstdint>
#include <chrono>
#include <unordered_set>
#include <unordered_map>

namespace Engine::Core { class Window; }
namespace Engine::Components { class Mesh; }
namespace Engine::Components { class Sprite; }
namespace Engine::Components { class Material; }
namespace Engine::Components { class Skeleton; }
namespace Engine::Components { class AnimationManager; }

namespace Engine::Editor
{
class ViewFactory;
class IEditorPanel;
class PreferencesView;
class ConsoleView;
class PropertiesView;
class HierarchyView;
class AssetsExplorerView;
class SceneView;
class AssetDocumentView;
class IEditorUi;

// ---------------------------------------------------------------------------
// EditorState — Encapsulates all editor application state
// ---------------------------------------------------------------------------
class EditorState
{
public:
    EditorState(HINSTANCE hInstance, const Engine::Model::ProjectSettings& projectSettings,
        std::string projectFilePath);
    ~EditorState();

    // ---- Initialization ----
    bool Init();
    void InitializeUiState();

    // ---- Access ----
    ::Engine::Core::Window* GetWindow() const { return m_window.get(); }
    ::Engine::Renderers::IEditorRenderer* GetRenderer() const { return m_renderer.get(); }
    Engine::Scene::Scene* GetScene() const { return m_scene.get(); }
    ViewFactory* GetViewFactory() const { return m_viewFactory.get(); }
    ConsoleView* GetConsole() const { return m_primaryConsole; }
    PreferencesView* GetPreferences() const { return m_preferences.get(); }

    std::vector<std::unique_ptr<IEditorPanel>>& GetPanels() { return m_panels; }
    const std::vector<std::unique_ptr<IEditorPanel>>& GetPanels() const { return m_panels; }

    // ---- Save/Load State ----
    bool HasUnsavedChanges() const;
    bool HasActiveDocumentUnsavedChanges() const;
    void SetHasUnsavedChanges(bool dirty) { m_hasUnsavedChanges = dirty; }

    void SaveScene();
    void SaveAll();
    void LoadScene(const std::string& path);
    void RequestSceneLoad(const std::string& path);
    bool ConfirmSceneLoad(bool saveCurrentScene);
    void CancelSceneLoad();
    bool HasPendingSceneLoadWarning() const { return m_showUnsavedWarning; }
    const std::string& GetPendingSceneLoadPath() const { return m_sceneToLoad; }
    void OpenPrefabStage(const std::string& path);
    void ProcessPendingPrefabStageOpen();
    void ClosePrefabStage();
    void HandlePrefabPanelClosures();
    void QueueAssetDocumentOpen(const std::string& path,
        const std::string& editorType);
    void ProcessPendingAssetDocumentOpens();
    void HandleAssetDocumentClosures();
    void QueueSceneAssetDocumentOpen(const std::string& path);
    void ProcessPendingSceneAssetDocumentOpens();
    void HandleSceneAssetDocumentClosures();
    bool IsEditingPrefab() const;
    std::string GetActiveDocumentName() const;
    void BakeLighting();
    void ClearBakedLighting();
    void ImportAsset();
    void CapturePlayModeScene();
    void RestorePlayModeScene();
    void SetLoadingOverlay(bool visible, std::string message = {})
    {
        m_loadingOverlayVisible = visible;
        m_loadingOverlayMessage = std::move(message);
    }
    bool IsLoadingOverlayVisible() const { return m_loadingOverlayVisible; }
    const std::string& GetLoadingOverlayMessage() const { return m_loadingOverlayMessage; }
    void RefreshSelectionAfterReload(const ::Engine::Scene::Scene::ObjectPath& selectedPath);

    // ---- Undo/Redo ----
    void TrackSceneChanges(bool allowHistory = true, bool editInProgress = false);
    void Undo();
    void Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    void SetHistoryLimit(uint32_t limit);
    void ResetSceneEditInProgress() { m_sceneEditInProgress = false; }
    void ReportSceneEditInProgress(bool active)
    {
        m_sceneEditInProgress = m_sceneEditInProgress || active;
        if (active)
            MarkSceneEdited();
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
    Engine::Core::Object* InstantiateAsset(const std::string& path, bool recordChange = true);
    std::string ImportAssetFile(const std::string& path);
    void SelectObject(Engine::Core::Object* object);
    struct HistoryEntry
    {
        std::string scene;
        bool hasSelection = false;
        ::Engine::Scene::Scene::ObjectPath selectionPath;
    };
    HistoryEntry CaptureHistoryEntry() const;
    void CaptureHistorySelection(HistoryEntry& entry) const;
    void MarkSceneEdited();
    void MarkHistorySelectionChanged()
    {
        m_historySelectionDirty = true;
        if (m_renderer) m_renderer->MarkDirty();
    }
    void ApplyHistoryEntry(HistoryEntry entry, const char* operation);
    void ResetHistory(bool sceneIsSaved);
    void CommitPendingHistoryEdit();
    void TrimHistory();
    bool SaveMainScene();
    void LoadSceneNow(const std::string& path);
    void RemovePrefabPanels();
    void SetPrefabDocumentFocused(bool focused);
    void SetPrefabDirty(bool dirty);
    void RefreshPrefabDocumentTitle();
    void SetActiveAssetDocument(AssetDocumentView* document);
    struct SceneAssetDocument
    {
        std::string path;
        std::string identity;
        bool prefab = false;
        bool meshStage = false;
        bool modelStage = false;
        bool skeletonStage = false;
        bool objectStage = false;
        bool showChildHierarchy = true;
        std::string stageDataPath;
        Engine::Components::Mesh* mesh = nullptr;
        Engine::Core::Object* subject = nullptr;
        Engine::Core::Object* propertiesProjection = nullptr;
        std::unordered_set<Engine::Core::Object*> selectableObjects;
        std::unordered_set<Engine::Core::Object*> skeletonMeshObjects;
        std::unordered_map<Engine::Core::Object*, bool> originalEnabledState;
        std::unordered_map<Engine::Components::Mesh*, bool> originalMeshVisibility;
        std::unordered_map<Engine::Components::Sprite*, bool> originalSpriteVisibility;
        struct GhostMaterialState
        {
            Engine::Components::Material* material = nullptr;
            float alpha = 1.f;
            std::string alphaMode;
            bool castsShadows = true;
        };
        std::vector<GhostMaterialState> ghostMaterialStates;
        Engine::Components::Skeleton* skeleton = nullptr;
        bool showSkeletonMesh = true;
        Engine::Components::AnimationManager* previewAnimationManager = nullptr;
        bool previewAnimation = false;
        std::chrono::steady_clock::time_point lastPreviewTick{};
        std::string previewRestoreSnapshot;
        std::string meshSavePath;
        std::unique_ptr<Engine::Scene::Scene> scene;
        SceneView* view = nullptr;
        std::function<void(IEditorUi&)> objectStageTools;
        std::function<void(IEditorUi&)> meshStageTools;
        std::function<void(IEditorUi&)> skeletonStageTools;
        int assetToolsTab = -1;
        std::string baseline;
        std::string savedSnapshot;
        std::deque<std::string> undo;
        std::deque<std::string> redo;
        bool dirty = false;
        uint32_t selectedVertex = 0;
        uint32_t selectedInfluence = 0;
    };
    void SetActiveSceneAssetDocument(SceneAssetDocument* document);
    bool SaveSceneAssetDocument(SceneAssetDocument& document);
    std::string CaptureSceneAssetDocumentSnapshot(
        SceneAssetDocument& document);
    bool RestoreSceneAssetDocumentSnapshot(SceneAssetDocument& document,
        const std::string& snapshot);
    void ApplySkeletonStageVisibility(SceneAssetDocument& document);
    void RestoreSkeletonStageVisibility(SceneAssetDocument& document);
    void RebuildSkeletonStageContext(SceneAssetDocument& document);
    void RebuildObjectStageContext(SceneAssetDocument& document);
    void ApplyObjectStageVisibility(SceneAssetDocument& document);
    void ApplyMeshStageVisibility(SceneAssetDocument& document);
    void StopSkeletonAnimationPreview(SceneAssetDocument& document);
    void RefreshSceneAssetDocumentTitle(SceneAssetDocument& document);
    void RefreshSceneDocumentTitle();
    Engine::Scene::Scene* GetActiveDocumentScene() const;

    // Core objects
    std::unique_ptr<::Engine::Core::Window> m_window;
    std::unique_ptr<::Engine::Renderers::IEditorRenderer> m_renderer;
    std::unique_ptr<Engine::Scene::Scene> m_scene;
    std::unique_ptr<Engine::Scene::Scene> m_prefabScene;
    std::unique_ptr<ViewFactory> m_viewFactory;
    std::unique_ptr<PreferencesView> m_preferences;

    // Panels
    std::vector<std::unique_ptr<IEditorPanel>> m_panels;
    ConsoleView* m_primaryConsole = nullptr;
    HierarchyView* m_primaryHierarchy = nullptr;
    PropertiesView* m_primaryProperties = nullptr;
    AssetsExplorerView* m_primaryAssets = nullptr;
    SceneView* m_prefabSceneView = nullptr;
    std::vector<AssetDocumentView*> m_assetDocuments;
    AssetDocumentView* m_activeAssetDocument = nullptr;
    std::vector<std::unique_ptr<SceneAssetDocument>> m_sceneAssetDocuments;
    std::deque<std::string> m_pendingSceneAssetDocuments;
    SceneAssetDocument* m_activeSceneAssetDocument = nullptr;

    // State
    Engine::Model::ProjectSettings m_projectSettings;
    std::string m_projectFilePath;
    std::string m_currentScenePath;
    std::string m_activePrefabPath;
    std::string m_pendingPrefabPath;
    std::deque<std::pair<std::string, std::string>> m_pendingAssetDocuments;
    bool m_prefabHasUnsavedChanges = false;
    bool m_prefabDocumentFocused = false;
    bool m_hasUnsavedChanges = false;
    bool m_showPreferences = false;
    bool m_loadingOverlayVisible = false;
    std::string m_loadingOverlayMessage;
    std::string m_playModeSceneSnapshot;
    bool m_prePlayHasUnsavedChanges = false;
    bool m_prePlayHadObjectSelection = false;
    ::Engine::Scene::Scene::ObjectPath m_prePlaySelectionPath;

    std::deque<HistoryEntry> m_undoHistory;
    std::deque<HistoryEntry> m_redoHistory;
    HistoryEntry m_historyBaseline;
    HistoryEntry m_pendingHistoryBefore;
    std::string m_savedSceneSnapshot;
    uint32_t m_historyLimit = 100;
    bool m_hasPendingHistoryEdit = false;
    bool m_assetPreviewActive = false;
    bool m_sceneEditInProgress = false;
    bool m_mainSceneGizmoWasActive = false;
    uint64_t m_sceneEditRevision = 0;
    uint64_t m_historyCapturedRevision = 0;
    bool m_historySelectionDirty = false;

    std::string m_sceneToLoad;
    bool m_showUnsavedWarning = false;

    // Timing
    LARGE_INTEGER m_perfFreq, m_lastCounter;
    float m_deltaTime = 0.0f;
};
}
