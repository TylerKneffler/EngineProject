#pragma once
#include <algorithm>
#include "View/View.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/ProjectLoader.h"
#include "Engine/Editor/Core/Gizmos/EditorGizmoSystem.h"
#include "Scene/SceneViewportToolbar.h"
#include "Core/Serialization/PreparedPrefab.h"
#include "Engine/Editor/Core/View/Templates/EditModes/ViewportModeTemplate.h"

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// SceneView — editor Scene panel
//
// Extends View with scene-camera orbit/pan/zoom controls driven by mouse
// input captured inside the Scene panel. Supports aspect ratio
// constraints with letterboxing/pillarboxing visualization.
//
// ---- Per-frame call order ----
//   sceneViewport.Init(device, w, h, srvCpu, srvGpu, &scene, projectSettings);
//   sceneViewport.Render(cmdList, mainRtv, drawFn);   // inherited from View
//   sceneViewport.DrawPanel();                        // overridden here
// ---------------------------------------------------------------------------
class SceneView : public View
{
public:
    SceneView();
    ~SceneView();

    // Calls View::Init then stores the scene pointer and aspect ratio settings.
    // scene and settings must outlive this viewport.
    // device: opaque renderer device handle (cast internally to ID3D12Device*)
    // srvCpu/srvGpu: opaque descriptor handles (cast internally from void*)
    void Init(void* device,
              uint32_t width, uint32_t height,
              void* srvCpu,
              void* srvGpu,
              uint32_t srvSlotIndex,
              Engine::Scene::Scene* scene,
              const Engine::Model::ProjectSettings& settings);

    // Issues the editor-camera scene draw into cmd each frame.
    // cmd: opaque graphics command list handle (cast internally to ID3D12GraphicsCommandList*)
    void Render3D(void* cmd) override;
    void RenderShadow3D(void* cmd) override;
    void RecordRenderCommands(Engine::Graphics::GraphicsCommandPacket& shadow,
        Engine::Graphics::GraphicsCommandPacket& color,
        const Engine::Scene::Scene::NativeRenderPacket* packet);

    // Shows the offscreen texture with letterboxing/pillarboxing based on
    // aspect ratio settings, captures mouse input, and drives the
    // scene's editorCamera with FPS-style look / pan / zoom.
    void DrawPanel(IEditorUi& ui) override;
    Engine::Scene::Scene* GetScene() const { return m_scene; }
    void SetScene(Engine::Scene::Scene* scene);
    bool ContainsScreenPoint(float x, float y) const
    { return m_viewportScreenMax.x > m_viewportScreenMin.x &&
        x >= m_viewportScreenMin.x && x < m_viewportScreenMax.x &&
        y >= m_viewportScreenMin.y && y < m_viewportScreenMax.y; }
    void SetEditMode(SceneEditMode mode) { m_editMode = mode; }
    SceneEditMode GetEditMode() const { return m_editMode; }
    EditorTransformTool GetTransformTool() const
    { return m_toolbar.GetTransformTool(); }
    void SetTransformTool(EditorTransformTool tool)
    { m_toolbar.SetTransformTool(tool); }
    void SetPoseEditing(bool enabled) { m_gizmos.SetPoseEditing(enabled); }
    void SetModelManipulation(bool enabled) { m_modelManipulation = enabled; }
    void SetModelSelectionMode(int mode)
    { m_modelSelectionMode = std::clamp(mode, 0, 3); }
    const std::vector<Engine::Core::Object*>& ModelChangedBones() const
    { return m_gizmos.ModelChangedBones(); }
    bool ModelDragEnded() const { return m_gizmos.ModelDragEnded(); }
    void SetPoseTranslationHandler(std::function<bool(Engine::Core::Object*,
        const glm::vec3&)> handler)
    { m_gizmos.SetPoseTranslationHandler(std::move(handler)); }
    void SetBoneTransformSettings(int pivotMode, const glm::vec3& customPivot,
        float translationSnap, float rotationSnapDegrees, float scaleSnap)
    {
        m_gizmos.SetBoneTransformSettings(pivotMode, customPivot,
            translationSnap, rotationSnapDegrees, scaleSnap);
    }
    void DrawPoseToolbar(const std::function<bool(float)>& next,
        bool animationEditor = false, bool inlineOptions = false);
    void DrawTerrainToolbar(const std::function<bool(float)>& next);
    bool UseGlobalToolbar = false;
    bool UseGlobalBoneVisibility = true;
    void SetDocumentPath(std::string path) { m_documentPath = std::move(path); }
    void RequestFocusOnNextDraw() { m_focusOnNextDraw = true; }
    std::function<void(const std::string&)> OnAssetDropped;
    std::function<void(const std::string&, Engine::Core::Object*)> OnAssetAssigned;
    std::function<Engine::Core::Object*(const std::string&)> OnAssetPreviewRequested;
    std::function<Engine::Core::Object*(Engine::Scene::Scene*, Engine::Serialization::PreparedPrefab&)> OnPreparedAssetPreviewRequested;
    bool HasPendingPrefabPreparation() const { return m_prefabPreparation.valid() || m_prefabDropPending; }
    std::function<void(Engine::Core::Object*)> OnAssetPreviewCancelled;
    std::function<void(Engine::Core::Object*, const std::string&)> OnAssetPreviewCommitted;
    std::function<void(Engine::Core::Object*)> OnObjectSelected;
    std::function<void(Engine::Core::Object*)> OnObjectCreated;
    std::function<void()> OnDeleteSelectionRequested;
    std::function<void(bool)> OnGizmoInteraction;
    std::function<void()> OnKeyModelControllersRequested;
    std::function<void()> OnModelControllerEdited;
    std::function<bool(IEditorUi&)> OnDrawDocumentTools;
    std::function<void(IEditorUi&)> OnDrawBottomPanel;
    std::function<bool(IEditorUi&, const EditorUiViewportInput&,
        EditorTransformTool)> OnMeshViewportInput;
    std::function<bool(IEditorUi&, const EditorUiViewportInput&)> OnDrawRigControls;
    std::function<void(IEditorUi&)> OnDocumentDrop;
    std::function<bool(const Engine::Core::Object*)> CanSelectObject;
    bool AllowObjectCreation = true;
    bool AllowAssetDrops = true;
    bool AllowObjectTransform = true;
    float BottomPanelHeight = 420.f;

protected:
    float EffectiveRenderScale() const override;
private:
    void CancelPrefabPreview();

    // Calculates the game viewport size and position based on aspect ratio mode
    Engine::Scene::Scene* m_scene = nullptr; // non-owning; set via Init()
    EditorUiVec2 m_viewportScreenMin{}, m_viewportScreenMax{};
    std::string m_documentPath;
    
    // Aspect ratio settings
    Engine::Model::ProjectSettings::AspectRatioMode m_aspectRatioMode = Engine::Model::ProjectSettings::AspectRatioMode::Locked;
    float m_gameAspectRatio = 1.777f;  // 16:9
    uint32_t m_gameWindowWidth = 1920;
    uint32_t m_gameWindowHeight = 1080;
    glm::vec4 m_letterboxColor{0.f, 0.f, 0.f, 1.f};

    // Computed viewport aspect ratio
    float m_aspect = 1.0f;
    Engine::Core::Object* m_prefabPreview = nullptr;
    Engine::Core::Object* m_previousSelectedObject = nullptr;
    std::string m_prefabPreviewPath;
    bool m_prefabPreviewHasPlacement = false;
    std::future<std::unique_ptr<Engine::Serialization::PreparedPrefab>> m_prefabPreparation;
    std::shared_ptr<std::atomic_bool> m_prefabPreparationCanceled;
    glm::vec3 m_prefabPlacement{};
    bool m_prefabDropPending = false;
    Engine::Scene::Scene::SceneGeneration m_prefabPreparationGeneration = 0;
    std::string m_prefabPreparationError;
    void PollPrefabPreparation();
    void CommitPrefabPreview();
    bool m_focusOnNextDraw = false;
    SceneEditMode m_editMode = SceneEditMode::Object;
    int m_terrainBrush = 0;
    int m_terrainLayer = 0;
    float m_terrainRadius = 3.f;
    float m_terrainStrength = .25f;
    bool m_editTerrainCells = false;
    std::string m_terrainObjectPrefab;
    bool m_terrainAlignObjects = true;
    float m_terrainObjectSpacing = 2.f;
    glm::vec3 m_lastTerrainObjectPosition{0.f};
    bool m_terrainObjectStroke = false;
    bool m_modelManipulation = false;
    int m_modelSelectionMode = 2;
    int m_modelAction = 0;
    bool m_modelScrollDepth = false;
    int m_modelScope = 1;
    EditorGizmoSystem m_gizmos;
    SceneViewportToolbar m_toolbar;
};
}
