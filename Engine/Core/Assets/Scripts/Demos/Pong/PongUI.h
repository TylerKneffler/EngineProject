#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

namespace Engine::Components
{
class UIObject;
class UIText;
}

class PongUI final : public Engine::Core::Script
{
public:
    PongUI();

    PROPERTY(Inspector, EditAnywhere, Category = "Pong | UI")
    std::string scoreTextName = "Score";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | UI")
    std::string statusTextName = "Status";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | UI")
    std::string controlsTextName = "Controls";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | UI")
    std::string menuTitleName = "Menu Title";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | UI")
    std::string menuBodyName = "Menu Body";

    void Start() override;
    void ShowMenu(int selection, bool aiOpponent, int scoreLimit);
    void ShowPlaying(int leftScore, int rightScore, bool aiOpponent);
    void ShowGameOver(int winningPlayer, int leftScore, int rightScore);
    void UpdateScore(int leftScore, int rightScore);
    void SetStatus(const std::string& text);

private:
    void ResolveObjects();
    void SetMenuVisible(bool visible);
    void SetHudVisible(bool visible);
    static void SetVisible(Engine::Components::UIObject* layout, bool visible);

    Engine::Components::UIText* m_scoreText = nullptr;
    Engine::Components::UIText* m_statusText = nullptr;
    Engine::Components::UIText* m_controlsText = nullptr;
    Engine::Components::UIText* m_menuTitleText = nullptr;
    Engine::Components::UIText* m_menuBodyText = nullptr;
    Engine::Components::UIObject* m_scoreLayout = nullptr;
    Engine::Components::UIObject* m_statusLayout = nullptr;
    Engine::Components::UIObject* m_controlsLayout = nullptr;
    Engine::Components::UIObject* m_menuTitleLayout = nullptr;
    Engine::Components::UIObject* m_menuBodyLayout = nullptr;
};
