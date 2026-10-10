#pragma once

#include "Core/Component.h"
#include <array>
#include <cstdint>
#include <glm/glm.hpp>

namespace Engine::Core { class Object; }
namespace Engine::Components
{
class RigidBody;
// Adds a bounded two-joint correction to an animated quadruped pose. The
// caller owns gait timing and root motion; this component owns ground queries
// and the joint solve.
class GroundedFootIK final : public Engine::Core::Component
{
public:
    struct Leg
    {
        Engine::Core::Object* upper = nullptr;
        Engine::Core::Object* lower = nullptr;
        Engine::Core::Object* foot = nullptr;
        glm::vec3 plantedPoint { 0.f };
        glm::vec3 supportLocalPoint { 0.f };
        RigidBody* supportBody = nullptr;
        bool planted = false;
        uint64_t plantSequence = 0;
        float plantAgeSeconds = 0.f;
        // Paw displacement from the animated pose, as a percentage of the
        // animated upper-to-lower-to-paw chain length for this frame.
        float lastPoseToIKPercent = 0.f;
    };

    GroundedFootIK();
    void SetLeg(unsigned index, Engine::Core::Object* upper,
        Engine::Core::Object* lower, Engine::Core::Object* foot);
    void Clear();
    bool SampleGround(const glm::vec3& position,
        const Engine::Core::Object& character, glm::vec3& point) const;
    // phase is normalized to [0,1); supportFraction controls stance duration.
    // Ground queries ignore the moving character and accept any upward-facing
    // static or kinematic collider.
    void Solve(Engine::Core::Object& character,
        const std::array<float, 4>& phases, float supportFraction,
        float footClearance, float influence, float deltaSeconds);
    const Leg& GetLeg(unsigned index) const { return m_legs.at(index); }

private:
    bool FindGround(const glm::vec3& position, const Engine::Core::Object& character,
        glm::vec3& point, RigidBody*& body) const;
    static void RotateToward(Engine::Core::Object& joint,
        const glm::vec3& foot, const glm::vec3& target, float weight);
    std::array<Leg, 4> m_legs;
};
}
