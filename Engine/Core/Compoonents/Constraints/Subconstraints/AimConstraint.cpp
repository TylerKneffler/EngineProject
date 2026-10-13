#include "Core/Compoonents/Constraints/Subconstraints/AimConstraint.h"
#include <cmath>

namespace Engine::Components
{
AimConstraint::AimConstraint()
{
    SetTypeName(COMPONENT_TYPE_NAME(AimConstraint));
    editorAddable = true;
    RegisterField("rotationWeight", rotationWeight, "Constraint");
    RegisterField("positionOffset", positionOffset, "Aim");
    RegisterField("rotationOffsetDegrees", rotationOffsetDegrees, "Aim");
    RegisterField("localAimAxis", localAimAxis, "Aim");
    RegisterField("localUpAxis", localUpAxis, "Aim");
    RegisterField("worldUpAxis", worldUpAxis, "Aim");
}

bool AimConstraint::DesiredRotation(const glm::vec3& currentPosition,
    const glm::vec3& targetPosition, const glm::quat& targetRotation,
    const glm::vec3& offset, const glm::quat& rotationOffset,
    glm::quat& result) const
{
    glm::vec3 forward = targetPosition + targetRotation * offset - currentPosition;
    if (glm::dot(forward, forward) < 1e-10f ||
        glm::dot(localAimAxis, localAimAxis) < 1e-10f ||
        glm::dot(localUpAxis, localUpAxis) < 1e-10f) return false;
    forward = glm::normalize(forward);
    const glm::vec3 localForward = glm::normalize(localAimAxis);
    glm::vec3 localRight = glm::cross(localUpAxis, localForward);
    if (glm::dot(localRight, localRight) < 1e-10f) return false;
    localRight = glm::normalize(localRight);
    const glm::vec3 localVertical = glm::cross(localForward, localRight);
    glm::vec3 right = glm::cross(worldUpAxis, forward);
    if (glm::dot(right, right) < 1e-10f)
    {
        const glm::vec3 fallback = std::abs(forward.y) < .9f
            ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
        right = glm::cross(fallback, forward);
    }
    right = glm::normalize(right);
    const glm::vec3 vertical = glm::cross(forward, right);
    result = glm::normalize(glm::quat_cast(glm::mat3(right, vertical, forward)) *
        glm::inverse(glm::quat_cast(glm::mat3(
            localRight, localVertical, localForward))) * rotationOffset);
    return true;
}
}
