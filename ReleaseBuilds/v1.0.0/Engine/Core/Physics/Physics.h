#pragma once


namespace Engine::Scene { class Scene; }

namespace Engine::Physics
{
class Physics
{
public:
    explicit Physics(Engine::Scene::Scene& scene);
    ~Physics();

    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    void Step(float deltaTime);
    void Reset();

    // Internal bridge for physics components; keeps Bullet types out of the
    // engine-facing header.
    void* GetInternalState();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
}
