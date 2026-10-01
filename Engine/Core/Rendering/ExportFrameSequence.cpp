#include "ExportFrameSequence.h"

#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"

#include <stdexcept>

namespace Engine::Rendering
{
void ExportFrameSequence::PrepareNextFrame()
{
    if (!m_scene.GetClock().IsFixedStep())
        throw std::logic_error(
            "ExportFrameSequence requires a fixed scene time step");

    if (m_preparedFrameCount > 0u)
        AdvanceFixedSteps(1u);
    PrepareCurrentFrame();
}

void ExportFrameSequence::PrepareCurrentFrame()
{
    if (!m_scene.GetClock().IsFixedStep())
        throw std::logic_error(
            "ExportFrameSequence requires a fixed scene time step");
    m_scene.PrepareRenderFrame();
    ++m_preparedFrameCount;
}

void ExportFrameSequence::AdvanceFixedSteps(uint32_t stepCount)
{
    AdvanceFixedSteps(stepCount, {});
}

bool ExportFrameSequence::AdvanceFixedSteps(uint32_t stepCount,
    const std::function<bool(double, double)>& beforeStep)
{
    if (!m_scene.GetClock().IsFixedStep())
        throw std::logic_error(
            "ExportFrameSequence requires a fixed scene time step");
    for (uint32_t step = 0; step < stepCount; ++step)
    {
        const double stepStartTime = m_scene.GetElapsedTime();
        const double stepDuration = m_scene.GetClock().GetFixedStep();
        if (beforeStep && !beforeStep(stepStartTime, stepDuration))
            return false;
        m_scene.UpdateFixedFrame();
        Engine::Core::SceneManager::ProcessPendingSceneLoad();
    }
    return true;
}
}
