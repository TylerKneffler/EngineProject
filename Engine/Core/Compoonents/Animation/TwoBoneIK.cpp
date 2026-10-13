#include "Core/Compoonents/Animation/TwoBoneIK.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Components
{
namespace
{
glm::quat WorldRotation(const Engine::Core::Object& object)
{
    glm::mat3 basis(object.transform.GetWorldMatrix());
    for (int i = 0; i < 3; ++i)
    {
        const float length = glm::length(basis[i]);
        if (length > 1e-6f) basis[i] /= length;
    }
    return glm::normalize(glm::quat_cast(basis));
}

void RotateWorld(Engine::Core::Object& object, const glm::quat& delta)
{
    const glm::quat parent = object.Parent
        ? WorldRotation(*object.Parent) : glm::quat(1.f, 0.f, 0.f, 0.f);
    const glm::quat local = glm::quat(object.transform.rotation);
    object.transform.rotation = glm::eulerAngles(glm::normalize(
        glm::inverse(parent) * delta * parent * local));
}

glm::quat FromTo(const glm::vec3& from, const glm::vec3& to,
    const glm::vec3& fallbackAxis)
{
    const float dot = std::clamp(glm::dot(from, to), -1.f, 1.f);
    if (dot > 1.f - 1e-6f) return glm::quat(1.f, 0.f, 0.f, 0.f);
    glm::vec3 axis = glm::cross(from, to);
    if (glm::dot(axis, axis) < 1e-10f)
    {
        axis = fallbackAxis - from * glm::dot(fallbackAxis, from);
        if (glm::dot(axis, axis) < 1e-10f)
        {
            const glm::vec3 basis = std::abs(from.y) < .9f
                ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
            axis = glm::cross(from, basis);
        }
    }
    return glm::angleAxis(std::acos(dot), glm::normalize(axis));
}
}

TwoBoneIK::TwoBoneIK()
{
    SetTypeName(COMPONENT_TYPE_NAME(TwoBoneIK));
    RegisterField("lowerReference", lowerReference);
    RegisterField("tipReference", tipReference);
    RegisterField("targetReference", targetReference);
    RegisterField("poleReference", poleReference);
    RegisterField("positionWeight", positionWeight);
    RegisterField("rotationWeight", rotationWeight);
    RegisterField("maintainTargetOffset", maintainTargetOffset);
}

void TwoBoneIK::Start()
{
    m_offsetReady = false;
    m_lastSolved = false;
}

void TwoBoneIK::Apply()
{
    if (!Owner || !Owner->GetScene()) return;
    auto* lowerTransform = Engine::Core::ResolveComponentReference<
        Engine::Components::Transform>(Owner, lowerReference);
    auto* tipTransform = Engine::Core::ResolveComponentReference<
        Engine::Components::Transform>(Owner, tipReference);
    auto* targetTransform = Engine::Core::ResolveComponentReference<
        Engine::Components::Transform>(Owner, targetReference);
    auto* poleTransform = Engine::Core::ResolveComponentReference<
        Engine::Components::Transform>(Owner, poleReference);
    auto* lower = lowerTransform ? lowerTransform->Owner : nullptr;
    auto* tip = tipTransform ? tipTransform->Owner : nullptr;
    auto* target = targetTransform ? targetTransform->Owner : nullptr;
    auto* pole = poleTransform ? poleTransform->Owner : nullptr;
    if (!lower || !tip || !target || lower->Parent != Owner ||
        tip->Parent != lower) return;
    const std::array<Engine::Core::Object*, 3> chain{ Owner, lower, tip };
    if (m_lastSolved)
    {
        bool unchanged = true;
        for (size_t i = 0; i < chain.size(); ++i)
            unchanged = unchanged && glm::distance(
                chain[i]->transform.rotation, m_lastOutputRotations[i]) < 1e-5f;
        if (unchanged)
            for (size_t i = 0; i < chain.size(); ++i)
                chain[i]->transform.rotation = m_lastInputRotations[i];
    }
    for (size_t i = 0; i < chain.size(); ++i)
        m_lastInputRotations[i] = chain[i]->transform.rotation;
    if (maintainTargetOffset && !m_offsetReady)
    {
        m_targetOffset = glm::inverse(target->transform.GetWorldMatrix()) *
            tip->transform.GetWorldMatrix();
        m_offsetReady = true;
    }
    const glm::vec3 root = Owner->transform.GetWorldPosition();
    const glm::vec3 mid = lower->transform.GetWorldPosition();
    const glm::vec3 end = tip->transform.GetWorldPosition();
    const glm::vec3 polePosition = pole ? pole->transform.GetWorldPosition()
        : mid + (mid - (root + end) * 0.5f);
    const glm::mat4 targetWorld = target->transform.GetWorldMatrix();
    m_lastSolved = Solve(*Owner, *lower, *tip, target->transform.GetWorldPosition(),
        polePosition, positionWeight, rotationWeight, &targetWorld,
        maintainTargetOffset ? &m_targetOffset : nullptr);
    if (m_lastSolved)
        for (size_t i = 0; i < chain.size(); ++i)
            m_lastOutputRotations[i] = chain[i]->transform.rotation;
}

bool TwoBoneIK::Solve(Engine::Core::Object& upper,
    Engine::Core::Object& lower, Engine::Core::Object& tip,
    const glm::vec3& targetPosition, const glm::vec3& polePosition,
    float positionWeight, float rotationWeight,
    const glm::mat4* targetWorld, const glm::mat4* targetOffset)
{
    if (lower.Parent != &upper || tip.Parent != &lower) return false;
    const glm::vec3 root = upper.transform.GetWorldPosition();
    const glm::vec3 mid = lower.transform.GetWorldPosition();
    const glm::vec3 end = tip.transform.GetWorldPosition();
    const float upperLength = glm::distance(root, mid);
    const float lowerLength = glm::distance(mid, end);
    if (upperLength < 1e-5f || lowerLength < 1e-5f) return false;
    const float positionBlend = std::clamp(positionWeight, 0.f, 1.f);
    const float rotationBlend = std::clamp(rotationWeight, 0.f, 1.f);
    if (positionBlend > 0.f)
    {
        const glm::vec3 desired = targetWorld && targetOffset
            ? glm::vec3((*targetWorld * *targetOffset)[3]) : targetPosition;
        const glm::vec3 blended = glm::mix(end, desired, positionBlend);
        glm::vec3 direction = blended - root;
        float distance = glm::length(direction);
        if (distance < 1e-6f)
        {
            direction = end - root;
            distance = glm::length(direction);
            if (distance < 1e-6f) direction = mid - root;
        }
        direction = glm::normalize(direction);
        // Stay just inside both singular limits so the bend direction remains
        // defined and moving the target through full extension does not flip it.
        const float bendEpsilon = std::min(1e-4f,
            std::min(upperLength, lowerLength) * 0.01f);
        const float reach = std::clamp(distance,
            std::abs(upperLength - lowerLength) + bendEpsilon,
            upperLength + lowerLength - bendEpsilon);
        glm::vec3 bend = polePosition - root;
        bend -= direction * glm::dot(bend, direction);
        if (glm::dot(bend, bend) < 1e-8f)
        {
            bend = mid - root;
            bend -= direction * glm::dot(bend, direction);
        }
        if (glm::dot(bend, bend) < 1e-8f)
        {
            const glm::vec3 axis = std::abs(direction.y) < 0.9f
                ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
            bend = glm::cross(direction, axis);
        }
        bend = glm::normalize(bend);
        const float along = (upperLength * upperLength + reach * reach -
            lowerLength * lowerLength) / (2.f * reach);
        const float height = std::sqrt(std::max(0.f,
            upperLength * upperLength - along * along));
        const glm::vec3 solvedMid = root + direction * along + bend * height;
        const glm::vec3 solvedEnd = root + direction * reach;
        const glm::vec3 oldUpper = glm::normalize(mid - root);
        RotateWorld(upper, FromTo(oldUpper,
            glm::normalize(solvedMid - root), bend));
        const glm::vec3 currentMid = lower.transform.GetWorldPosition();
        const glm::vec3 currentEnd = tip.transform.GetWorldPosition();
        RotateWorld(lower, FromTo(glm::normalize(currentEnd - currentMid),
            glm::normalize(solvedEnd - currentMid), bend));
    }
    if (rotationBlend > 0.f && targetWorld)
    {
        const glm::mat4 desiredWorld = targetOffset
            ? *targetWorld * *targetOffset : *targetWorld;
        glm::mat3 basis(desiredWorld);
        for (int i = 0; i < 3; ++i)
            if (glm::length(basis[i]) > 1e-6f)
                basis[i] = glm::normalize(basis[i]);
        const glm::quat desiredRotation = glm::normalize(glm::quat_cast(basis));
        const glm::quat currentRotation = WorldRotation(tip);
        RotateWorld(tip, glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f),
            desiredRotation * glm::inverse(currentRotation), rotationBlend));
    }
    return true;
}

bool TwoBoneIK::Blend(Engine::Core::Object& upper,
    Engine::Core::Object& lower, Engine::Core::Object& tip,
    const glm::vec3& targetPosition, const glm::vec3& polePosition,
    const glm::quat& targetRotation, float blend)
{
    blend = std::clamp(blend, 0.f, 1.f);
    if (blend <= 0.f) return true;
    const glm::quat fkUpper(upper.transform.rotation);
    const glm::quat fkLower(lower.transform.rotation);
    const glm::quat fkTip(tip.transform.rotation);
    const glm::mat4 world = glm::translate(glm::mat4(1.f), targetPosition) *
        glm::mat4_cast(targetRotation);
    if (!Solve(upper, lower, tip, targetPosition, polePosition,
            1.f, 1.f, &world)) return false;
    if (blend < 1.f)
    {
        upper.transform.rotation = glm::eulerAngles(glm::normalize(glm::slerp(
            fkUpper, glm::quat(upper.transform.rotation), blend)));
        lower.transform.rotation = glm::eulerAngles(glm::normalize(glm::slerp(
            fkLower, glm::quat(lower.transform.rotation), blend)));
        tip.transform.rotation = glm::eulerAngles(glm::normalize(glm::slerp(
            fkTip, glm::quat(tip.transform.rotation), blend)));
    }
    return true;
}
}
