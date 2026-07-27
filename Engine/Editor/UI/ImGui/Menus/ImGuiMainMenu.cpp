#include "pch.h"
#include "Engine/Editor/UI/ImGui/Menus/ImGuiMainMenu.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/Core/View/ViewFactory.h"
#include "imgui.h"

namespace
{
bool IsBusy(PlayState state)
{
    return state == PlayState::Building ||
           state == PlayState::Playing ||
           state == PlayState::Paused;
}
}

void ImGuiMainMenu::Draw(EditorState& state, PlayState playState,
    GameBuildManager* buildManager) const
{
    if (!ImGui::BeginMainMenuBar()) return;
    DrawFileMenu(state, playState, buildManager);
    DrawViewsMenu(state);
    DrawPlayControls(playState, buildManager);
    ImGui::EndMainMenuBar();
}

void ImGuiMainMenu::DrawFileMenu(EditorState& state, PlayState playState,
    GameBuildManager* buildManager) const
{
    if (!ImGui::BeginMenu("File")) return;
    if (ImGui::MenuItem("Save All", "Ctrl+S"))
        state.SaveScene();
    ImGui::Separator();

    const bool busy = IsBusy(playState);
    if (busy) ImGui::BeginDisabled();
    if (ImGui::MenuItem("Build", "Ctrl+B") && buildManager)
        buildManager->StartBuild(PostBuildAction::Nothing);
    if (ImGui::MenuItem("Build and Run in Editor") && buildManager)
        buildManager->StartBuild(PostBuildAction::PlayInEditor);
    if (ImGui::MenuItem("Build and Run Standalone") && buildManager)
        buildManager->StartBuild(PostBuildAction::LaunchStandalone);
    if (busy) ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::MenuItem("Project Preferences"))
        state.SetShowPreferences(true);
    ImGui::Separator();
    if (ImGui::MenuItem("Exit"))
        PostQuitMessage(0);
    ImGui::EndMenu();
}

void ImGuiMainMenu::DrawViewsMenu(EditorState& state) const
{
    if (!ImGui::BeginMenu("Views")) return;
    ViewFactory* factory = state.GetViewFactory();
    const bool no3D = !factory || !factory->CanCreate3DView();
    if (no3D) ImGui::BeginDisabled();
    if (ImGui::MenuItem("Scene")) OpenPanel(state, "Scene");
    if (ImGui::MenuItem("Game")) OpenPanel(state, "Game");
    if (no3D) ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::MenuItem("Hierarchy")) OpenPanel(state, "Hierarchy");
    if (ImGui::MenuItem("Properties")) OpenPanel(state, "Properties");
    if (ImGui::MenuItem("Assets")) OpenPanel(state, "Assets");
    if (ImGui::MenuItem("Console")) OpenPanel(state, "Console");
    ImGui::EndMenu();
}

void ImGuiMainMenu::DrawPlayControls(
    PlayState playState, GameBuildManager* buildManager) const
{
    constexpr float buttonWidth = 60.f;
    constexpr float doubleButtonWidth = buttonWidth * 2.f + 4.f;
    const float barWidth = ImGui::GetWindowWidth();

    if (playState == PlayState::Stopped || playState == PlayState::BuildFailed)
    {
        ImGui::SetCursorPosX((barWidth - buttonWidth) * 0.5f);
        if (ImGui::Button("Play", {buttonWidth, 0.f}) && buildManager)
            buildManager->StartBuild(PostBuildAction::PlayInEditor);
        if (playState == PlayState::BuildFailed)
        {
            ImGui::SameLine();
            ImGui::TextColored({1.f, 0.3f, 0.3f, 1.f}, "Build failed");
        }
    }
    else if (playState == PlayState::Building)
    {
        static const char* spinner[] = {"|", "/", "-", "\\"};
        const int frame = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
        ImGui::SetCursorPosX((barWidth - 120.f) * 0.5f);
        ImGui::Text("Building... %s", spinner[frame]);
    }
    else if (playState == PlayState::Playing)
    {
        ImGui::SetCursorPosX((barWidth - doubleButtonWidth) * 0.5f);
        if (ImGui::Button("Pause", {buttonWidth, 0.f}) && buildManager)
            buildManager->Pause();
        ImGui::SameLine(0.f, 4.f);
        if (ImGui::Button("Stop", {buttonWidth, 0.f}) && buildManager)
            buildManager->Stop();
    }
    else if (playState == PlayState::Paused)
    {
        ImGui::SetCursorPosX((barWidth - doubleButtonWidth) * 0.5f);
        if (ImGui::Button("Resume", {buttonWidth, 0.f}) && buildManager)
            buildManager->Resume();
        ImGui::SameLine(0.f, 4.f);
        if (ImGui::Button("Stop", {buttonWidth, 0.f}) && buildManager)
            buildManager->Stop();
    }
}

void ImGuiMainMenu::OpenPanel(EditorState& state, const char* type) const
{
    ViewFactory* factory = state.GetViewFactory();
    if (!factory) return;
    auto panel = factory->Create(type);
    if (panel) state.GetPanels().push_back(std::move(panel));
}
