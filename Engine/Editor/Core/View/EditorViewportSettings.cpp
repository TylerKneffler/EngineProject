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
bool showBones = true;
}

void EditorViewportSettings::RegisterSettingsHandler()
{
    if (ImGui::FindSettingsHandler("SceneView")) return;
    showBones = true;
    ImGuiSettingsHandler handler;
    handler.TypeName = "SceneView";
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.UserData = &showBones;
    handler.ClearAllFn = handler.ReadInitFn = [](ImGuiContext*, ImGuiSettingsHandler* settings)
    { *static_cast<bool*>(settings->UserData) = true; };
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* settings, const char* name) -> void*
    { return std::strcmp(name, "Preferences") == 0 ? settings->UserData : nullptr; };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
    {
        int value = -1;
        if (sscanf_s(line, "ShowBones=%d", &value) == 1 && (value == 0 || value == 1))
            *static_cast<bool*>(entry) = value != 0;
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* settings, ImGuiTextBuffer* output)
    {
        output->appendf("[SceneView][Preferences]\nShowBones=%d\n\n",
            *static_cast<bool*>(settings->UserData) ? 1 : 0);
    };
    ImGui::AddSettingsHandler(&handler);
}

bool EditorViewportSettings::AreBonesVisible() { return showBones; }

void EditorViewportSettings::SetBonesVisible(bool visible)
{
    if (showBones == visible) return;
    showBones = visible;
    if (ImGui::GetCurrentContext()) ImGui::MarkIniSettingsDirty();
}

void EditorViewportSettings::Apply(Engine::Scene::Scene& scene)
{
    scene.SetEditorBonesVisible(showBones);
}
}
