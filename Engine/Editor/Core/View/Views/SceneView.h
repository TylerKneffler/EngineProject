#pragma once
#include "View/View.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/ProjectLoader.h"
#include "Engine/Editor/Core/Gizmos/EditorGizmoSystem.h"
#include "Scene/SceneViewportToolbar.h"
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

    // Shows the offscreen texture with letterboxing/pillarboxing based on
    // aspect ratio settings, captures mouse input, and drives the
    // scene's editorCamera with FPS-style look / pan / zoom.
    void DrawPanel(IEditorUi& ui) override;
    Engine::Scene::Scene* GetScene() const { return m_scene; }
    void SetEditMode(SceneEditMode mode) { m_editMode = mode; }
    SceneEditMode GetEditMode() const { return m_editMode; }
    EditorTransformTool GetTransformTool() const
    { return m_toolbar.GetTransformTool(); }
    void SetTransformTool(EditorTransformTool tool)
    { m_toolbar.SetTransformTool(tool); }
    void SetBoneTransformSettings(int pivotMode, const glm::vec3& customPivot,
        float translationSnap, float rotationSnapDegrees, float scaleSnap)
    {
        m_gizmos.SetBoneTransformSettings(pivotMode, customPivot,
            translationSnap, rotationSnapDegrees, scaleSnap);
    }
    bool UseGlobalToolbar = false;
    void SetDocumentPath(std::string path) { m_documentPath = std::move(path); }
    void RequestFocusOnNextDraw() { m_focusOnNextDraw = true; }
    std::function<void(const std::string&)> OnAssetDropped;
    std::function<Engine::Core::Object*(const std::string&)> OnAssetPreviewRequested;
    std::function<void(Engine::Core::Object*)> OnAssetPreviewCancelled;
    std::function<void(Engine::Core::Object*, const std::string&)> OnAssetPreviewCommitted;
    std::function<void(Engine::Core::Object*)> OnObjectSelected;
    std::function<void(Engine::Core::Object*)> OnObjectCreated;
    std::function<void()> OnDeleteSelectionRequested;
    std::function<void(bool)> OnGizmoInteraction;
    std::function<bool(IEditorUi&)> OnDrawDocumentTools;
    std::function<void(IEditorUi&)> OnDrawBottomPanel;
    std::function<bool(IEditorUi&, const EditorUiViewportInput&,
        EditorTransformTool)> OnMeshViewportInput;
    std::function<bool(const Engine::Core::Object*)> CanSelectObject;
    bool AllowObjectCreation = true;
    bool AllowAssetDrops = true;
    bool AllowObjectTransform = true;
    float BottomPanelHeight = 420.f;

private:
    void CancelPrefabPreview();

    // Calculates the game viewport size and position based on aspect ratio mode
    Engine::Scene::Scene* m_scene = nullptr; // non-owning; set via Init()
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
    std::string m_prefabPreviewPath;
    bool m_prefabPreviewHasPlacement = false;
    bool m_focusOnNextDraw = false;
    SceneEditMode m_editMode = SceneEditMode::Object;
    EditorGizmoSystem m_gizmos;
    SceneViewportToolbar m_toolbar;
};
}
