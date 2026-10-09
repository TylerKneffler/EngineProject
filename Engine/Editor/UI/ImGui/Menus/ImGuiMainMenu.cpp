#include "pch.h"
#include "Engine/Editor/UI/ImGui/Menus/ImGuiMainMenu.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/Input/EditorKeyBindings.h"
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
        const std::string shortcut = EditorKeyBindings::Get().ShortcutLabel(
            EditorCommand::ImportAsset);
        if (ImGui::MenuItem("Asset...", shortcut.c_str()))
            state.ImportAsset();
        ImGui::Separator();
        ImGui::TextDisabled("Or drag files into the editor");
        ImGui::EndMenu();
    }
    DrawViewsMenu(state);
    DrawToolbarMenu(state);
    DrawRenderingMenu(state, playState);
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
        const std::string bakeShortcut = EditorKeyBindings::Get().ShortcutLabel(
            EditorCommand::BakeLighting);
        const std::string clearShortcut = EditorKeyBindings::Get().ShortcutLabel(
            EditorCommand::ClearBakedLighting);
        if (ImGui::MenuItem("Bake Lighting", bakeShortcut.c_str()))
            state.BakeLighting();
        if (ImGui::MenuItem("Clear Baked Lighting", clearShortcut.c_str()))
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
    EditorKeyBindings& keybinds = EditorKeyBindings::Get();
    const std::string undoShortcut = keybinds.ShortcutLabel(EditorCommand::Undo);
    const std::string redoShortcut = keybinds.ShortcutLabel(EditorCommand::Redo);
    const std::string saveShortcut = keybinds.ShortcutLabel(EditorCommand::SaveScene);
    const std::string saveAllShortcut = keybinds.ShortcutLabel(EditorCommand::SaveAll);
    const std::string buildShortcut = keybinds.ShortcutLabel(EditorCommand::Build);
    const std::string buildPlayShortcut = keybinds.ShortcutLabel(EditorCommand::BuildAndPlay);
    const std::string buildStandaloneShortcut = keybinds.ShortcutLabel(EditorCommand::BuildStandalone);
    if (ImGui::MenuItem("Undo", undoShortcut.c_str(), false, state.CanUndo()))
        state.Undo();
    if (ImGui::MenuItem("Redo", redoShortcut.c_str(), false, state.CanRedo()))
        state.Redo();
    if (busy) ImGui::EndDisabled();
    ImGui::Separator();
    if (busy) ImGui::BeginDisabled();
    if (ImGui::MenuItem("Save", saveShortcut.c_str()))
        state.SaveScene();
    if (ImGui::MenuItem("Save All", saveAllShortcut.c_str()))
        state.SaveAll();
    if (busy) ImGui::EndDisabled();
    if (state.IsEditingPrefab() && ImGui::MenuItem("Close Prefab Stage"))
        state.ClosePrefabStage();
    ImGui::Separator();

    if (busy) ImGui::BeginDisabled();
    if (ImGui::MenuItem("Build", buildShortcut.c_str()) && buildManager)
        buildManager->StartBuild(PostBuildAction::Nothing);
    if (ImGui::MenuItem("Build and Run in Editor", buildPlayShortcut.c_str()) && buildManager)
        buildManager->StartBuild(PostBuildAction::PlayInEditor);
    if (ImGui::MenuItem("Build and Run Standalone", buildStandaloneShortcut.c_str()) && buildManager)
        buildManager->StartBuild(PostBuildAction::LaunchStandalone);
    if (busy) ImGui::EndDisabled();

    ImGui::Separator();
    const std::string preferencesShortcut = keybinds.ShortcutLabel(
        EditorCommand::Preferences);
    if (ImGui::MenuItem("Project Preferences", preferencesShortcut.c_str()))
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
    if (ImGui::MenuItem("Animation Editor", nullptr, false,
        state.CanOpenAnimationForSelection()))
        state.OpenAnimationForSelection();
    if (no3D) ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::MenuItem("Hierarchy")) OpenPanel(state, "Hierarchy");
    if (ImGui::MenuItem("Properties")) OpenPanel(state, "Properties");
    if (ImGui::MenuItem("Assets")) OpenPanel(state, "Assets");
    if (ImGui::MenuItem("Console")) OpenPanel(state, "Console");
    if (ImGui::MenuItem("Problems")) OpenPanel(state, "Problems");
    if (ImGui::MenuItem("Terminal")) OpenPanel(state, "Terminal");
    if (ImGui::MenuItem("Performance")) OpenPanel(state, "Performance");
    ImGui::EndMenu();
}

void ImGuiMainMenu::DrawToolbarMenu(EditorState& state) const
{
    if (!ImGui::BeginMenu("Toolbar")) return;
    EditorState::ToolbarVisibility& visible = state.GetToolbarVisibility();
    ImGui::MenuItem("Play Controls", nullptr, &visible.playControls);
    ImGui::MenuItem("Edit Mode", nullptr, &visible.editMode);
    ImGui::MenuItem("Save", nullptr, &visible.save);
    ImGui::MenuItem("Undo / Redo", nullptr, &visible.undoRedo);
    ImGui::MenuItem("Mode Tools", nullptr, &visible.modeTools);
    ImGui::MenuItem("Transform Tools", nullptr, &visible.transformTools);
    ImGui::MenuItem("Scene Display", nullptr, &visible.sceneDisplay);
    ImGui::MenuItem("Prefab Actions", nullptr, &visible.prefabActions);
    ImGui::Separator();
    ImGui::MenuItem("Edit Tools", nullptr, &visible.toolDetails);
    ImGui::EndMenu();
}

void ImGuiMainMenu::OpenPanel(EditorState& state, const char* type) const
{
    ViewFactory* factory = state.GetViewFactory();
    if (!factory) return;
    auto panel = factory->Create(type);
    if (panel) state.GetPanels().push_back(std::move(panel));
}
}
