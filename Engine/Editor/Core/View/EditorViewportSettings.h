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
    static bool AreCollidersVisible();
    static void SetCollidersVisible(bool visible);
    static bool AreMeshNormalsVisible();
    static void SetMeshNormalsVisible(bool visible);
    static bool AreBoundsVisible();
    static void SetBoundsVisible(bool visible);
    static bool AreLightVolumesVisible();
    static void SetLightVolumesVisible(bool visible);
    static bool AreCameraFrustumsVisible();
    static void SetCameraFrustumsVisible(bool visible);
    static void Apply(Engine::Scene::Scene& scene);
};
}
