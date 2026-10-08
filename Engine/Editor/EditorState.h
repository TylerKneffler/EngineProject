#pragma once
#include "Core/Model/ProjectSettings.h"
#include "pch.h"
#include "Core/ProjectLoader.h"
#include "Core/Scene/Scene.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "UI/IEditorUi.h"
#include "Core/Gizmos/EditorGizmoSystem.h"
#include "Core/View/Templates/EditModes/Skeleton/WeightPaintSurface.h"
#include <memory>
#include <vector>
#include <functional>
#include <deque>
#include <cstdint>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <utility>

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
    struct ToolbarVisibility
    {
        bool editMode = true;
        bool playControls = true;
        bool save = true;
        bool undoRedo = true;
        bool modeTools = true;
        bool transformTools = true;
        bool sceneDisplay = true;
        bool toolDetails = true;
        bool prefabActions = true;
    };

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
    void DrawGlobalToolbar(IEditorUi& ui);
    void DrawToolbarTools(IEditorUi& ui);
    float GetGlobalToolbarHeight() const;
    bool HasToolbarToolRow() const;
    ToolbarVisibility& GetToolbarVisibility() { return m_toolbarVisibility; }
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
    struct MeshEditSession
    {
        struct EdgeCache
        {
            const Engine::Components::Mesh* mesh = nullptr;
            size_t vertexCount = 0;
            std::vector<uint32_t> indices;
            std::vector<std::pair<uint32_t, uint32_t>> edges;
        };
        bool enabled = false;
        Engine::Components::Mesh* activeMesh = nullptr;
        int selectionMode = 0; // vertex, edge, face
        uint32_t selectedElement = 0;
        std::vector<uint32_t> selectedElements;
        std::shared_ptr<EdgeCache> edgeCache;
        int selectionTool = 0; // click, box, lasso
        int pivotMode = 0;
        float rotate[3]{};
        float scale[3]{ 1.f, 1.f, 1.f };
        float customPivot[3]{};
        float snapStep = 0.f;
        float rotationSnap = 0.f;
        float scaleSnap = 0.f;
        float bevelWidth = .15f;
        std::string toolError;
        bool selecting = false;
        EditorUiVec2 selectionStart{};
        std::vector<EditorUiVec2> lasso;
        bool gizmoDragging = false;
        bool gizmoChanged = false;
        int gizmoAxis = -1;
        EditorTransformTool gizmoTool = EditorTransformTool::Translate;
        EditorUiVec2 gizmoStartMouse{};
        EditorUiVec2 gizmoScreenDirection{};
        float gizmoUnitsPerPixel = 0.f;
        float gizmoLastPixels = 0.f;
        std::vector<std::pair<uint32_t, std::array<float, 3>>>
            gizmoStartPositions;
        std::string gizmoBeforeSnapshot;
        std::string savePath;
        std::string baseline;
        std::string savedSnapshot;
        std::deque<std::string> undo;
        std::deque<std::string> redo;
        bool dirty = false;
        char search[96]{};
        float move[3]{};
        float extrudeDistance = 0.25f;
        float insetAmount = 0.2f;
    };
    MeshEditSession* ActiveMeshEditSession();
    Engine::Components::Mesh* ResolveMeshForSelection(
        Engine::Core::Object* selected) const;
    void SyncMeshEditSelection(Engine::Scene::Scene* scene,
        MeshEditSession& session);
    void SetMeshEditMode(Engine::Scene::Scene* scene,
        MeshEditSession& session, bool enabled);
    void DrawMeshEditTools(IEditorUi& ui);
    const std::vector<std::pair<uint32_t, uint32_t>>& CachedMeshEdges(
        MeshEditSession& session, const Engine::Components::Mesh& mesh);
    bool HandleMeshViewport(IEditorUi& ui,
        const EditorUiViewportInput& input, Engine::Scene::Scene* scene,
        MeshEditSession& session, EditorTransformTool tool);
    struct SkeletonEditSession;
    void DrawObjectEditTools(IEditorUi& ui);
    bool SaveMeshEditSession(MeshEditSession& session);
    bool SavePendingMeshEdits();
    void InvalidateMeshEditPointers();
    bool ApplyMeshHistory(bool redo);
    struct SkeletonEditSession
    {
        struct WeightAdjacencyCache
        {
            const Engine::Components::Mesh* mesh = nullptr;
            size_t vertexCount = 0;
            std::vector<uint32_t> indices;
            std::vector<std::vector<uint32_t>> neighbors;
        };
        bool enabled = false;
        int submode = 0; // bone edit, weight paint, skin binding
        int boneTool = 0; // edit, add, remove
        char boneName[128]{};
        int reparentBoneIndex = -1;
        int pivotMode = 0; // joint, parent, skeleton root, custom world point
        float customPivot[3]{};
        float translationSnap = 0.f;
        float rotationSnap = 0.f;
        float scaleSnap = 0.f;
        int brushOperation = 0; // add, subtract, replace, smooth
        int brushShape = 0; // circle, square
        int brushFalloff = 0; // hard, linear, smooth
        float brushRadius = 40.f;
        float brushHardness = 0.5f;
        float brushStrength = 0.35f;
        float brushPaintWeight = 1.f;
        std::vector<bool> lockedBones;
        std::shared_ptr<WeightAdjacencyCache> adjacency;
        std::shared_ptr<WeightPaintSurface::Topology> surfaceTopology;
        Engine::Components::Skeleton* skeleton = nullptr;
        Engine::Components::Mesh* mesh = nullptr;
        Engine::Components::Mesh* bindingMesh = nullptr;
        int boneIndex = -1;
        int mirrorBoneIndex = -1; // -1 disables paired-bone painting
        char bindingVertexIds[256]{};
        std::string bindingStatus;
        Engine::Core::Object* observedSelection = nullptr;
        uint64_t observedStructureRevision = 0;
        int observedSubmode = -1;
        std::vector<glm::mat4> observedBindTransforms;
        bool painting = false;
        bool strokeChanged = false;
        EditorUiVec2 lastPaintPosition{};
        std::string strokeBefore;
        enum class HistoryKind { Scene, Mesh, Binding };
        struct HistoryAction
        {
            struct AssetEdit
            {
                ::Engine::Scene::Scene::ObjectPath beforePath;
                ::Engine::Scene::Scene::ObjectPath afterPath;
                std::string savePath;
                std::string before;
                std::string after;
            };
            HistoryKind kind = HistoryKind::Scene;
            ::Engine::Scene::Scene::ObjectPath meshPath;
            std::string meshSavePath;
            std::vector<AssetEdit> assetEdits;
        };
        std::deque<HistoryAction> undoOrder;
        std::deque<HistoryAction> redoOrder;
        bool pendingBindingMeshHistory = false;
        std::vector<HistoryAction::AssetEdit> pendingAssetEdits;
        ::Engine::Scene::Scene::ObjectPath historyMeshPath;
        std::string error;
    };
    SkeletonEditSession* ActiveSkeletonEditSession();
    void SetSkeletonEditMode(Engine::Scene::Scene* scene,
        SkeletonEditSession& session, MeshEditSession& meshSession,
        bool enabled);
    void SyncSkeletonEditSelection(Engine::Scene::Scene* scene,
        SkeletonEditSession& session);
    void DrawSkeletonEditTools(IEditorUi& ui);
    bool HandleSkeletonViewport(IEditorUi& ui,
        const EditorUiViewportInput& input, Engine::Scene::Scene* scene,
        SkeletonEditSession& session);
    void FinishSkeletonPaintStroke(SkeletonEditSession& session);
    void RecordSkeletonHistory(SkeletonEditSession& session,
        SkeletonEditSession::HistoryKind kind);
    bool ApplySkeletonHistory(bool redo);
    void RefreshSkeletonBindPose(SkeletonEditSession& session);
    bool ApplySkeletonBoneAction(Engine::Scene::Scene* scene,
        SkeletonEditSession& session, int action);
    bool ApplySkinBindingAction(Engine::Scene::Scene* scene,
        SkeletonEditSession& session, int action);
    void CommitSkeletonEdit(Engine::Scene::Scene* scene);
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
        std::function<void(IEditorUi&)> focusPicker;
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
        MeshEditSession meshEdit;
        SkeletonEditSession skeletonEdit;
    };
    void DrawObjectStageTools(IEditorUi& ui, SceneAssetDocument& document);
    void DrawMeshStageTools(IEditorUi& ui, SceneAssetDocument& document);
    void SetActiveSceneAssetDocument(SceneAssetDocument* document,
        bool refresh = false);
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
    MeshEditSession m_mainMeshEdit;
    MeshEditSession m_prefabMeshEdit;
    SkeletonEditSession m_mainSkeletonEdit;
    SkeletonEditSession m_prefabSkeletonEdit;
    std::unordered_map<std::string, MeshEditSession> m_meshEditCache;

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
    bool m_gameViewFocused = false;
    ToolbarVisibility m_toolbarVisibility;
    std::string m_toolbarPopupTool;
    Engine::Scene::Scene* m_toolbarPopupScene = nullptr;
    int m_toolbarPopupMode = 0;
    std::string m_toolbarDirectTool;
    char m_meshToolbarSearch[64]{};
    Engine::Scene::Scene* m_meshToolbarScene = nullptr;
    int m_meshToolbarSelectionMode = 0;
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
