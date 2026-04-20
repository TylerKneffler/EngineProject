
#include "SceneView.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Renderers/DX12/DX12GraphicsContext.h"
#include "Core/Renderers/DX12/D3D12View.h"

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
void SceneView::DrawPanel()
{
    // Remove inner padding so the texture fills the panel edge-to-edge.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::Begin(m_title.c_str(), &m_open);

    float panDX = 0.f, panDY = 0.f;
    float orbitDX = 0.f, orbitDY = 0.f;
    float zoom = 0.f;

    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x > 0.f && size.y > 0.f)
    {
        // Update computed aspect ratio
        m_aspect = size.x / size.y;

        // Calculate game viewport with aspect ratio constraints
        ImVec2 viewportSize, viewportPos;
        CalculateGameViewport(size, viewportSize, viewportPos);

        // Draw letterbox/pillarbox background (if needed)
        if (viewportSize.x < size.x || viewportSize.y < size.y)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec2 panelPos = ImGui::GetCursorScreenPos();
            
            // Draw background in letterbox color
            ImU32 bgColor = ImGui::GetColorU32(ImVec4(
                m_letterboxColor.r, m_letterboxColor.g, 
                m_letterboxColor.b, m_letterboxColor.a
            ));
            drawList->AddRectFilled(
                panelPos,
                ImVec2(panelPos.x + size.x, panelPos.y + size.y),
                bgColor
            );
        }

        // Position cursor at viewport location and draw the game texture
        ImGui::SetCursorPos(viewportPos);
        ImGui::Image((ImTextureID)(uintptr_t)GetD3D12View()->GetSrvGpu().ptr, viewportSize);

        // Check for mouse input on the viewport
        if (ImGui::IsItemHovered())
        {
            ImVec2 d = ImGui::GetIO().MouseDelta;

            // Right-button drag → pan.
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                panDX = d.x;
                panDY = d.y;
            }

            // Middle-button drag → orbit.
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            {
                orbitDX = d.x;
                orbitDY = d.y;
            }

            // Scroll wheel → zoom.
            zoom = ImGui::GetIO().MouseWheel;
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();

    ApplyCameraControls(panDX, panDY, orbitDX, orbitDY, zoom);
}

// ---------------------------------------------------------------------------
// CalculateGameViewport
// ---------------------------------------------------------------------------
void SceneView::CalculateGameViewport(ImVec2 availableSize, ImVec2& outViewportSize, ImVec2& outViewportPos)
{
    outViewportPos = ImVec2(0.f, 0.f);

    switch (m_aspectRatioMode)
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

            if (availableAspect > m_gameAspectRatio)
            {
                // Panel is wider than game aspect: pillarbox (vertical bars)
                outViewportSize.y = availableSize.y;
                outViewportSize.x = availableSize.y * m_gameAspectRatio;
                outViewportPos.x = (availableSize.x - outViewportSize.x) * 0.5f;
            }
            else
            {
                // Panel is narrower than game aspect: letterbox (horizontal bars)
                outViewportSize.x = availableSize.x;
                outViewportSize.y = availableSize.x / m_gameAspectRatio;
                outViewportPos.y = (availableSize.y - outViewportSize.y) * 0.5f;
            }
            break;
        }

        case ProjectSettings::AspectRatioMode::Hardcoded:
        {
            // Hardcoded size: fit to exact window size, scale to fit
            float gameAspect = (float)m_gameWindowWidth / (float)m_gameWindowHeight;
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

    using namespace DirectX;

    Camera*    cam = m_scene->editorCamera.GetComponent<Camera>();
    assert(cam && "Scene editorCamera must have a Camera component");
    glm::vec3& pos = m_scene->editorCamera.transform.position;

    XMVECTOR eyeV    = XMVectorSet(pos.x, pos.y, pos.z, 0.f);
    XMVECTOR targetV = XMLoadFloat3(&cam->target);
    XMVECTOR upV     = XMLoadFloat3(&cam->up);
    XMVECTOR fwdV    = XMVector3Normalize(XMVectorSubtract(targetV, eyeV));
    XMVECTOR rightV  = XMVector3Normalize(XMVector3Cross(upV, fwdV));
    XMVECTOR realUpV = XMVector3Normalize(XMVector3Cross(fwdV, rightV));

    // Pan — move camera and target together along the view plane,
    //        opposite the drag direction (grab-the-world feel).
    if (panDX != 0.f || panDY != 0.f)
    {
        float dist  = XMVectorGetX(XMVector3Length(XMVectorSubtract(targetV, eyeV)));
        float speed = dist * 0.002f;
        XMVECTOR pan = XMVectorAdd(
            XMVectorScale(rightV,   -panDX * speed),
            XMVectorScale(realUpV,   panDY * speed));
        XMFLOAT3 panF;
        XMStoreFloat3(&panF, pan);
        pos.x += panF.x; pos.y += panF.y; pos.z += panF.z;
        cam->target.x += panF.x;
        cam->target.y += panF.y;
        cam->target.z += panF.z;
        eyeV    = XMVectorSet(pos.x, pos.y, pos.z, 0.f);  // refresh for orbit
        targetV = XMLoadFloat3(&cam->target);
    }

    // Orbit — rotate camera around target.
    //         Horizontal drag yaws around world Y; vertical drag pitches around right.
    if (orbitDX != 0.f || orbitDY != 0.f)
    {
        constexpr float kSensitivity = 0.005f;
        XMVECTOR arm = XMVectorSubtract(eyeV, targetV);
        arm = XMVector3Transform(arm, XMMatrixRotationY(orbitDX * kSensitivity));
        arm = XMVector3Transform(arm, XMMatrixRotationAxis(rightV, orbitDY * kSensitivity));
        XMFLOAT3 newEye;
        XMStoreFloat3(&newEye, XMVectorAdd(targetV, arm));
        pos.x = newEye.x; pos.y = newEye.y; pos.z = newEye.z;
        eyeV = XMVectorSet(pos.x, pos.y, pos.z, 0.f);  // refresh for zoom
    }

    // Zoom — move eye along the forward vector, scaled by distance so the
    //         step shrinks as the camera approaches the target (Unity/Blender feel).
    if (zoom != 0.f)
    {
        constexpr float kZoomFactor  = 0.1f;
        constexpr float kMinDistance = 0.05f;
        float dist   = XMVectorGetX(XMVector3Length(XMVectorSubtract(targetV, eyeV)));
        float step   = zoom * dist * kZoomFactor;
        // Clamp so we never overshoot the target or go below the minimum distance.
        step = std::min(step, dist - kMinDistance);
        XMVECTOR move = XMVectorScale(fwdV, step);
        XMFLOAT3 moveF;
        XMStoreFloat3(&moveF, move);
        pos.x += moveF.x; pos.y += moveF.y; pos.z += moveF.z;
    }
}

// ---------------------------------------------------------------------------
// Render3D
// ---------------------------------------------------------------------------
void SceneView::Render3D(void* cmd)
{
    if (m_scene)
    {
        D3D12GraphicsContext graphicsContext(static_cast<ID3D12GraphicsCommandList*>(cmd));
        m_scene->Render(&graphicsContext, m_aspect);
    }
}
