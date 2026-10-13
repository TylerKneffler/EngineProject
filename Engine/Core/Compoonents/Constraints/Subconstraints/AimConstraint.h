#pragma once
#include "Core/Compoonents/Constraints/TransformConstraint.h"

namespace Engine::Components
{
class AimConstraint final : public TransformConstraint
{
public:
    AimConstraint();
    glm::vec3 localAimAxis { 0.f, 0.f, 1.f };
    glm::vec3 localUpAxis { 0.f, 1.f, 0.f };
    glm::vec3 worldUpAxis { 0.f, 1.f, 0.f };
protected:
    bool WantsPosition() const override { return false; }
    bool WantsRotation() const override { return true; }
    bool UseCapturedRotationOffset() const override { return false; }
    bool DesiredRotation(const glm::vec3& currentPosition,
        const glm::vec3& targetPosition, const glm::quat& targetRotation,
        const glm::vec3& offset, const glm::quat& rotationOffset,
        glm::quat& result) const override;
};
}
