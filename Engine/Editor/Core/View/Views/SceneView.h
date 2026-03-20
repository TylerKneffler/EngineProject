#pragma once
#include "View/View.h"
#include "Core/Scene/Scene.h"

// ---------------------------------------------------------------------------
// SceneView — editor Scene panel
//
// Extends View with scene-camera orbit/pan/zoom controls driven by mouse
// input captured inside the "Scene" ImGui panel.
//
// ---- Per-frame call order ----
//   sceneViewport.Init(device, w, h, srvCpu, srvGpu, &scene);
//   sceneViewport.Render(cmdList, mainRtv, drawFn);   // inherited from View
//   sceneViewport.DrawPanel();                        // overridden here
// ---------------------------------------------------------------------------
class SceneView : public View
{
public:
    SceneView()  = default;
    ~SceneView() = default;

    // Calls View::Init then stores the scene pointer.
    // scene must outlive this viewport; its editorCamera drives all controls.
    void Init(ID3D12Device* device,
              uint32_t width, uint32_t height,
              D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
              D3D12_GPU_DESCRIPTOR_HANDLE srvGpu,
              Scene* scene);

    // Shows the offscreen texture, captures mouse input, and drives the
    // scene's editorCamera with orbit / pan / zoom.
    void DrawPanel() override;

private:
    void ApplyCameraControls(float panDX, float panDY,
                             float orbitDX, float orbitDY, float zoom);

    Scene* m_scene = nullptr; // non-owning; set via Init()
};
