#pragma once

#include "Core/Compoonents/UI/UIObject.h"
#include <glm/glm.hpp>
#include <vector>

class Canvas;
class Object;
class Scene;
class UIText;

struct UITextLayout
{
    Canvas* canvas = nullptr;
    UIObject* layout = nullptr;
    UIText* text = nullptr;
    glm::vec2 canvasSize { 1.f };
    int sortKey = 0;
};

class UILayout
{
public:
    // Resolves every enabled Canvas hierarchy and returns visible text items in
    // deterministic canvas/z/hierarchy order.
    static std::vector<UITextLayout> Resolve(Scene& scene, float viewportAspect);
};
