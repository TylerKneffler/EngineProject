#include "Scripts/Demos/Pong/PongUI.h"

#include "Core/Compoonents/UI/UIObject.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>

PongUI::PongUI()
{
    SetTypeName(COMPONENT_TYPE_NAME(PongUI));
    RegisterField("scoreTextName", scoreTextName);
    RegisterField("statusTextName", statusTextName);
    RegisterField("controlsTextName", controlsTextName);
    RegisterField("menuTitleName", menuTitleName);
    RegisterField("menuBodyName", menuBodyName);
    RegisterField("menuBackgroundName", menuBackgroundName);
}

namespace
{
struct PongUIRegistration
{
    PongUIRegistration()
    {
        Engine::Serialization::RegisterComponentType<PongUI>("PongUI");
    }
};
PongUIRegistration g_registration;
}

void PongUI::Start()
{
    ResolveObjects();
}

void PongUI::ResolveObjects()
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

void PongUI::ShowMenu(int selection, bool aiOpponent, int scoreLimit)
{
    if (!m_menuBodyText)
        ResolveObjects();
    SetHudVisible(false);
    SetMenuVisible(true);
    if (m_menuTitleText)
    {
        m_menuTitleText->text = "PONG";
        m_menuTitleText->MarkConfigurationDirty();
    }
    if (!m_menuBodyText)
        return;
    const char* markers[] = { "  ", "  ", "  " };
    markers[std::clamp(selection, 0, 2)] = "> ";
    m_menuBodyText->text = std::string(markers[0]) + "PLAY\n\n" +
        markers[1] + "OPPONENT   " + (aiOpponent ? "CPU" : "2 PLAYER") +
        "\n\n" + markers[2] + "FIRST TO   " + std::to_string(scoreLimit) +
        "\n\nW/S SELECT   A/D CHANGE   ENTER OK";
    m_menuBodyText->MarkConfigurationDirty();
}

void PongUI::ShowPlaying(int leftScore, int rightScore, bool aiOpponent)
{
    if (!m_scoreText)
        ResolveObjects();
    SetMenuVisible(false);
    SetHudVisible(true);
    UpdateScore(leftScore, rightScore);
    SetStatus("");
    if (m_controlsText)
    {
        m_controlsText->text = aiOpponent
            ? "W / S     ESC MENU"
            : "W / S     UP / DOWN     ESC MENU";
        m_controlsText->MarkConfigurationDirty();
    }
}

void PongUI::ShowGameOver(int winningPlayer, int leftScore, int rightScore)
{
    SetMenuVisible(false);
    SetHudVisible(true);
    UpdateScore(leftScore, rightScore);
    SetStatus("PLAYER " + std::to_string(winningPlayer) +
        " WINS\n\nENTER REPLAY   ESC MENU");
}

void PongUI::UpdateScore(int leftScore, int rightScore)
{
    if (!m_scoreText)
        ResolveObjects();
    if (!m_scoreText)
        return;
    m_scoreText->text = std::to_string(leftScore) + "     " +
        std::to_string(rightScore);
    m_scoreText->MarkConfigurationDirty();
}

void PongUI::SetStatus(const std::string& text)
{
    if (!m_statusText)
        ResolveObjects();
    if (!m_statusText)
        return;
    m_statusText->text = text;
    m_statusText->MarkConfigurationDirty();
}

void PongUI::SetMenuVisible(bool visible)
{
    SetVisible(m_menuBackgroundLayout, visible);
    SetVisible(m_menuTitleLayout, visible);
    SetVisible(m_menuBodyLayout, visible);
}

void PongUI::SetHudVisible(bool visible)
{
    SetVisible(m_scoreLayout, visible);
    SetVisible(m_statusLayout, visible);
    SetVisible(m_controlsLayout, visible);
}

void PongUI::SetVisible(Engine::Components::UIObject* layout, bool visible)
{
    if (!layout)
        return;
    layout->visible = visible;
    layout->MarkConfigurationDirty();
}
