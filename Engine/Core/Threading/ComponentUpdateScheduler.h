#pragma once
#include "Core/component.h"
#include <vector>

namespace Engine::Threading
{
// A segment contains only opt-in components. The caller joins it before any
// legacy callback or other owner-thread scene work runs.
class ComponentUpdateScheduler
{
public:
    struct Node
    {
        Core::Component* component;
        Core::Component::UpdateAccess access;
    };
    static void Run(const std::vector<Node>& nodes);
};
}
