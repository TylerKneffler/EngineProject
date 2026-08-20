#include "Core/Compoonents/UI/UIButton.h"

#include <utility>

namespace Engine::Components
{
UIButton::UIButton()
{
    SetTypeName(COMPONENT_TYPE_NAME(UIButton));
    singlecomponent = true;
    RegisterField("interactable", interactable);
    RegisterField("normalColor", normalColor);
    RegisterField("hoverColor", hoverColor);
    RegisterField("pressedColor", pressedColor);
    RegisterField("disabledColor", disabledColor);
    RegisterField("alpha", alpha);
}

void UIButton::UpdateInteraction(bool hovered, bool mouseDown)
{
    const bool wasHovered = m_hovered;
    const bool wasPressed = m_pressed;
    const bool hadPendingClick = m_clicked;

    if (!interactable)
    {
        if (m_pressed)
            Notify(m_onReleased);
        if (m_hovered)
            Notify(m_onHoverExit);
        m_hovered = false;
        m_pressed = false;
        m_clicked = false;
        m_pointerHeld = false;
        m_wasMouseDown = mouseDown;
        return;
    }

    m_hovered = hovered;

    if (mouseDown && !m_wasMouseDown && hovered)
        m_pointerHeld = true;

    if (!mouseDown && m_wasMouseDown)
    {
        if (m_pointerHeld && hovered)
            m_clicked = true;
        m_pointerHeld = false;
    }

    if (m_pointerHeld && !hovered && !mouseDown)
        m_pointerHeld = false;

    m_pressed = m_pointerHeld && mouseDown;
    m_wasMouseDown = mouseDown;

    if (!wasHovered && m_hovered)
        Notify(m_onHoverEnter);
    if (wasHovered && !m_hovered)
        Notify(m_onHoverExit);
    if (!wasPressed && m_pressed)
        Notify(m_onPressed);
    if (wasPressed && !m_pressed)
        Notify(m_onReleased);
    if (!hadPendingClick && m_clicked)
        Notify(m_onClick);
}

bool UIButton::ConsumeClick()
{
    const bool clicked = m_clicked;
    m_clicked = false;
    return clicked;
}

void UIButton::AddOnClickListener(ButtonEvent listener)
{
    if (listener)
        m_onClick.push_back(std::move(listener));
}

void UIButton::AddOnHoverEnterListener(ButtonEvent listener)
{
    if (listener)
        m_onHoverEnter.push_back(std::move(listener));
}

void UIButton::AddOnHoverExitListener(ButtonEvent listener)
{
    if (listener)
        m_onHoverExit.push_back(std::move(listener));
}

void UIButton::AddOnPressedListener(ButtonEvent listener)
{
    if (listener)
        m_onPressed.push_back(std::move(listener));
}

void UIButton::AddOnReleasedListener(ButtonEvent listener)
{
    if (listener)
        m_onReleased.push_back(std::move(listener));
}

void UIButton::ClearListeners()
{
    m_onClick.clear();
    m_onHoverEnter.clear();
    m_onHoverExit.clear();
    m_onPressed.clear();
    m_onReleased.clear();
}

void UIButton::Notify(const std::vector<ButtonEvent>& listeners)
{
    for (const ButtonEvent& listener : listeners)
        listener(*this);
}
}
