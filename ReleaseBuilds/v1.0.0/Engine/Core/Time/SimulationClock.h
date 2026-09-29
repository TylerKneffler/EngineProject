#pragma once

#include <cstdint>

namespace Engine::Time
{
class SimulationClock
{
public:
    void Reset();
    void SetFixedStep(double seconds);
    void UseVariableStep();
    float Advance(double variableDeltaSeconds = 0.0);

    bool IsFixedStep() const { return m_fixedStep; }
    double GetFixedStep() const { return m_fixedDeltaSeconds; }
    float GetDeltaTime() const { return m_deltaSeconds; }
    double GetElapsedTime() const { return m_elapsedSeconds; }
    uint64_t GetFrameIndex() const { return m_frameIndex; }

private:
    bool m_fixedStep = false;
    double m_fixedDeltaSeconds = 1.0 / 60.0;
    float m_deltaSeconds = 0.f;
    double m_elapsedSeconds = 0.0;
    uint64_t m_frameIndex = 0;
};
}
