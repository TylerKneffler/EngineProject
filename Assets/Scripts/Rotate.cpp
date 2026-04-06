#include "Scripts/Rotate.h"
#include "Core/Object.h"
#include <glm/gtc/matrix_transform.hpp>

void Rotate::Start()
{
    // Nothing to initialise — axis and speed are set directly.
}

void Rotate::Update()
{
    if (!Owner) return;
    // Accumulate rotation in Euler radians stored on the Transform.
    // dt is not yet threaded through Update(); using a fixed 60 fps step
    // for now (swap out kFixedDt once Update(float dt) is added).
    constexpr float kFixedDt = 1.f / 60.f;
    Owner->transform.rotation += axis * glm::radians(speed) * kFixedDt;
}
