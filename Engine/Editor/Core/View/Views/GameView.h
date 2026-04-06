#pragma once
#include "View/View.h"
#include "Core/Scene/Scene.h"

// ---------------------------------------------------------------------------
// GameView — editor Game panel
//
// Renders the scene from the perspective of the first Camera component found
// on any game object (not the editor camera), giving a preview of what the
// player will see at runtime.
//
// Unlike SceneView, this panel is purely a display surface — no camera
// controls are exposed. Lifecycle (Start / Update) is driven by the editor
// play state in Main.cpp, not here.
//
// ---- Per-frame call order ----
//   gameView.Init(device, w, h, srvCpu, srvGpu, &scene);
//   gameView.Render(cmdList, mainRtv, drawFn);   // inherited from View
//   gameView.DrawPanel();                        // overridden here
// ---------------------------------------------------------------------------
class GameView : public View
{
public:
    GameView()  = default;
    ~GameView() = default;

    // Calls View::Init then stores the scene pointer.
    // scene must outlive this view.
    void Init(ID3D12Device* device,
              uint32_t width, uint32_t height,
              D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
              D3D12_GPU_DESCRIPTOR_HANDLE srvGpu,
              Scene* scene);

    // Draws the "Game" ImGui window showing the game-camera render output.
    void DrawPanel() override;

private:
    Scene* m_scene = nullptr; // non-owning
};
