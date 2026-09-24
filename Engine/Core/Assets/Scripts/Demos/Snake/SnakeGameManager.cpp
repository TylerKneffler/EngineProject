#include "Scripts/Demos/Snake/SnakeGameManager.h"

#include "Scripts/Demos/Snake/SnakeFood.h"
#include "Scripts/Demos/Snake/SnakePlayer.h"
#include "Scripts/Demos/Snake/SnakeUI.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>

SnakeGameManager::SnakeGameManager()
{
    SetTypeName(COMPONENT_TYPE_NAME(SnakeGameManager));
    RegisterField("headObjectName", headObjectName);
    RegisterField("foodObjectName", foodObjectName);
    RegisterField("uiObjectName", uiObjectName);
    RegisterField("segmentNamePrefix", segmentNamePrefix);
    RegisterField("maximumSegments", maximumSegments);
    RegisterField("startImmediately", startImmediately);
    RegisterField("minimumX", minimumX);
    RegisterField("maximumX", maximumX);
    RegisterField("minimumY", minimumY);
    RegisterField("maximumY", maximumY);
}

namespace
{
struct SnakeGameManagerRegistration
{
    SnakeGameManagerRegistration()
    {
        Engine::Serialization::RegisterComponentType<SnakeGameManager>(
            "SnakeGameManager");
    }
};
SnakeGameManagerRegistration g_registration;
}

void SnakeGameManager::Start()
{
    ResolveComponents();
    if (startImmediately)
        StartMatch();
    else
        EnterMenu();
}

void SnakeGameManager::Update()
{
    if (!Owner || !Owner->GetScene())
        return;
    if (!m_headObject || !m_player || !m_food || !m_ui)
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
    {
        EnterMenu();
        return;
    }

    constexpr float moveIntervals[] = { 0.19f, 0.125f, 0.075f };
    m_moveAccumulator += std::clamp(Owner->GetScene()->GetDeltaTime(),
        0.f, 0.05f);
    const float interval = moveIntervals[std::clamp(m_speedIndex, 0, 2)];
    int stepBudget = 4;
    while (m_moveAccumulator >= interval && stepBudget-- > 0 &&
        m_state == GameState::Playing)
    {
        m_moveAccumulator -= interval;
        StepSnake();
    }
}

void SnakeGameManager::ResolveComponents()
{
    m_headObject = FindObjectInSceneByName(headObjectName);
    Engine::Core::Object* foodObject = FindObjectInSceneByName(foodObjectName);
    Engine::Core::Object* uiObject = FindObjectInSceneByName(uiObjectName);
    m_player = m_headObject ? m_headObject->GetComponent<SnakePlayer>() : nullptr;
    m_food = foodObject ? foodObject->GetComponent<SnakeFood>() : nullptr;
    m_ui = uiObject ? uiObject->GetComponent<SnakeUI>() : nullptr;
    m_segments.clear();
    for (int index = 1; index <= std::max(1, maximumSegments); ++index)
    {
        Engine::Core::Object* segment = FindObjectInSceneByName(
            SegmentName(segmentNamePrefix, index));
        if (!segment)
            break;
        m_segments.push_back(segment);
    }
}

void SnakeGameManager::EnterMenu()
{
    m_state = GameState::Menu;
    ResetBoard();
    if (m_food)
        m_food->Respawn(m_body);
    if (m_ui)
        m_ui->ShowMenu(m_menuSelection, m_speedIndex, m_highScore);
}

void SnakeGameManager::StartMatch()
{
    m_score = 0;
    m_moveAccumulator = 0.f;
    m_state = GameState::Playing;
    ResetBoard();
    if (m_food)
        m_food->Respawn(m_body);
    if (m_ui)
        m_ui->ShowPlaying(m_score, m_highScore);
}

void SnakeGameManager::ResetBoard()
{
    m_direction = { 1, 0 };
    m_body = { { -2, 0 }, { -3, 0 }, { -4, 0 }, { -5, 0 } };
    if (m_player)
        m_player->ResetDirection();
    ApplyBodyTransforms();
}

void SnakeGameManager::StepSnake()
{
    if (!m_player || !m_food || m_body.empty())
        return;
    m_direction = m_player->ConsumeDirection(m_direction);
    const glm::ivec2 nextHead = m_body.front() + m_direction;
    const bool ateFood = nextHead == m_food->GetGridPosition();
    const bool hitWall = nextHead.x < minimumX || nextHead.x > maximumX ||
        nextHead.y < minimumY || nextHead.y > maximumY;
    const size_t collisionCount = ateFood ? m_body.size() : m_body.size() - 1;
    const bool hitBody = std::find(m_body.begin(),
        m_body.begin() + static_cast<std::ptrdiff_t>(collisionCount),
        nextHead) != m_body.begin() + static_cast<std::ptrdiff_t>(collisionCount);
    if (hitWall || hitBody)
    {
        FinishGame(false);
        return;
    }

    m_body.insert(m_body.begin(), nextHead);
    if (!ateFood)
        m_body.pop_back();
    else
    {
        ++m_score;
        m_highScore = std::max(m_highScore, m_score);
        if (m_ui)
            m_ui->UpdateScore(m_score, m_highScore);
        if (m_body.size() >= m_segments.size() + 1 ||
            !m_food->Respawn(m_body))
        {
            ApplyBodyTransforms();
            FinishGame(true);
            return;
        }
    }
    ApplyBodyTransforms();
}

void SnakeGameManager::ApplyBodyTransforms()
{
    if (m_headObject && !m_body.empty())
    {
        m_headObject->enabled = true;
        m_headObject->transform.position.x = static_cast<float>(m_body[0].x);
        m_headObject->transform.position.y = static_cast<float>(m_body[0].y);
    }
    for (size_t index = 0; index < m_segments.size(); ++index)
    {
        Engine::Core::Object* segment = m_segments[index];
        const size_t bodyIndex = index + 1;
        segment->enabled = bodyIndex < m_body.size();
        if (!segment->enabled)
            continue;
        segment->transform.position.x = static_cast<float>(m_body[bodyIndex].x);
        segment->transform.position.y = static_cast<float>(m_body[bodyIndex].y);
    }
}

void SnakeGameManager::FinishGame(bool won)
{
    m_state = GameState::GameOver;
    m_highScore = std::max(m_highScore, m_score);
    if (m_ui)
        m_ui->ShowGameOver(m_score, m_highScore, won);
}

void SnakeGameManager::UpdateMenu(bool confirmPressed, bool upPressed,
    bool downPressed, bool leftPressed, bool rightPressed)
{
    if (upPressed || downPressed)
        m_menuSelection = (m_menuSelection + 1) % 2;
    const bool adjust = leftPressed || rightPressed ||
        (confirmPressed && m_menuSelection == 1);
    if (adjust && m_menuSelection == 1)
        m_speedIndex = (m_speedIndex + (leftPressed ? 2 : 1)) % 3;
    else if (confirmPressed && m_menuSelection == 0)
    {
        StartMatch();
        return;
    }
    RefreshMenu();
}

void SnakeGameManager::RefreshMenu()
{
    if (m_ui)
        m_ui->ShowMenu(m_menuSelection, m_speedIndex, m_highScore);
}

bool SnakeGameManager::IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool SnakeGameManager::KeyPressed(int virtualKey)
{
    if (virtualKey < 0 || virtualKey >= 256)
        return false;
    const bool down = IsKeyDown(virtualKey);
    const bool pressed = down && !m_keyWasDown[virtualKey];
    m_keyWasDown[virtualKey] = down;
    return pressed;
}

std::string SnakeGameManager::SegmentName(const std::string& prefix, int index)
{
    return prefix + (index < 10 ? "0" : "") + std::to_string(index);
}
