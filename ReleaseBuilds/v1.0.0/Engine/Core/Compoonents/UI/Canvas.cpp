#include "Core/Compoonents/UI/Canvas.h"
#include <algorithm>

namespace Engine::Components
{
Canvas::Canvas()
{
    SetTypeName(COMPONENT_TYPE_NAME(Canvas));
    singlecomponent = true;
    RegisterField("referenceResolution", referenceResolution);
    RegisterField("scaleMode", scaleMode);
    RegisterField("matchWidthOrHeight", matchWidthOrHeight);
    RegisterField("sortingOrder", sortingOrder);
}

glm::vec2 Canvas::GetLogicalSize(float viewportAspect) const
{
    const float width = std::max(1.f, referenceResolution.x);
    const float height = std::max(1.f, referenceResolution.y);
    if (scaleMode == "Constant") return { width, height };
    const float aspect = std::max(0.01f, viewportAspect);
    const glm::vec2 matchHeight(height * aspect, height);
    const glm::vec2 matchWidth(width, width / aspect);
    return glm::mix(matchHeight, matchWidth,
        std::clamp(matchWidthOrHeight, 0.f, 1.f));
}
}
