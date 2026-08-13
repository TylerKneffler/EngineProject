#pragma once
#include "View/View.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/ProjectLoader.h"
#include "Engine/Editor/Core/Gizmos/EditorGizmoSystem.h"

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
    SceneView()  = default;
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

    // Shows the offscreen texture with letterboxing/pillarboxing based on
    // aspect ratio settings, captures mouse input, and drives the
    // scene's editorCamera with orbit / pan / zoom.
    void DrawPanel(IEditorUi& ui) override;
    std::function<void(const std::string&)> OnAssetDropped;
    std::function<Engine::Core::Object*(const std::string&)> OnAssetPreviewRequested;
    std::function<void(Engine::Core::Object*)> OnAssetPreviewCancelled;
    std::function<void(Engine::Core::Object*, const std::string&)> OnAssetPreviewCommitted;
    std::function<void(Engine::Core::Object*)> OnObjectSelected;
    std::function<void(Engine::Core::Object*)> OnObjectCreated;
    std::function<void(bool)> OnGizmoInteraction;

private:
    Engine::Core::Object* PickObjectInViewport(const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize) const;
    bool FindPrefabPlacement(const EditorUiVec2& mousePos,
        const EditorUiVec2& viewportSize, glm::vec3& placement) const;
    void CancelPrefabPreview();
    void ApplyCameraControls(float panDX, float panDY,
                             float orbitDX, float orbitDY, float zoom);

    // Calculates the game viewport size and position based on aspect ratio mode
    Engine::Scene::Scene* m_scene = nullptr; // non-owning; set via Init()
    
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
    EditorGizmoSystem m_gizmos;
};
}
