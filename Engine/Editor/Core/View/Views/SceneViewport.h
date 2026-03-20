#pragma once
#include "View/View.h"

class Scene; // full definition in Core/Scene/Scene.h — only a pointer is stored here

// ---------------------------------------------------------------------------
// SceneViewport — editor Scene panel
//
// Extends View with scene-camera orbit/pan/zoom controls driven by mouse
// input captured inside the "Scene" ImGui panel.
//
// ---- Per-frame call order ----
//   sceneViewport.Init(device, w, h, srvCpu, srvGpu, &scene);
//   sceneViewport.Render(cmdList, mainRtv, drawFn);   // inherited from View
//   sceneViewport.DrawPanel();                        // overridden here
// ---------------------------------------------------------------------------
class SceneViewport : public View
{
public:
    SceneViewport()  = default;
    ~SceneViewport() = default;

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
