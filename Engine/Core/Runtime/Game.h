#pragma once

#include "Core/Time/SimulationClock.h"
#include <cstdint>

namespace Engine::Scene { class Scene; }

namespace Engine::Runtime
{
// The running simulation over a scene. Drives the lifecycle (Start, per-frame
// Update with physics/audio/pose phases, fixed steps) and owns the simulation
// clock. The scene stays the owner of object memory; Object and Component keep
// their lifecycle methods and the Scene -> Object -> Component cascade.
//
// One Game is used by the standalone game, the server, the video exporter and
// Editor Play mode, so all of them run the same loop. Editor edit mode creates
// no Game, so gameplay callbacks never run there.
//
// The Game binds itself to the scene for its lifetime (Scene::GetGame()), and
// must not outlive the scene.
class Game
{
public:
    using Scene = Engine::Scene::Scene;

    struct UpdateTimings
    {
        double objectsMs = 0.0;
        double poseMs = 0.0;
        double physicsMs = 0.0;
        double postPhysicsMs = 0.0;
        double constraintsMs = 0.0, audioMs = 0.0;
    };

    explicit Game(Scene& scene);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    Scene& GetScene() { return m_scene; }
    const Scene& GetScene() const { return m_scene; }

    // ---- Lifecycle ------------------------------------------------------------
    // Resets the clock and starts every object (Scene -> Object -> Component).
    void Start();
    bool HasStarted() const;
    // One simulation frame: component updates, spatial scripts, transforms,
    // pose constraints + audio, physics, post-physics, then commit.
    void Update(float deltaTime);
    // Advances exactly one configured fixed frame. Offline exporters use this
    // instead of supplying or sampling wall-clock deltas.
    void UpdateFixedFrame();
    // Manual render loops advance the shared streaming/upload frame budget
    // without running component and physics updates.
    void AdvanceManualFrame(float deltaTime);

    // ---- Clock ----------------------------------------------------------------
    void SetFixedTimeStep(double seconds);
    void UseVariableTimeStep();
    const Engine::Time::SimulationClock& GetClock() const { return m_clock; }
    float GetDeltaTime() const { return m_deltaTime; }
    uint64_t GetFrameIndex() const { return m_clock.GetFrameIndex(); }

    // ---- Diagnostics ------------------------------------------------------------
    // Timings are recorded while Scene::SetUpdateProfilingEnabled(true).
    const UpdateTimings& GetLastUpdateTimings() const { return m_lastUpdateTimings; }

private:
    Scene& m_scene;
    Engine::Time::SimulationClock m_clock;
    float m_deltaTime = 0.f;
    UpdateTimings m_lastUpdateTimings{};
};
}
