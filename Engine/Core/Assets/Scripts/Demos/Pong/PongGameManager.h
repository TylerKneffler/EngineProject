#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

class PongBall;
class PongUI;

class PongGameManager final : public Engine::Core::Script
{
public:
    PongGameManager();

    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string ballObjectName = "Ball";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string uiObjectName = "Pong HUD";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Match", ClampMin = "1")
    int winningScore = 11;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Match")
    bool rightPaddleAi = true;

    void Start() override;
    void Update() override;

    bool IsPlaying() const;
    bool IsRightPaddleAi() const { return rightPaddleAi; }
    void ScorePoint(bool leftPlayerScored);
    void SetStatus(const std::string& text);

private:
    enum class GameState { Menu, Playing, GameOver };

    void ResolveComponents();
    void EnterMenu();
    void StartMatch();
    void UpdateMenu(bool confirmPressed, bool upPressed, bool downPressed,
        bool leftPressed, bool rightPressed);
    void RefreshMenu();
    bool KeyPressed(int virtualKey);
    static bool IsKeyDown(int virtualKey);

    PongBall* m_ball = nullptr;
    PongUI* m_ui = nullptr;
    GameState m_state = GameState::Menu;
    int m_leftScore = 0;
    int m_rightScore = 0;
    int m_menuSelection = 0;
    bool m_keyWasDown[256] {};
};
