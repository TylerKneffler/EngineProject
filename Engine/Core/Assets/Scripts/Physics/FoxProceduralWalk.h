#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <string>

namespace Engine::Core { class Object; }
namespace Engine::Components { class RigidBody; }

// Scene choreography for the fox walking demo. The authored Walk clip drives
// the gait while GroundedFootIK adjusts each leg to the terrain.
class FoxProceduralWalk final : public Engine::Core::Script
{
public:
    FoxProceduralWalk();
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Walk")
    std::string foxObjectName = "Fox";
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Walk")
    std::string cameraObjectName = "Walk Camera";
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Walk")
    std::string animationClip = "Walk";
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Walk", ClampMin = "0")
    float forwardSpeed = 2.67f;
    // Distance traveled by the root during one complete Walk clip cycle.
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Walk", ClampMin = "0.01")
    float strideDistance = 2.7f;
    // Let the fox cross the frame before the camera begins following it.
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Walk", ClampMin = "0")
    float cameraFollowDelayDistance = 2.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Walk", ClampMin = "1")
    float obstacleRepeatDistance = 16.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Walk", ClampMin = "1")
    float floorTileLength = 48.f;

    void Start() override;
    void Update() override;
    float GetTravel() const { return m_travel; }
    float GetGaitPeriod() const { return m_gaitPeriod; }

private:
    bool Resolve();
    bool ResolveCourse();
    void RecycleCourse();
    struct CourseBody
    {
        Engine::Components::RigidBody* body = nullptr;
        glm::vec3 startPosition { 0.f };
        float initialProgress = 0.f;
        float progress = 0.f;
        float repeat = 0.f;
        float behindMargin = 0.f;
    };
    Engine::Core::Object* m_fox = nullptr;
    Engine::Core::Object* m_camera = nullptr;
    std::array<Engine::Core::Object*, 4> m_feet {};
    float m_travel = 0.f;
    float m_clipDuration = 0.f;
    float m_gaitPeriod = 0.f;
    float m_lastStrideDistance = 0.f;
    glm::vec3 m_startRoot { 0.f };
    glm::vec3 m_forward { 0.f, 0.f, -1.f };
    glm::quat m_startRotation { 1.f, 0.f, 0.f, 0.f };
    float m_startGroundHeight = 0.f;
    float m_bodyHeight = 0.f;
    glm::vec3 m_bodyUp { 0.f, 1.f, 0.f };
    glm::vec3 m_startCamera { 0.f };
    std::array<CourseBody, 5> m_course;
};
