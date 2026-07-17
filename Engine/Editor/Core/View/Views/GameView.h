#pragma once
#include "View/View.h"
#include "Core/Scene/Scene.h"
#include "Core/ProjectLoader.h"

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
//   gameView.Init(device, w, h, srvCpu, srvGpu, &scene, projectSettings);
//   gameView.Render(cmdList, mainRtv, drawFn);   // inherited from View
//   gameView.DrawPanel();                        // overridden here
// ---------------------------------------------------------------------------
class GameView : public View
{
public:
    GameView()  = default;
    ~GameView() = default;

    // Calls View::Init then stores the scene pointer and aspect ratio settings.
    // scene must outlive this view.
    // device: opaque renderer device handle (cast internally to ID3D12Device*)
    // srvCpu/srvGpu: opaque descriptor handles (cast internally from void*)
    void Init(void* device,
              uint32_t width, uint32_t height,
              void* srvCpu,
              void* srvGpu,
              uint32_t srvSlotIndex,
              Scene* scene,
              const ProjectSettings& settings);

    // Issues the game-camera scene draw into cmd each frame.
    // cmd: opaque graphics command list handle (cast internally to ID3D12GraphicsCommandList*)
    void Render3D(void* cmd) override;

    // Defines the Game panel showing the game-camera render output.
    void DrawPanel(IEditorUi& ui) override;

private:
    Scene* m_scene = nullptr; // non-owning

    // Aspect ratio settings
    ProjectSettings::AspectRatioMode m_aspectRatioMode = ProjectSettings::AspectRatioMode::Locked;
    float m_gameAspectRatio = 1.777f;  // 16:9
    uint32_t m_gameWindowWidth = 1920;
    uint32_t m_gameWindowHeight = 1080;
    glm::vec4 m_letterboxColor{0.f, 0.f, 0.f, 1.f};

    // Computed viewport aspect ratio
    float m_aspect = 1.0f;
};
