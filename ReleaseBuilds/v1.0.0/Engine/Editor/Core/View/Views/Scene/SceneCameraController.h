#pragma once

namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
class SceneCameraController
{
public:
    static void Apply(Engine::Scene::Scene& scene,
        float panDX, float panDY,
        float orbitDX, float orbitDY,
        float zoom, float dolly);
};
}
