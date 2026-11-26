#include "pch.h"
#include "Engine/Editor/UI/ImGui/Menus/ImGuiMainMenu.h"
#include "Engine/Editor/State/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/Input/EditorKeyBindings.h"
#include "Engine/Editor/Core/View/ViewFactory.h"
#include "Engine/Editor/Core/View/Views/ConsoleView.h"
#include "Core/Components/Materials/Texture.h"
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
    PollAssetGenerators(state);
    if (m_terrainGenerator) m_terrainGenerator->Draw(state);
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
    if (ImGui::MenuItem("Code Editor"))
        state.OpenCodeEditor();
    DrawToolbarMenu(state);
    DrawRenderingMenu(state, playState);
    DrawToolsMenu(state);
    ImGui::EndMainMenuBar();
    if (m_openNewAnimationPopup)
    {
        ImGui::OpenPopup("New Animation");
        m_openNewAnimationPopup = false;
    }
    if (ImGui::BeginPopupModal("New Animation", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Create an animation for the selected rig.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool submitted = ImGui::InputTextWithHint("##AnimationName",
            "Animation name (optional)", m_newAnimationName,
            sizeof(m_newAnimationName),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (submitted || ImGui::Button("Create"))
        {
            if (state.CreateAnimationClip(m_newAnimationName))
                ImGui::CloseCurrentPopup();
            else
                m_newAnimationError = state.CanCreateAnimationClip()
                    ? "An animation with this name already exists."
                    : "Open an animation rig first.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        if (!m_newAnimationError.empty())
            ImGui::TextDisabled("%s", m_newAnimationError.c_str());
        ImGui::EndPopup();
    }
}

void ImGuiMainMenu::DrawToolsMenu(EditorState& state) const
{
    if (!ImGui::BeginMenu("Tools")) return;
    if (ImGui::MenuItem("Terrain generator..."))
    {
        if (!m_terrainGenerator) m_terrainGenerator = std::make_unique<TerrainGeneratorWindow>();
        m_terrainGenerator->Open(state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Regenerate the open terrain's map-scale base heights (size, relief, seed, erosion)");
    ImGui::Separator();
    if (!m_generators) m_generators = std::make_unique<AssetGeneratorRunner>();
    auto* console = state.GetConsole();
    const auto start = [&](const AssetGeneratorRunner::Generator& generator)
    {
        std::string error;
        if (m_generators->IsRunning()) m_generatorQueue.push_back(generator);
        else if (m_generators->Start(generator, &error))
        {
            if (console) console->AddLog(ConsoleView::Level::Build,
                "[Generators] " + generator.label + " started");
        }
        else if (console) console->AddLog(ConsoleView::Level::Error, "[Generators] " + error);
    };
    if (ImGui::BeginMenu("Asset generators"))
    {
        const auto& generators = m_generators->Generators();
        if (generators.empty())
            ImGui::TextDisabled("No tools/generate_*.py scripts found");
        for (const auto& generator : generators)
        {
            if (ImGui::MenuItem(generator.label.c_str())) start(generator);
            if (!generator.summary.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s (%s)", generator.summary.c_str(),
                    generator.script.filename().string().c_str());
        }
        ImGui::Separator();
        // Order matters: clusters are baked from the conifers.
        if (ImGui::MenuItem("Run all woodland generators"))
            for (const char* name : { "generate_conifers", "generate_boulders",
                     "generate_forest_clusters", "generate_sky_sprites" })
                for (const auto& generator : generators)
                    if (generator.script.stem() == name) start(generator);
        if (ImGui::MenuItem("Rescan tools folder")) m_generators->Rescan();
        ImGui::EndMenu();
    }
    if (m_generators->IsRunning())
    {
        ImGui::Separator();
        ImGui::TextDisabled("Running: %s (%.0f s)%s", m_generators->RunningLabel().c_str(),
            m_generators->RunningSeconds(), m_generatorQueue.empty() ? "" : ", more queued");
    }
    ImGui::EndMenu();
}

void ImGuiMainMenu::PollAssetGenerators(EditorState& state) const
{
    if (!m_generators) return;
    auto* console = state.GetConsole();
    for (const auto& line : m_generators->TakeNewOutput())
        if (console && !line.empty()) console->AddLog(ConsoleView::Level::Build, "  " + line);
    AssetGeneratorRunner::Result result;
    if (!m_generators->Poll(result)) return;
    for (const auto& line : m_generators->TakeNewOutput())
        if (console && !line.empty()) console->AddLog(ConsoleView::Level::Build, "  " + line);
    // Reload what the run rewrote: cached textures by path, then the open
    // scenes (meshes, materials and prefabs re-resolve on refresh). The
    // asset browser lists new files through its own directory watch.
    size_t textures = 0;
    std::error_code error;
    for (const auto& path : result.changedFiles)
    {
        std::string extension = path.extension().string();
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".ktx2" && extension != ".png" && extension != ".jpg" &&
            extension != ".dds" && extension != ".tga") continue;
        Engine::Components::Texture::Invalidate(path.string());
        const auto relative = std::filesystem::relative(path, m_generators->AssetsRoot(), error);
        if (!error) Engine::Components::Texture::Invalidate("Assets/" + relative.generic_string());
        ++textures;
    }
    if (result.exitCode == 0 && !result.changedFiles.empty())
        if (auto* views = state.GetViewFactory(); views && views->OnAssetContentsChanged)
            views->OnAssetContentsChanged(m_generators->AssetsRoot().string());
    if (console)
    {
        char summary[256];
        std::snprintf(summary, sizeof(summary), "[Generators] %s %s in %.0f s: %zu asset files written (%zu textures reloaded)",
            result.label.c_str(), result.exitCode == 0 ? "finished" : "FAILED", result.seconds,
            result.changedFiles.size(), textures);
        console->AddLog(result.exitCode == 0 ? ConsoleView::Level::Build : ConsoleView::Level::Error, summary);
    }
    if (!m_generatorQueue.empty())
    {
        auto next = m_generatorQueue.front();
        m_generatorQueue.pop_front();
        std::string startError;
        if (result.exitCode != 0)
        {
            if (console) console->AddLog(ConsoleView::Level::Warning,
                "[Generators] Queue stopped after a failure.");
            m_generatorQueue.clear();
        }
        else if (m_generators->Start(next, &startError))
        {
            if (console) console->AddLog(ConsoleView::Level::Build, "[Generators] " + next.label + " started");
        }
        else if (console) console->AddLog(ConsoleView::Level::Error, "[Generators] " + startError);
    }
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
    if (ImGui::BeginMenu("New"))
    {
        if (ImGui::MenuItem("Animation...", nullptr, false,
                !busy && state.CanCreateAnimationClip()))
        {
            m_newAnimationName[0] = '\0';
            m_newAnimationError.clear();
            m_openNewAnimationPopup = true;
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
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
    if (ImGui::MenuItem("Animation Editor"))
        state.OpenAnimationEditor();
    if (no3D) ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::MenuItem("Hierarchy")) OpenPanel(state, "Hierarchy");
    if (ImGui::MenuItem("Properties")) OpenPanel(state, "Properties");
    if (ImGui::MenuItem("Assets")) OpenPanel(state, "Assets");
    if (ImGui::MenuItem("Console")) OpenPanel(state, "Console");
    if (ImGui::MenuItem("Animation Timeline")) OpenPanel(state, "Animation Timeline");
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
    ImGui::MenuItem("Paint Tools", nullptr, &visible.paintTools);
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
