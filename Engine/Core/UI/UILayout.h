#pragma once

#include "Core/Model/UIData.h"
#include <glm/glm.hpp>
#include <vector>

class Object;
class Scene;

class UILayout
{
public:
    // Resolves every enabled Canvas hierarchy and returns visible text items in
    // deterministic canvas/z/hierarchy order.
    static std::vector<UITextLayout> Resolve(Scene& scene, float viewportAspect);
};
