#include "Editor/Core/View/EditorViewportSettings.h"
#include "Core/Scene/Scene.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void NewSession()
{
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // Test writes only its own settings file.
    Engine::Editor::EditorViewportSettings::RegisterSettingsHandler();
}

int main()
{
    try
    {
        using Engine::Editor::EditorViewportSettings;
        const std::filesystem::path path = ".local/test-output/viewport-settings/editor.ini";
        std::filesystem::create_directories(path.parent_path());
        NewSession();
        Engine::Scene::Scene first;
        const auto sceneBefore = first.SaveToString();
        Check(EditorViewportSettings::AreBonesVisible(), "Legacy default is incorrect");
        Check(!EditorViewportSettings::AreCollidersVisible() &&
            !EditorViewportSettings::AreMeshNormalsVisible() &&
            !EditorViewportSettings::AreBoundsVisible(),
            "Diagnostic overlays should start hidden");
        EditorViewportSettings::SetBonesVisible(false);
        EditorViewportSettings::SetCollidersVisible(true);
        EditorViewportSettings::SetMeshNormalsVisible(true);
        EditorViewportSettings::SetBoundsVisible(true);
        EditorViewportSettings::SetLightVolumesVisible(true);
        EditorViewportSettings::SetCameraFrustumsVisible(true);
        EditorViewportSettings::Apply(first);
        Check(!first.AreEditorBonesVisible(), "Toggle did not update the view");
        Check(ImGui::GetCurrentContext()->SettingsDirtyTimer > 0.f,
            "Toggle did not schedule settings persistence");
        Check(first.SaveToString() == sceneBefore, "Visibility modified saved scene data");
        ImGui::SaveIniSettingsToDisk(path.string().c_str());
        ImGui::DestroyContext();

        NewSession();
        Check(EditorViewportSettings::AreBonesVisible(), "New session did not reset defaults");
        Check(!EditorViewportSettings::AreCollidersVisible() &&
            !EditorViewportSettings::AreMeshNormalsVisible(),
            "New session did not reset diagnostic overlays");
        ImGui::LoadIniSettingsFromDisk(path.string().c_str());
        Engine::Scene::Scene restored, newlyOpened;
        EditorViewportSettings::Apply(restored);
        EditorViewportSettings::Apply(newlyOpened);
        Check(!restored.AreEditorBonesVisible() && !newlyOpened.AreEditorBonesVisible(),
            "Hidden bones did not survive restart or apply to a new view");
        Check(EditorViewportSettings::AreCollidersVisible() &&
            EditorViewportSettings::AreMeshNormalsVisible() &&
            EditorViewportSettings::AreBoundsVisible() &&
            EditorViewportSettings::AreLightVolumesVisible() &&
            EditorViewportSettings::AreCameraFrustumsVisible(),
            "Diagnostic overlays did not survive restart");
        EditorViewportSettings::SetBonesVisible(true);
        ImGui::SaveIniSettingsToDisk(path.string().c_str());
        ImGui::DestroyContext();

        NewSession();
        ImGui::LoadIniSettingsFromDisk(path.string().c_str());
        EditorViewportSettings::Apply(restored);
        Check(restored.AreEditorBonesVisible(), "Visible bones did not survive restart");
        ImGui::LoadIniSettingsFromMemory("[SceneView][Preferences]\nShowBones=2\n\n");
        Check(EditorViewportSettings::AreBonesVisible(), "Invalid preference did not retain the default");
        ImGui::LoadIniSettingsFromMemory("[SceneView][Preferences]\nShowColliders=2\n\n");
        Check(!EditorViewportSettings::AreCollidersVisible(),
            "Invalid collider preference did not retain the default");
        ImGui::LoadIniSettingsFromMemory("[Toolbar][Main]\nFill=1,1\n\n");
        Check(EditorViewportSettings::AreBonesVisible(), "Old editor settings did not retain the default");
        ImGui::DestroyContext();
        std::filesystem::remove(path);
        std::cout << "Viewport visuals persist across editor sessions and leave scene data intact.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
