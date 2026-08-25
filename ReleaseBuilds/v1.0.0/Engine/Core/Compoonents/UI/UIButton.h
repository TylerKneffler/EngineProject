#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <functional>
#include <vector>

namespace Engine::Components
{
class UIButton final : public Engine::Core::Component
{
public:
    using ButtonEvent = std::function<void(UIButton&)>;

    UIButton();

    PROPERTY(Inspector, EditAnywhere, Category = "UI | Button")
    bool interactable = true;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Button")
    glm::vec3 normalColor { 0.18f, 0.48f, 0.90f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Button")
    glm::vec3 hoverColor { 0.26f, 0.56f, 0.98f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Button")
    glm::vec3 pressedColor { 0.12f, 0.34f, 0.70f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Button")
    glm::vec3 disabledColor { 0.45f, 0.45f, 0.45f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Button", Range = "0, 1")
    float alpha = 1.f;

    void UpdateInteraction(bool hovered, bool mouseDown);
    bool IsHovered() const { return m_hovered; }
    bool IsPressed() const { return m_pressed; }
    bool ConsumeClick();

    void AddOnClickListener(ButtonEvent listener);
    void AddOnHoverEnterListener(ButtonEvent listener);
    void AddOnHoverExitListener(ButtonEvent listener);
    void AddOnPressedListener(ButtonEvent listener);
    void AddOnReleasedListener(ButtonEvent listener);
    void ClearListeners();

private:
    void Notify(const std::vector<ButtonEvent>& listeners);

    bool m_hovered = false;
    bool m_pressed = false;
    bool m_clicked = false;
    bool m_pointerHeld = false;
    bool m_wasMouseDown = false;

    std::vector<ButtonEvent> m_onClick;
    std::vector<ButtonEvent> m_onHoverEnter;
    std::vector<ButtonEvent> m_onHoverExit;
    std::vector<ButtonEvent> m_onPressed;
    std::vector<ButtonEvent> m_onReleased;
};
}
