#include "pch.h"
#include "Engine/Editor/UI/ImGui/Menus/ImGuiMainMenu.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/Core/View/ViewFactory.h"
#include "imgui.h"

namespace Engine::Editor
{
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
    if (ImGui::BeginMenu("Import"))
    {
        if (ImGui::MenuItem("Asset..."))
            state.ImportAsset();
        ImGui::Separator();
        ImGui::TextDisabled("Or drag files into the editor");
        ImGui::EndMenu();
    }
    DrawViewsMenu(state);
    DrawRenderingMenu(state, playState);
    DrawPlayControls(playState, buildManager);
    ImGui::EndMainMenuBar();
}

void ImGuiMainMenu::DrawRenderingMenu(
    EditorState& state, PlayState playState) const
{
    if (!ImGui::BeginMenu("Rendering")) return;
    if (ImGui::BeginMenu("Lighting"))
    {
        Engine::Scene::Scene* scene = state.GetScene();
        const bool disabled = !scene || IsBusy(playState);
        if (disabled) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Bake Lighting"))
            state.BakeLighting();
        if (ImGui::MenuItem("Clear Baked Lighting"))
            state.ClearBakedLighting();
        if (disabled) ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void ImGuiMainMenu::DrawFileMenu(EditorState& state, PlayState playState,
    GameBuildManager* buildManager) const
{
    if (!ImGui::BeginMenu("File")) return;
    const bool busy = IsBusy(playState);
    if (busy) ImGui::BeginDisabled();
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, state.CanUndo()))
        state.Undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, state.CanRedo()))
        state.Redo();
    if (busy) ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem("Save", "Ctrl+S"))
        state.SaveScene();
    if (ImGui::MenuItem("Save All", "Ctrl+A+S"))
        state.SaveAll();
    if (state.IsEditingPrefab() && ImGui::MenuItem("Close Prefab Stage"))
        state.ClosePrefabStage();
    ImGui::Separator();

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
    if (ImGui::MenuItem("Problems")) OpenPanel(state, "Problems");
    if (ImGui::MenuItem("Terminal")) OpenPanel(state, "Terminal");
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
            buildManager->PlayInEditor();
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
}
