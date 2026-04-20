#include "GameView.h"
#include "../../../../Core/Renderers/DX12/D3D12View.h"
#include "Core/Renderers/DX12/DX12GraphicsContext.h"

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void GameView::Init(void* device,
                    uint32_t width, uint32_t height,
                    void* srvCpu,
                    void* srvGpu,
                    uint32_t srvSlotIndex,
                    Scene* scene,
                    const ProjectSettings& settings)
{
    View::Init(device, width, height, srvCpu, srvGpu, srvSlotIndex);
    m_scene = scene;
    m_aspectRatioMode  = settings.aspectRatioMode;
    m_gameAspectRatio  = settings.gameAspectRatio;
    m_gameWindowWidth  = settings.gameWindowWidth;
    m_gameWindowHeight = settings.gameWindowHeight;
    m_letterboxColor   = settings.letterboxColor;
}

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void GameView::DrawPanel()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });

    if (ImGui::Begin(m_title.c_str(), &m_open))
    {
        ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.x > 1.f && available.y > 1.f)
        {
            ImVec2 viewportSize, viewportPos;
            CalculateGameViewport(available, viewportSize, viewportPos);

            // Update the aspect ratio used for the projection matrix.
            m_aspect = viewportSize.x / viewportSize.y;

            // Draw letterbox/pillarbox background if the viewport doesn't fill the panel.
            if (viewportSize.x < available.x || viewportSize.y < available.y)
            {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2 panelPos = ImGui::GetCursorScreenPos();
                ImU32 bgColor = ImGui::GetColorU32(ImVec4(
                    m_letterboxColor.r, m_letterboxColor.g,
                    m_letterboxColor.b, m_letterboxColor.a));
                drawList->AddRectFilled(
                    panelPos,
                    ImVec2(panelPos.x + available.x, panelPos.y + available.y),
                    bgColor);
            }

            ImGui::SetCursorPos(viewportPos);
            ImGui::Image(
                static_cast<ImTextureID>(GetD3D12View()->GetSrvGpu().ptr),
                viewportSize);
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Render3D
// ---------------------------------------------------------------------------
void GameView::Render3D(void* cmd)
{
    if (m_scene)
    {
        D3D12GraphicsContext graphicsContext(static_cast<ID3D12GraphicsCommandList*>(cmd));
        m_scene->Render(&graphicsContext, m_aspect, m_scene->FindGameCamera());
    }
}

// ---------------------------------------------------------------------------
// CalculateGameViewport
// ---------------------------------------------------------------------------
void GameView::CalculateGameViewport(ImVec2 availableSize, ImVec2& outViewportSize, ImVec2& outViewportPos)
{
    outViewportPos = ImVec2(0.f, 0.f);

    switch (m_aspectRatioMode)
    {
        case ProjectSettings::AspectRatioMode::Free:
        {
            outViewportSize = availableSize;
            break;
        }

        case ProjectSettings::AspectRatioMode::Locked:
        {
            float availableAspect = availableSize.x / availableSize.y;
            if (availableAspect > m_gameAspectRatio)
            {
                outViewportSize.y = availableSize.y;
                outViewportSize.x = availableSize.y * m_gameAspectRatio;
                outViewportPos.x = (availableSize.x - outViewportSize.x) * 0.5f;
            }
            else
            {
                outViewportSize.x = availableSize.x;
                outViewportSize.y = availableSize.x / m_gameAspectRatio;
                outViewportPos.y = (availableSize.y - outViewportSize.y) * 0.5f;
            }
            break;
        }

        case ProjectSettings::AspectRatioMode::Hardcoded:
        {
            float gameAspect = (float)m_gameWindowWidth / (float)m_gameWindowHeight;
            float availableAspect = availableSize.x / availableSize.y;
            if (availableAspect > gameAspect)
            {
                outViewportSize.y = availableSize.y;
                outViewportSize.x = availableSize.y * gameAspect;
                outViewportPos.x = (availableSize.x - outViewportSize.x) * 0.5f;
            }
            else
            {
                outViewportSize.x = availableSize.x;
                outViewportSize.y = availableSize.x / gameAspect;
                outViewportPos.y = (availableSize.y - outViewportSize.y) * 0.5f;
            }
            break;
        }
    }
}
