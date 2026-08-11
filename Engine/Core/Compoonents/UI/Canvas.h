#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

// Root of a screen-space retained UI hierarchy. Child Objects with UIObject
// components are laid out inside this logical resolution.
class Canvas final : public Component
{
public:
    Canvas();

    PROPERTY(Inspector, EditAnywhere, Category = "Canvas")
    glm::vec3 referenceResolution { 1920.f, 1080.f, 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Canvas")
    std::string scaleMode = "ScaleWithScreen"; // ScaleWithScreen, Constant
    PROPERTY(Inspector, EditAnywhere, Category = "Canvas", Range = "0, 1")
    float matchWidthOrHeight = 0.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Canvas")
    int sortingOrder = 0;

    glm::vec2 GetLogicalSize(float viewportAspect) const;
};
