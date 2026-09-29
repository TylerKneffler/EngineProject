#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

class SnakeFood;
class SnakePlayer;
class SnakeUI;

class SnakeGameManager final : public Engine::Core::Script
{
public:
    SnakeGameManager();

    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Objects")
    std::string headObjectName = "Snake Head";
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Objects")
    std::string foodObjectName = "Snake Food";
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Objects")
    std::string uiObjectName = "Snake HUD";
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Objects")
    std::string segmentNamePrefix = "Snake Segment ";
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Objects", ClampMin = "1")
    int maximumSegments = 24;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Match")
    bool startImmediately = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board")
    int minimumX = -7;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board")
    int maximumX = 7;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board")
    int minimumY = -3;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board")
    int maximumY = 3;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board", ClampMin = "0.01")
    float cellSize = 1.f;

    void Start() override;
    void Update() override;

private:
    enum class GameState { Menu, Playing, GameOver };

    void ResolveComponents();
    void EnterMenu();
    void StartMatch();
    void ResetBoard();
    void StepSnake();
    void ApplyBodyTransforms();
    void FinishGame(bool won);
    void UpdateMenu(bool confirmPressed, bool upPressed, bool downPressed,
        bool leftPressed, bool rightPressed);
    void RefreshMenu();
    bool KeyPressed(int virtualKey);
    static bool IsKeyDown(int virtualKey);
    static std::string SegmentName(const std::string& prefix, int index);

    Engine::Core::Object* m_headObject = nullptr;
    SnakePlayer* m_player = nullptr;
    SnakeFood* m_food = nullptr;
    SnakeUI* m_ui = nullptr;
    std::vector<Engine::Core::Object*> m_segments;
    std::vector<glm::ivec2> m_body;
    glm::ivec2 m_direction { 1, 0 };
    GameState m_state = GameState::Menu;
    float m_moveAccumulator = 0.f;
    int m_score = 0;
    int m_highScore = 0;
    int m_menuSelection = 0;
    int m_speedIndex = 1;
    bool m_keyWasDown[256] {};
};
