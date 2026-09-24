#include "Scripts/Demos/Snake/SnakeUI.h"

#include "Core/Compoonents/UI/UIObject.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>

SnakeUI::SnakeUI()
{
    SetTypeName(COMPONENT_TYPE_NAME(SnakeUI));
    RegisterField("scoreTextName", scoreTextName);
    RegisterField("statusTextName", statusTextName);
    RegisterField("controlsTextName", controlsTextName);
    RegisterField("menuTitleName", menuTitleName);
    RegisterField("menuBodyName", menuBodyName);
    RegisterField("menuBackgroundName", menuBackgroundName);
}

namespace
{
struct SnakeUIRegistration
{
    SnakeUIRegistration()
    {
        Engine::Serialization::RegisterComponentType<SnakeUI>("SnakeUI");
    }
};
SnakeUIRegistration g_registration;
}

void SnakeUI::Start()
{
    ResolveObjects();
}

void SnakeUI::ResolveObjects()
{
    const auto resolve = [this](const std::string& name,
        Engine::Components::UIText*& text,
        Engine::Components::UIObject*& layout)
    {
        Engine::Core::Object* object = FindObjectInSceneByName(name);
        text = object ? object->GetComponent<Engine::Components::UIText>() : nullptr;
        layout = object ? object->GetComponent<Engine::Components::UIObject>() : nullptr;
    };
    resolve(scoreTextName, m_scoreText, m_scoreLayout);
    resolve(statusTextName, m_statusText, m_statusLayout);
    resolve(controlsTextName, m_controlsText, m_controlsLayout);
    resolve(menuTitleName, m_menuTitleText, m_menuTitleLayout);
    resolve(menuBodyName, m_menuBodyText, m_menuBodyLayout);
    Engine::Core::Object* menuBackground =
        FindObjectInSceneByName(menuBackgroundName);
    m_menuBackgroundLayout = menuBackground
        ? menuBackground->GetComponent<Engine::Components::UIObject>() : nullptr;
}

void SnakeUI::ShowMenu(int selection, int speedIndex, int highScore)
{
    if (!m_menuBodyText)
        ResolveObjects();
    SetHudVisible(false);
    SetMenuVisible(true);
    if (m_menuTitleText)
    {
        m_menuTitleText->text = "SNAKE";
        m_menuTitleText->MarkConfigurationDirty();
    }
    if (!m_menuBodyText)
        return;
    constexpr const char* speeds[] = { "SLOW", "NORMAL", "FAST" };
    const char* markers[] = { "  ", "  " };
    markers[std::clamp(selection, 0, 1)] = "> ";
    m_menuBodyText->text = std::string(markers[0]) + "PLAY\n\n" +
        markers[1] + "SPEED   " + speeds[std::clamp(speedIndex, 0, 2)] +
        "\n\nHIGH SCORE   " + std::to_string(highScore) +
        "\n\nW/S SELECT   A/D CHANGE   ENTER OK";
    m_menuBodyText->MarkConfigurationDirty();
}

void SnakeUI::ShowPlaying(int score, int highScore)
{
    if (!m_scoreText)
        ResolveObjects();
    SetMenuVisible(false);
    SetHudVisible(true);
    UpdateScore(score, highScore);
    if (m_statusText)
    {
        m_statusText->text.clear();
        m_statusText->MarkConfigurationDirty();
    }
    if (m_controlsText)
    {
        m_controlsText->text = "WASD / ARROWS     ESC MENU";
        m_controlsText->MarkConfigurationDirty();
    }
}

void SnakeUI::ShowGameOver(int score, int highScore, bool won)
{
    SetMenuVisible(false);
    SetHudVisible(true);
    UpdateScore(score, highScore);
    if (!m_statusText)
        ResolveObjects();
    if (m_statusText)
    {
        m_statusText->text = won
            ? "BOARD CLEARED\n\nENTER REPLAY   ESC MENU"
            : "GAME OVER\n\nENTER REPLAY   ESC MENU";
        m_statusText->MarkConfigurationDirty();
    }
}

void SnakeUI::UpdateScore(int score, int highScore)
{
    if (!m_scoreText)
        ResolveObjects();
    if (!m_scoreText)
        return;
    m_scoreText->text = "SCORE " + std::to_string(score) +
        "     HIGH " + std::to_string(highScore);
    m_scoreText->MarkConfigurationDirty();
}

void SnakeUI::SetMenuVisible(bool visible)
{
    SetVisible(m_menuBackgroundLayout, visible);
    SetVisible(m_menuTitleLayout, visible);
    SetVisible(m_menuBodyLayout, visible);
}

void SnakeUI::SetHudVisible(bool visible)
{
    SetVisible(m_scoreLayout, visible);
    SetVisible(m_statusLayout, visible);
    SetVisible(m_controlsLayout, visible);
}

void SnakeUI::SetVisible(Engine::Components::UIObject* layout, bool visible)
{
    if (!layout)
        return;
    layout->visible = visible;
    layout->MarkConfigurationDirty();
}
