#pragma once

#include <glm/glm.hpp>

namespace Engine::Components { class Canvas; class UIObject; class UIText; }

namespace Engine::Model
{
struct UIRect
{
    float x = 0.f;
    float y = 0.f;
    float width = 0.f;
    float height = 0.f;
};

struct UITextLayout
{
    Engine::Components::Canvas* canvas = nullptr;
    Engine::Components::UIObject* layout = nullptr;
    Engine::Components::UIText* text = nullptr;
    glm::vec2 canvasSize { 1.f };
    int sortKey = 0;
};
}
