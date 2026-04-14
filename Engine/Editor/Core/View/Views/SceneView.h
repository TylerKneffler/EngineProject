#pragma once
#include "View/View.h"
#include "Core/Scene/Scene.h"
#include "Core/ProjectLoader.h"

// ---------------------------------------------------------------------------
// SceneView — editor Scene panel
//
// Extends View with scene-camera orbit/pan/zoom controls driven by mouse
// input captured inside the "Scene" ImGui panel. Supports aspect ratio
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
    ~SceneView() = default;

    // Calls View::Init then stores the scene pointer and aspect ratio settings.
    // scene and settings must outlive this viewport.
    void Init(ID3D12Device* device,
              uint32_t width, uint32_t height,
              D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
              D3D12_GPU_DESCRIPTOR_HANDLE srvGpu,
              Scene* scene,
              const ProjectSettings& settings);

    // Shows the offscreen texture with letterboxing/pillarboxing based on
    // aspect ratio settings, captures mouse input, and drives the
    // scene's editorCamera with orbit / pan / zoom.
    void DrawPanel() override;

private:
    void ApplyCameraControls(float panDX, float panDY,
                             float orbitDX, float orbitDY, float zoom);

    // Calculates the game viewport size and position based on aspect ratio mode
    void CalculateGameViewport(ImVec2 availableSize, ImVec2& outViewportSize, ImVec2& outViewportPos);

    Scene* m_scene = nullptr; // non-owning; set via Init()
    
    // Aspect ratio settings
    ProjectSettings::AspectRatioMode m_aspectRatioMode = ProjectSettings::AspectRatioMode::Locked;
    float m_gameAspectRatio = 1.777f;  // 16:9
    uint32_t m_gameWindowWidth = 1920;
    uint32_t m_gameWindowHeight = 1080;
    glm::vec4 m_letterboxColor{0.f, 0.f, 0.f, 1.f};
};
