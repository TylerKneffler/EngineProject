#include "GameView.h"

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void GameView::Init(ID3D12Device* device,
                    uint32_t width, uint32_t height,
                    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
                    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu,
                    Scene* scene)
{
    View::Init(device, width, height, srvCpu, srvGpu);
    m_scene = scene;
}

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void GameView::DrawPanel()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.f, 0.f });

    if (ImGui::Begin("Game"))
    {
        ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.x > 1.f && available.y > 1.f)
        {
            ImGui::Image(
                static_cast<ImTextureID>(m_srvGpu.ptr),
                available);
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
}
