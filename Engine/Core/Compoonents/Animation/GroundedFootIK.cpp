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

bool GroundedFootIK::SampleGround(const glm::vec3& position,
    const Engine::Core::Object& character, glm::vec3& point) const
{
    RigidBody* body = nullptr;
    return FindGround(position, character, point, body);
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
        std::clamp(weight, 0.f, 1.f), 0.5f);
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
    float footClearance, float influence, float deltaSeconds)
{
    const float support = std::clamp(supportFraction, 0.2f, 0.85f);
    const auto smooth = [](float value)
    {
        const float t = std::clamp(value, 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    };
    for (unsigned index = 0; index < m_legs.size(); ++index)
    {
        Leg& leg = m_legs[index];
        leg.lastPoseToIKPercent = 0.f;
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
        // A support point lasts for the whole stance. Replanting when the
        // animated limb bends short makes the paw jump every few frames.
        if (stance && found && (!leg.planted ||
            (groundBody != leg.supportBody &&
                ground.y > leg.plantedPoint.y + 0.06f)))
        {
            leg.plantedPoint = ground;
            leg.supportBody = groundBody;
            leg.supportLocalPoint = groundBody && groundBody->Owner
                ? glm::vec3(glm::inverse(
                    groundBody->Owner->transform.GetWorldMatrix()) *
                    glm::vec4(ground, 1.f)) : glm::vec3(0.f);
            leg.planted = true;
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
            // local space. Ease the correction in after contact and out before
            // lift-off, leaving the clip in charge of the transition.
            const float contactWeight = smooth(phase / 0.14f) *
                smooth((support - phase) / 0.12f);
            target = glm::mix(animatedFoot,
                leg.plantedPoint + glm::vec3(0.f, footClearance, 0.f),
                contactWeight);
        }
        else if (found)
        {
            // Preserve the animated swing arc and horizontal stride. Only
            // lift the paw when the clip would put it through the terrain.
            target.y = std::max(animatedFoot.y, ground.y + footClearance);
        }
        // Animation supplies the base pose every frame. Correct only the two
        // articulated segments and retain its foot rotation and stride style.
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            RotateToward(*leg.lower, leg.foot->transform.GetWorldPosition(),
                target, influence);
            RotateToward(*leg.upper, leg.foot->transform.GetWorldPosition(),
                target, influence);
        }
        // Rotating two joints can leave a small residual on steep steps or
        // near full extension. Keep the paw above the actual support even
        // when the animated limb cannot reach its exact target.
        glm::vec3 contact;
        RigidBody* contactBody = nullptr;
        const glm::vec3 solvedFoot = leg.foot->transform.GetWorldPosition();
        if (FindGround(solvedFoot, character, contact, contactBody) &&
            solvedFoot.y < contact.y + footClearance && leg.foot->Parent)
        {
            const glm::vec3 correction(0.f,
                contact.y + footClearance - solvedFoot.y, 0.f);
            leg.foot->transform.position += glm::mat3(glm::inverse(
                leg.foot->Parent->transform.GetWorldMatrix())) * correction;
        }
        leg.lastPoseToIKPercent = 100.f * glm::distance(animatedFoot,
            leg.foot->transform.GetWorldPosition()) /
            std::max(limbLength, 1e-5f);
    }
}
}
