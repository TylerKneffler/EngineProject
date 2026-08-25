#pragma once


namespace Engine::Scene { class Scene; }

namespace Engine::Audio
{
// Per-scene audio coordination. The process-wide output device and buses remain
// owned by AudioMixer; this class selects one listener and manages scene reset.
class Audio
{
public:
    explicit Audio(Engine::Scene::Scene& scene);

    void Update(float deltaTime);
    void Reset();

private:
    Engine::Scene::Scene* m_scene = nullptr;
};
}
