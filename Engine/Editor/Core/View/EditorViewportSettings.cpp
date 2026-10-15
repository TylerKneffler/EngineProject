#include "EditorViewportSettings.h"
#include "Core/Scene/Scene.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdio>
#include <cstring>

namespace Engine::Editor
{
namespace
{
struct Preferences
{
    bool bones = true;
    bool colliders = false;
    bool meshNormals = false;
    bool bounds = false;
    bool lightVolumes = false;
    bool cameraFrustums = false;
};
Preferences preferences;

void SetPreference(bool& preference, bool visible)
{
    if (preference == visible) return;
    preference = visible;
    if (ImGui::GetCurrentContext()) ImGui::MarkIniSettingsDirty();
}
}

void EditorViewportSettings::RegisterSettingsHandler()
{
    if (ImGui::FindSettingsHandler("SceneView")) return;
    preferences = {};
    ImGuiSettingsHandler handler;
    handler.TypeName = "SceneView";
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.UserData = &preferences;
    handler.ClearAllFn = handler.ReadInitFn = [](ImGuiContext*, ImGuiSettingsHandler* settings)
    { *static_cast<Preferences*>(settings->UserData) = {}; };
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* settings, const char* name) -> void*
    { return std::strcmp(name, "Preferences") == 0 ? settings->UserData : nullptr; };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
    {
        int value = -1;
        auto& prefs = *static_cast<Preferences*>(entry);
        bool* target = nullptr;
        if (sscanf_s(line, "ShowBones=%d", &value) == 1) target = &prefs.bones;
        else if (sscanf_s(line, "ShowColliders=%d", &value) == 1) target = &prefs.colliders;
        else if (sscanf_s(line, "ShowMeshNormals=%d", &value) == 1) target = &prefs.meshNormals;
        else if (sscanf_s(line, "ShowBounds=%d", &value) == 1) target = &prefs.bounds;
        else if (sscanf_s(line, "ShowLightVolumes=%d", &value) == 1) target = &prefs.lightVolumes;
        else if (sscanf_s(line, "ShowCameraFrustums=%d", &value) == 1) target = &prefs.cameraFrustums;
        if (target && (value == 0 || value == 1)) *target = value != 0;
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* settings, ImGuiTextBuffer* output)
    {
        const auto& prefs = *static_cast<Preferences*>(settings->UserData);
        output->appendf("[SceneView][Preferences]\nShowBones=%d\nShowColliders=%d\n"
            "ShowMeshNormals=%d\nShowBounds=%d\nShowLightVolumes=%d\n"
            "ShowCameraFrustums=%d\n\n", prefs.bones, prefs.colliders,
            prefs.meshNormals, prefs.bounds, prefs.lightVolumes, prefs.cameraFrustums);
    };
    ImGui::AddSettingsHandler(&handler);
}

bool EditorViewportSettings::AreBonesVisible() { return preferences.bones; }

void EditorViewportSettings::SetBonesVisible(bool visible)
{
    SetPreference(preferences.bones, visible);
}

bool EditorViewportSettings::AreCollidersVisible() { return preferences.colliders; }
void EditorViewportSettings::SetCollidersVisible(bool value) { SetPreference(preferences.colliders, value); }
bool EditorViewportSettings::AreMeshNormalsVisible() { return preferences.meshNormals; }
void EditorViewportSettings::SetMeshNormalsVisible(bool value) { SetPreference(preferences.meshNormals, value); }
bool EditorViewportSettings::AreBoundsVisible() { return preferences.bounds; }
void EditorViewportSettings::SetBoundsVisible(bool value) { SetPreference(preferences.bounds, value); }
bool EditorViewportSettings::AreLightVolumesVisible() { return preferences.lightVolumes; }
void EditorViewportSettings::SetLightVolumesVisible(bool value) { SetPreference(preferences.lightVolumes, value); }
bool EditorViewportSettings::AreCameraFrustumsVisible() { return preferences.cameraFrustums; }
void EditorViewportSettings::SetCameraFrustumsVisible(bool value) { SetPreference(preferences.cameraFrustums, value); }

void EditorViewportSettings::Apply(Engine::Scene::Scene& scene)
{
    scene.SetEditorBonesVisible(preferences.bones);
}
}
