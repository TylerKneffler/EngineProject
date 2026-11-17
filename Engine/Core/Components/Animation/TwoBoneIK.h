#pragma once

#include "Core/Component.h"
#include <array>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Core { class Object; }
namespace Engine::Components
{
// Solves an existing upper -> lower -> tip hierarchy without changing bone lengths.
// Attach to the upper joint for runtime use. The static solve is also used by
// animation authoring, where the targets are editor controls rather than objects.
class TwoBoneIK final : public Engine::Core::Component
{
public:
    TwoBoneIK();
    ComponentReference lowerReference { "Transform" };
    ComponentReference tipReference { "Transform" };
    ComponentReference targetReference { "Transform" };
    ComponentReference poleReference { "Transform" };
    float positionWeight = 1.f;
    float rotationWeight = 1.f;
    bool maintainTargetOffset = false;

    void Start() override;
    // Called by Scene after animation and ordinary component updates.
    void Apply();
    static bool Solve(Engine::Core::Object& upper,
        Engine::Core::Object& lower, Engine::Core::Object& tip,
        const glm::vec3& targetPosition, const glm::vec3& polePosition,
        float positionWeight = 1.f, float rotationWeight = 0.f,
        const glm::mat4* targetWorld = nullptr,
        const glm::mat4* targetOffset = nullptr);
    static bool Blend(Engine::Core::Object& upper,
        Engine::Core::Object& lower, Engine::Core::Object& tip,
        const glm::vec3& targetPosition, const glm::vec3& polePosition,
        const glm::quat& targetRotation, float blend);

private:
    glm::mat4 m_targetOffset { 1.f };
    bool m_offsetReady = false;
    bool m_lastSolved = false;
    std::array<glm::vec3, 3> m_lastInputRotations {};
    std::array<glm::vec3, 3> m_lastOutputRotations {};
};
}
