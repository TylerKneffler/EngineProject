#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

namespace Engine::Components
{
class UIObject;
class UIText;
}

class SnakeUI final : public Engine::Core::Script
{
public:
    SnakeUI();

    PROPERTY(Inspector, EditAnywhere, Category = "Snake | UI")
    std::string scoreTextName = "Snake Score";
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | UI")
    std::string statusTextName = "Snake Status";
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | UI")
    std::string controlsTextName = "Snake Controls";
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | UI")
    std::string menuTitleName = "Snake Menu Title";
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | UI")
    std::string menuBodyName = "Snake Menu Body";

    void Start() override;
    void ShowMenu(int selection, int speedIndex, int highScore);
    void ShowPlaying(int score, int highScore);
    void ShowGameOver(int score, int highScore, bool won);
    void UpdateScore(int score, int highScore);

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
