#pragma once

namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
// Session-independent viewport preferences saved alongside the editor layout.
class EditorViewportSettings
{
public:
    static void RegisterSettingsHandler();
    static bool AreBonesVisible();
    static void SetBonesVisible(bool visible);
    static void Apply(Engine::Scene::Scene& scene);
};
}
