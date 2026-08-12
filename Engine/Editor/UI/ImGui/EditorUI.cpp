#include "pch.h"
#include "Engine/Editor/UI/ImGui/EditorUI.h"
#include "Engine/Editor/EditorState.h"
#include "imgui.h"

EditorUI::EditorUI(EditorState* state)
    : m_state(state)
{
    OutputDebugStringA("[EditorUI] Constructor called\n");
}

void EditorUI::Render(PlayState playState)
{
    if (!m_state) return;
    m_dockspace.Draw();
    m_mainMenu.Draw(*m_state, playState, m_gameBuildManager);
    m_panelHost.Draw(*m_state);
    if (m_state->IsLoadingOverlayVisible())
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowBgAlpha(0.86f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
        ImGui::Begin("##scriptReloadLoading", nullptr, flags);
        const char* message = m_state->GetLoadingOverlayMessage().empty()
            ? "Compiling scripts..." : m_state->GetLoadingOverlayMessage().c_str();
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        ImGui::SetCursorPos({
            (ImGui::GetWindowSize().x - textSize.x) * 0.5f,
            (ImGui::GetWindowSize().y - textSize.y) * 0.5f });
        ImGui::TextUnformatted(message);
        ImGui::End();
    }
}
