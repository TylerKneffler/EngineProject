#pragma once

#include <cstdint>
#include <functional>

namespace Engine::Scene { class Scene; }

namespace Engine::Rendering
{
// Defines the sampling order used by deterministic offline exports. The first
// call prepares the scene exactly as Start() left it (t = 0); subsequent
// samples may advance deterministic fixed simulation ticks independently of
// output-frame boundaries.
class ExportFrameSequence final
{
public:
    explicit ExportFrameSequence(Engine::Scene::Scene& scene)
        : m_scene(scene) {}

    void PrepareNextFrame();
    void PrepareCurrentFrame();
    void AdvanceFixedSteps(uint32_t stepCount);
    bool AdvanceFixedSteps(uint32_t stepCount,
        const std::function<bool(double, double)>& beforeStep);
    uint64_t GetPreparedFrameCount() const { return m_preparedFrameCount; }

private:
    Engine::Scene::Scene& m_scene;
    uint64_t m_preparedFrameCount = 0;
};
}
