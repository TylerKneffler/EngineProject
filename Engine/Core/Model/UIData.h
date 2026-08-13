#pragma once

#include <glm/glm.hpp>

class Canvas;
class UIObject;
class UIText;

struct UIRect
{
    float x = 0.f;
    float y = 0.f;
    float width = 0.f;
    float height = 0.f;
};

struct UITextLayout
{
    Canvas* canvas = nullptr;
    UIObject* layout = nullptr;
    UIText* text = nullptr;
    glm::vec2 canvasSize { 1.f };
    int sortKey = 0;
};
