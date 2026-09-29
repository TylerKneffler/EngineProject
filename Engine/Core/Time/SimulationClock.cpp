#include "pch.h"
#include "SimulationClock.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Engine::Time
{
void SimulationClock::Reset()
{
    m_deltaSeconds = 0.f;
    m_elapsedSeconds = 0.0;
    m_frameIndex = 0;
}

void SimulationClock::SetFixedStep(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
        throw std::invalid_argument("A fixed simulation step must be finite and positive");
    m_fixedDeltaSeconds = seconds;
    m_fixedStep = true;
}

void SimulationClock::UseVariableStep()
{
    m_fixedStep = false;
}

float SimulationClock::Advance(double variableDeltaSeconds)
{
    const double delta = m_fixedStep
        ? m_fixedDeltaSeconds
        : std::max(0.0, std::isfinite(variableDeltaSeconds)
            ? variableDeltaSeconds : 0.0);
    ++m_frameIndex;
    // Derive fixed time from the integer frame index so long exports do not
    // accumulate a different rounding error based on frame history.
    m_elapsedSeconds = m_fixedStep
        ? static_cast<double>(m_frameIndex) * m_fixedDeltaSeconds
        : m_elapsedSeconds + delta;
    m_deltaSeconds = static_cast<float>(delta);
    return m_deltaSeconds;
}
}
