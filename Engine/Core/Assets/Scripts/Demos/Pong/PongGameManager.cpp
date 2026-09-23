#include "Scripts/Demos/Pong/PongGameManager.h"

#include "Scripts/Demos/Pong/PongBall.h"
#include "Scripts/Demos/Pong/PongUI.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>

PongGameManager::PongGameManager()
{
    SetTypeName(COMPONENT_TYPE_NAME(PongGameManager));
    RegisterField("ballObjectName", ballObjectName);
    RegisterField("uiObjectName", uiObjectName);
    RegisterField("winningScore", winningScore);
    RegisterField("rightPaddleAi", rightPaddleAi);
}

namespace
{
struct PongGameManagerRegistration
{
    PongGameManagerRegistration()
    {
        Engine::Serialization::RegisterComponentType<PongGameManager>(
            "PongGameManager");
    }
};
PongGameManagerRegistration g_registration;
}

void PongGameManager::Start()
{
    ResolveComponents();
    EnterMenu();
}

void PongGameManager::Update()
{
    if (!m_ball || !m_ui)
        ResolveComponents();

    const bool enterPressed = KeyPressed(VK_RETURN);
    const bool spacePressed = KeyPressed(VK_SPACE);
    const bool confirmPressed = enterPressed || spacePressed;
    const bool escapePressed = KeyPressed(VK_ESCAPE);
    const bool arrowUpPressed = KeyPressed(VK_UP);
    const bool wPressed = KeyPressed('W');
    const bool arrowDownPressed = KeyPressed(VK_DOWN);
    const bool sPressed = KeyPressed('S');
    const bool arrowLeftPressed = KeyPressed(VK_LEFT);
    const bool aPressed = KeyPressed('A');
    const bool arrowRightPressed = KeyPressed(VK_RIGHT);
    const bool dPressed = KeyPressed('D');

    if (m_state == GameState::Menu)
    {
        UpdateMenu(confirmPressed, arrowUpPressed || wPressed,
            arrowDownPressed || sPressed, arrowLeftPressed || aPressed,
            arrowRightPressed || dPressed);
        return;
    }
    if (m_state == GameState::GameOver)
    {
        if (confirmPressed)
            StartMatch();
        else if (escapePressed)
            EnterMenu();
        return;
    }
    if (escapePressed)
        EnterMenu();
}

bool PongGameManager::IsPlaying() const
{
    return m_state == GameState::Playing;
}

void PongGameManager::ResolveComponents()
{
    Engine::Core::Object* ballObject = FindObjectInSceneByName(ballObjectName);
    Engine::Core::Object* uiObject = FindObjectInSceneByName(uiObjectName);
    m_ball = ballObject ? ballObject->GetComponent<PongBall>() : nullptr;
    m_ui = uiObject ? uiObject->GetComponent<PongUI>() : nullptr;
}

void PongGameManager::EnterMenu()
{
    m_state = GameState::Menu;
    if (m_ball)
        m_ball->HoldAtCenter();
    if (m_ui)
        m_ui->ShowMenu(m_menuSelection, rightPaddleAi, winningScore);
}

void PongGameManager::StartMatch()
{
    m_leftScore = 0;
    m_rightScore = 0;
    m_state = GameState::Playing;
    if (m_ui)
        m_ui->ShowPlaying(m_leftScore, m_rightScore, rightPaddleAi);
    if (m_ball)
        m_ball->ResetRound(1.f);
}

void PongGameManager::UpdateMenu(bool confirmPressed, bool upPressed,
    bool downPressed, bool leftPressed, bool rightPressed)
{
    if (upPressed)
        m_menuSelection = (m_menuSelection + 2) % 3;
    if (downPressed)
        m_menuSelection = (m_menuSelection + 1) % 3;

    const bool adjust = leftPressed || rightPressed ||
        (confirmPressed && m_menuSelection != 0);
    if (adjust && m_menuSelection == 1)
        rightPaddleAi = !rightPaddleAi;
    else if (adjust && m_menuSelection == 2)
    {
        constexpr int scoreLimits[] = { 5, 11, 15 };
        int index = 0;
        for (int i = 0; i < 3; ++i)
            if (winningScore == scoreLimits[i])
                index = i;
        index = (index + (leftPressed ? 2 : 1)) % 3;
        winningScore = scoreLimits[index];
    }
    else if (confirmPressed && m_menuSelection == 0)
    {
        StartMatch();
        return;
    }
    RefreshMenu();
}

void PongGameManager::RefreshMenu()
{
    if (m_ui)
        m_ui->ShowMenu(m_menuSelection, rightPaddleAi, winningScore);
}

void PongGameManager::ScorePoint(bool leftPlayerScored)
{
    if (!IsPlaying())
        return;
    if (leftPlayerScored)
        ++m_leftScore;
    else
        ++m_rightScore;

    if (m_leftScore >= std::max(1, winningScore) ||
        m_rightScore >= std::max(1, winningScore))
    {
        m_state = GameState::GameOver;
        if (m_ball)
            m_ball->HoldAtCenter();
        if (m_ui)
            m_ui->ShowGameOver(m_leftScore > m_rightScore ? 1 : 2,
                m_leftScore, m_rightScore);
        return;
    }

    if (m_ui)
        m_ui->UpdateScore(m_leftScore, m_rightScore);
    if (m_ball)
        m_ball->ResetRound(leftPlayerScored ? 1.f : -1.f);
}

void PongGameManager::SetStatus(const std::string& text)
{
    if (m_ui)
        m_ui->SetStatus(text);
}

bool PongGameManager::IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool PongGameManager::KeyPressed(int virtualKey)
{
    if (virtualKey < 0 || virtualKey >= 256)
        return false;
    const bool down = IsKeyDown(virtualKey);
    const bool pressed = down && !m_keyWasDown[virtualKey];
    m_keyWasDown[virtualKey] = down;
    return pressed;
}
