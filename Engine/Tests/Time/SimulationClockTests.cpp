#include "Core/Time/SimulationClock.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        Engine::Time::SimulationClock first;
        Engine::Time::SimulationClock second;
        first.SetFixedStep(1.0 / 24.0);
        second.SetFixedStep(1.0 / 24.0);
        for (uint64_t frame = 1; frame <= 24000; ++frame)
        {
            const float firstDelta = first.Advance(19.0);
            const float secondDelta = second.Advance(0.00001);
            Require(firstDelta == secondDelta,
                "Fixed clocks accepted caller-dependent deltas");
            Require(first.GetElapsedTime() == second.GetElapsedTime(),
                "Identical fixed clocks diverged");
            Require(first.GetElapsedTime() ==
                static_cast<double>(frame) * first.GetFixedStep(),
                "Fixed elapsed time accumulated instead of deriving from its frame index");
        }
        Require(first.GetFrameIndex() == 24000,
            "Fixed clock reported the wrong frame index");
        first.Reset();
        Require(first.IsFixedStep() && first.GetFrameIndex() == 0 &&
            first.GetElapsedTime() == 0.0,
            "Reset did not preserve fixed configuration and clear timeline state");
        first.UseVariableStep();
        Require(first.Advance(0.25) == 0.25f &&
            std::abs(first.GetElapsedTime() - 0.25) < 1e-12,
            "Variable clock did not accept its supplied delta");
        std::puts("Simulation clock determinism tests passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Simulation clock test failure: %s\n", error.what());
        return 1;
    }
}
