
#include "SceneView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <glm/ext/matrix_transform.hpp>

// ---------------------------------------------------------------------------
// Init — store the scene pointer, then delegate resource creation to View.
// ---------------------------------------------------------------------------
void SceneView::Init(void* device,
                         uint32_t width, uint32_t height,
                         void* srvCpu,
                         void* srvGpu,
                         uint32_t srvSlotIndex,
                         Scene* scene,
                         const ProjectSettings& settings)
{
    m_scene = scene;
    m_aspectRatioMode = settings.aspectRatioMode;
    m_gameAspectRatio = settings.gameAspectRatio;
    m_gameWindowWidth = settings.gameWindowWidth;
    m_gameWindowHeight = settings.gameWindowHeight;
    m_letterboxColor = settings.letterboxColor;
    
    View::Init(device, width, height, srvCpu, srvGpu, srvSlotIndex);
}

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void SceneView::DrawPanel(IEditorUi& ui)
{
    // Remove inner padding so the texture fills the panel edge-to-edge.
    ui.BeginWindow(m_title.c_str(), &m_open, true);

    float panDX = 0.f, panDY = 0.f;
    float orbitDX = 0.f, orbitDY = 0.f;
    float zoom = 0.f;

    const float targetAspect = m_aspectRatioMode == ProjectSettings::AspectRatioMode::Free ? 0.f :
        (m_aspectRatioMode == ProjectSettings::AspectRatioMode::Locked ? m_gameAspectRatio :
         static_cast<float>(m_gameWindowWidth) / static_cast<float>(m_gameWindowHeight));
    const auto input = ui.Viewport(GetUiTextureHandle(), targetAspect,
        {m_letterboxColor.r,m_letterboxColor.g,m_letterboxColor.b,m_letterboxColor.a});
    EditorUiVec2 size = input.available;
    if (size.x > 0.f && size.y > 0.f)
    {
        // Update computed aspect ratio
        m_aspect = size.x / size.y;

        // Calculate game viewport with aspect ratio constraints
        EditorUiVec2 viewportSize = size, viewportPos{};

        // Draw letterbox/pillarbox background (if needed)
        if (viewportSize.x < size.x || viewportSize.y < size.y)
        {
            // Draw background in letterbox color
        }

        // Position cursor at viewport location and draw the game texture

        // Check for mouse input on the viewport
        if (input.hovered)
        {
            EditorUiVec2 d = input.mouseDelta;

            // Right-button drag → pan.
            if (input.rightDown)
            {
                panDX = d.x;
                panDY = d.y;
            }

            // Middle-button drag → orbit.
            if (input.middleDown)
            {
                orbitDX = d.x;
                orbitDY = d.y;
            }

            // Scroll wheel → zoom.
            zoom = input.mouseWheel;
        }
    }

    ui.EndWindow();

    ApplyCameraControls(panDX, panDY, orbitDX, orbitDY, zoom);
}

// ---------------------------------------------------------------------------
// CalculateGameViewport
// ---------------------------------------------------------------------------
static void CalculateGameViewport(EditorUiVec2 availableSize, EditorUiVec2& outViewportSize, EditorUiVec2& outViewportPos,
    ProjectSettings::AspectRatioMode mode, float lockedAspect, uint32_t windowWidth, uint32_t windowHeight)
{
    outViewportPos = {0.f, 0.f};

    switch (mode)
    {
        case ProjectSettings::AspectRatioMode::Free:
        {
            // Free aspect: use full available size
            outViewportSize = availableSize;
            break;
        }

        case ProjectSettings::AspectRatioMode::Locked:
        {
            // Locked aspect ratio: fit to aspect while maintaining ratio
            float availableAspect = availableSize.x / availableSize.y;

            if (availableAspect > lockedAspect)
            {
                // Panel is wider than game aspect: pillarbox (vertical bars)
                outViewportSize.y = availableSize.y;
                outViewportSize.x = availableSize.y * lockedAspect;
                outViewportPos.x = (availableSize.x - outViewportSize.x) * 0.5f;
            }
            else
            {
                // Panel is narrower than game aspect: letterbox (horizontal bars)
                outViewportSize.x = availableSize.x;
                outViewportSize.y = availableSize.x / lockedAspect;
                outViewportPos.y = (availableSize.y - outViewportSize.y) * 0.5f;
            }
            break;
        }

        case ProjectSettings::AspectRatioMode::Hardcoded:
        {
            // Hardcoded size: fit to exact window size, scale to fit
            float gameAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
            float availableAspect = availableSize.x / availableSize.y;

            if (availableAspect > gameAspect)
            {
                // Panel is wider: pillarbox
                outViewportSize.y = availableSize.y;
                outViewportSize.x = availableSize.y * gameAspect;
                outViewportPos.x = (availableSize.x - outViewportSize.x) * 0.5f;
            }
            else
            {
                // Panel is narrower: letterbox
                outViewportSize.x = availableSize.x;
                outViewportSize.y = availableSize.x / gameAspect;
                outViewportPos.y = (availableSize.y - outViewportSize.y) * 0.5f;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// ApplyCameraControls
// ---------------------------------------------------------------------------
void SceneView::ApplyCameraControls(float panDX, float panDY,
                                        float orbitDX, float orbitDY,
                                        float zoom)
{
    if (!m_scene) return;
    if (panDX == 0.f && panDY == 0.f && orbitDX == 0.f && orbitDY == 0.f && zoom == 0.f) return;

    Camera*    cam = m_scene->editorCamera.GetComponent<Camera>();
    assert(cam && "Scene editorCamera must have a Camera component");
    glm::vec3& pos = m_scene->editorCamera.transform.position;

    glm::vec3 eye     = pos;
    glm::vec3 target  = cam->target;
    glm::vec3 forward = glm::normalize(target - eye);
    const glm::vec3 right  = glm::normalize(glm::cross(cam->up, forward));
    const glm::vec3 realUp = glm::normalize(glm::cross(forward, right));

    // Pan — move camera and target together along the view plane,
    //        opposite the drag direction (grab-the-world feel).
    if (panDX != 0.f || panDY != 0.f)
    {
        const float dist = glm::length(target - eye);
        float speed = dist * 0.002f;
        const glm::vec3 pan = right * (-panDX * speed) + realUp * (panDY * speed);
        pos += pan;
        cam->target += pan;
        eye = pos;
        target = cam->target;
    }

    // Orbit — rotate camera around target.
    //         Horizontal drag yaws around world Y; vertical drag pitches around right.
    if (orbitDX != 0.f || orbitDY != 0.f)
    {
        constexpr float kSensitivity = 0.005f;
        glm::vec3 arm = eye - target;
        arm = glm::mat3(glm::rotate(glm::mat4(1.f), orbitDX * kSensitivity,
                                    glm::vec3(0.f, 1.f, 0.f))) * arm;
        arm = glm::mat3(glm::rotate(glm::mat4(1.f), orbitDY * kSensitivity, right)) * arm;
        pos = target + arm;
        eye = pos;
    }

    // Zoom — move eye along the forward vector, scaled by distance so the
    //         step shrinks as the camera approaches the target (Unity/Blender feel).
    if (zoom != 0.f)
    {
        constexpr float kZoomFactor  = 0.1f;
        constexpr float kMinDistance = 0.05f;
        const float dist = glm::length(target - eye);
        float step   = zoom * dist * kZoomFactor;
        // Clamp so we never overshoot the target or go below the minimum distance.
        step = std::min(step, dist - kMinDistance);
        forward = glm::normalize(target - eye);
        pos += forward * step;
    }
}

// ---------------------------------------------------------------------------
// Render3D
// ---------------------------------------------------------------------------
void SceneView::Render3D(void* cmd)
{
    if (m_scene)
    {
        auto* factory = m_scene->GetGraphicsProvider()->GetContextFactory();
        factory->SetCommandBuffer(cmd);
        auto ctx = factory->CreateContext();
        m_scene->Render(ctx.get(), m_aspect);
    }
}
