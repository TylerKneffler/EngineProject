#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

namespace Engine::Components
{
class UIText final : public Engine::Core::Component
{
public:
    UIText();

    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text")
    std::string text = "Text";
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text")
    std::string fontPath;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text", ClampMin = "1")
    float fontSize = 32.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text")
    glm::vec3 color { 1.f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text", Range = "0, 1")
    float alpha = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text")
    std::string horizontalAlignment = "Left"; // Left, Center, Right
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text")
    std::string verticalAlignment = "Top"; // Top, Center, Bottom
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text")
    bool wordWrap = true;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text")
    std::string overflow = "Clip"; // Visible, Clip, Ellipsis
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Text", ClampMin = "0.1")
    float lineSpacing = 1.f;
};
}
