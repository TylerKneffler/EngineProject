#include "GameView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"

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
void GameView::DrawPanel(IEditorUi& ui)
{
    if (ui.BeginWindow(m_title.c_str(), &m_open, true))
    {
        const float targetAspect = m_aspectRatioMode == ProjectSettings::AspectRatioMode::Free ? 0.f :
            (m_aspectRatioMode == ProjectSettings::AspectRatioMode::Locked ? m_gameAspectRatio :
             static_cast<float>(m_gameWindowWidth) / static_cast<float>(m_gameWindowHeight));
        const auto input = ui.Viewport(GetUiTextureHandle(), targetAspect,
            {m_letterboxColor.r,m_letterboxColor.g,m_letterboxColor.b,m_letterboxColor.a});
        EditorUiVec2 available = input.available;
        if (available.x > 1.f && available.y > 1.f)
        {
            EditorUiVec2 viewportSize = available, viewportPos{};

            // Update the aspect ratio used for the projection matrix.
            m_aspect = viewportSize.x / viewportSize.y;

            // Draw letterbox/pillarbox background if the viewport doesn't fill the panel.
            if (viewportSize.x < available.x || viewportSize.y < available.y)
            {
            }

        }
    }
    ui.EndWindow();
}

// ---------------------------------------------------------------------------
// Render3D
// ---------------------------------------------------------------------------
void GameView::Render3D(void* cmd)
{
    if (m_scene)
    {
        auto* factory = m_scene->GetGraphicsProvider()->GetContextFactory();
        factory->SetCommandBuffer(cmd);
        auto ctx = factory->CreateContext();
        m_scene->Render(ctx.get(), m_aspect, m_scene->FindGameCamera());
    }
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
            outViewportSize = availableSize;
            break;
        }

        case ProjectSettings::AspectRatioMode::Locked:
        {
            float availableAspect = availableSize.x / availableSize.y;
            if (availableAspect > lockedAspect)
            {
                outViewportSize.y = availableSize.y;
                outViewportSize.x = availableSize.y * lockedAspect;
                outViewportPos.x = (availableSize.x - outViewportSize.x) * 0.5f;
            }
            else
            {
                outViewportSize.x = availableSize.x;
                outViewportSize.y = availableSize.x / lockedAspect;
                outViewportPos.y = (availableSize.y - outViewportSize.y) * 0.5f;
            }
            break;
        }

        case ProjectSettings::AspectRatioMode::Hardcoded:
        {
            float gameAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
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
