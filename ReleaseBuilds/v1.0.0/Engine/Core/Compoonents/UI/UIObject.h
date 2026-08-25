#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include "Core/Model/UIData.h"
#include <glm/glm.hpp>
#include <string>

namespace Engine::Components
{
// RectTransform-style screen layout with an optional flex row/column for its
// immediate UIObject children. Normalized anchors use top-left as (0, 0).
class UIObject final : public Engine::Core::Component
{
public:
    using UIRect = Engine::Model::UIRect;

    UIObject();

    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    glm::vec3 anchorMin { 0.f, 0.f, 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    glm::vec3 anchorMax { 0.f, 0.f, 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    glm::vec3 pivot { 0.5f, 0.5f, 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    glm::vec3 anchoredPosition { 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    glm::vec3 sizeDelta { 300.f, 80.f, 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    float minWidth = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    float minHeight = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    float maxWidth = 100000.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Rect")
    float maxHeight = 100000.f;

    PROPERTY(Inspector, EditAnywhere, Category = "UI | Spacing")
    float marginLeft = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Spacing")
    float marginTop = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Spacing")
    float marginRight = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Spacing")
    float marginBottom = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Spacing")
    float paddingLeft = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Spacing")
    float paddingTop = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Spacing")
    float paddingRight = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Spacing")
    float paddingBottom = 0.f;

    PROPERTY(Inspector, EditAnywhere, Category = "UI | Layout")
    std::string layoutDirection = "None"; // None, Row, Column
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Layout")
    std::string justifyContent = "Start"; // Start, Center, End, SpaceBetween
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Layout")
    std::string alignItems = "Start"; // Start, Center, End, Stretch
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Layout")
    float spacing = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Layout")
    float flexGrow = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Layout")
    bool clipChildren = false;
    PROPERTY(Inspector, EditAnywhere, Category = "UI")
    bool visible = true;
    PROPERTY(Inspector, EditAnywhere, Category = "UI")
    int zOrder = 0;

    const UIRect& GetComputedRect() const { return m_computedRect; }
    const UIRect& GetComputedClipRect() const { return m_computedClipRect; }
    void SetComputedLayout(const UIRect& rect, const UIRect& clip)
    { m_computedRect = rect; m_computedClipRect = clip; }

private:
    UIRect m_computedRect{};
    UIRect m_computedClipRect{};
};
}
