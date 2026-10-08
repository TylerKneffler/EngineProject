#include "Core/Compoonents/Animation/GroundedFootIK.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Components
{
namespace
{
struct Registration
{
    Registration()
    {
        Engine::Serialization::RegisterComponentType<GroundedFootIK>(
            "GroundedFootIK");
    }
} registration;
}

GroundedFootIK::GroundedFootIK()
{
    SetTypeName(COMPONENT_TYPE_NAME(GroundedFootIK));
}

void GroundedFootIK::SetLeg(unsigned index, Engine::Core::Object* upper,
    Engine::Core::Object* lower, Engine::Core::Object* foot)
{
    if (index >= m_legs.size()) return;
    m_legs[index] = { upper, lower, foot };
}

void GroundedFootIK::Clear()
{
    m_legs = {};
}

bool GroundedFootIK::FindGround(const glm::vec3& position,
    const Engine::Core::Object& character, glm::vec3& point,
    RigidBody*& body) const
{
    if (!Owner || !Owner->GetScene()) return false;
    const auto hits = Owner->GetScene()->GetPhysics().RaycastAll(
        position + glm::vec3(0.f, 2.f, 0.f),
        glm::vec3(0.f, -1.f, 0.f), 4.5f);
    for (const auto& hit : hits)
    {
        if (!hit.rigidBody || !hit.object ||
            hit.object == &character || hit.normal.y < 0.45f ||
            hit.rigidBody->bodyType == "Dynamic")
            continue;
        point = hit.point;
        body = hit.rigidBody;
        return true;
    }
    return false;
}

void GroundedFootIK::RotateToward(Engine::Core::Object& joint,
    const glm::vec3& foot, const glm::vec3& target, float weight)
{
    const glm::vec3 pivot = joint.transform.GetWorldPosition();
    glm::vec3 from = foot - pivot, to = target - pivot;
    if (glm::dot(from, from) < 1e-7f ||
        glm::dot(to, to) < 1e-7f) return;
    from = glm::normalize(from);
    to = glm::normalize(to);
    const glm::vec3 cross = glm::cross(from, to);
    const float sine = glm::length(cross);
    if (sine < 1e-6f) return;
    const float angle = std::min(std::atan2(sine, glm::dot(from, to)) *
        std::clamp(weight, 0.f, 1.f), 0.32f);
    const glm::quat worldDelta = glm::angleAxis(angle, cross / sine);
    glm::quat parentWorld(1.f, 0.f, 0.f, 0.f);
    if (joint.Parent)
    {
        glm::mat3 basis(joint.Parent->transform.GetWorldMatrix());
        for (int column = 0; column < 3; ++column)
            basis[column] = glm::normalize(basis[column]);
        parentWorld = glm::normalize(glm::quat_cast(basis));
    }
    const glm::quat local = glm::quat(joint.transform.rotation);
    joint.transform.rotation = glm::eulerAngles(glm::normalize(
        glm::inverse(parentWorld) * worldDelta * parentWorld * local));
}

void GroundedFootIK::Solve(Engine::Core::Object& character,
    const std::array<float, 4>& phases, float supportFraction,
    float stepHeight, float footClearance, float influence,
    float deltaSeconds)
{
    const float support = std::clamp(supportFraction, 0.2f, 0.85f);
    const float response = 1.f - std::exp(-24.f *
        std::clamp(deltaSeconds, 0.f, 0.1f));
    for (unsigned index = 0; index < m_legs.size(); ++index)
    {
        Leg& leg = m_legs[index];
        if (!leg.upper || !leg.lower || !leg.foot) continue;
        const glm::vec3 animatedFoot = leg.foot->transform.GetWorldPosition();
        const float phase = phases[index] - std::floor(phases[index]);
        const bool stance = phase < support;
        glm::vec3 ground;
        RigidBody* groundBody = nullptr;
        const bool found = FindGround(animatedFoot, character,
            ground, groundBody);
        if (leg.planted && leg.supportBody && leg.supportBody->Owner)
            leg.plantedPoint = glm::vec3(
                leg.supportBody->Owner->transform.GetWorldMatrix() *
                glm::vec4(leg.supportLocalPoint, 1.f));
        const float limbLength = glm::distance(
            leg.upper->transform.GetWorldPosition(),
            leg.lower->transform.GetWorldPosition()) +
            glm::distance(leg.lower->transform.GetWorldPosition(),
                animatedFoot);
        // Leave a planted paw in place through small reach errors. The
        // animation changes the measured limb length as it bends, so using
        // the exact length can replant a paw mid-stance and cause a pop.
        const bool farBeyondReach = leg.planted &&
            glm::distance(leg.upper->transform.GetWorldPosition(),
                leg.plantedPoint + glm::vec3(0.f, footClearance, 0.f)) >
                limbLength * 1.2f;
        if (stance && found && (!leg.planted || farBeyondReach))
        {
            leg.plantedPoint = ground;
            leg.supportBody = groundBody;
            leg.supportLocalPoint = groundBody && groundBody->Owner
                ? glm::vec3(glm::inverse(
                    groundBody->Owner->transform.GetWorldMatrix()) *
                    glm::vec4(ground, 1.f)) : glm::vec3(0.f);
            leg.planted = true;
            leg.hasTarget = false;
            ++leg.plantSequence;
            leg.plantAgeSeconds = 0.f;
        }
        if (!stance)
        {
            leg.planted = false;
            leg.supportBody = nullptr;
            leg.plantAgeSeconds = 0.f;
        }
        if (stance && leg.planted)
            leg.plantAgeSeconds += std::max(deltaSeconds, 0.f);
        if (!found && !leg.planted) continue;

        glm::vec3 target = animatedFoot;
        if (stance && leg.planted)
        {
            // Stay at the point where this foot landed in the support's
            // local space, even if that floor or platform moves.
            target = leg.plantedPoint + glm::vec3(0.f, footClearance, 0.f);
        }
        else if (found)
        {
            const float swing = (phase - support) / (1.f - support);
            target = ground + glm::vec3(0.f,
                footClearance + std::sin(swing * 3.14159265f) * stepHeight, 0.f);
        }
        if (leg.hasTarget && !stance)
            target = glm::mix(leg.lastTarget, target, response);
        leg.lastTarget = target;
        leg.hasTarget = true;
        // Animation supplies the base pose every frame. Correct only the two
        // articulated segments and retain its foot rotation and stride style.
        for (int iteration = 0; iteration < 6; ++iteration)
        {
            RotateToward(*leg.lower, leg.foot->transform.GetWorldPosition(),
                target, influence);
            RotateToward(*leg.upper, leg.foot->transform.GetWorldPosition(),
                target, influence);
        }
    }
}
}
