#pragma once

#include <algorithm>
#include <atomic>

namespace Engine::Editor::Paint
{
inline std::atomic<float> penPressure{ 1.f };

inline float PressureScale(float response)
{
    const float pressure = std::clamp(penPressure.load(), 0.f, 1.f);
    return 1.f + (pressure - 1.f) * std::clamp(response, 0.f, 1.f);
}
}
