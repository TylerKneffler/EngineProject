#pragma once

#include "Core/Component.h"
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <unordered_map>

namespace Engine::Components
{
// Attach one instance per effector to the common root. The tip's parent path
// determines the chain. Scene evaluates all effectors on a root together.
class ChainIK final : public Engine::Core::Component
{
public:
    struct Effector
    {
        std::vector<Engine::Core::Object*> chain; // root through tip
        glm::vec3 target { 0.f };
        glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
        glm::vec3 pole { 0.f };
        float positionWeight = 1.f;
        float rotationWeight = 0.f;
        bool usePole = false;
        bool fixed = false; // Fixed pins are corrected after movable targets.
    };
    struct PosePreference
    {
        Engine::Core::Object* joint = nullptr;
        glm::quat localRotation { 1.f, 0.f, 0.f, 0.f };
        float stiffness = 0.f;
    };
    struct BalanceConstraint
    {
        glm::vec3 supportCenter { 0.f };
        float horizontalRadius = 0.f;
    };

    ChainIK();
    ComponentReference tipReference { "Transform" };
    ComponentReference targetReference { "Transform" };
    ComponentReference poleReference { "Transform" };
    float positionWeight = 1.f;
    float rotationWeight = 0.f;
    float rootTranslationWeight = 0.f;
    float maxJointAngleDegrees = 180.f;
    int iterations = 16;
    float tolerance = .001f;
    bool maintainTargetOffset = false;
    bool projectTargetToGround = false;
    bool alignToGroundNormal = false;
    float groundProbeHeight = 2.f;
    float groundProbeDistance = 4.5f;
    float groundClearance = 0.f;

    void Start() override;
    static bool SolveMultiple(const std::vector<Effector>& effectors,
        int iterations = 16, float tolerance = .001f,
        float rootTranslationWeight = 0.f,
        float maxJointAngleDegrees = 180.f,
        const std::vector<PosePreference>* preferences = nullptr,
        const BalanceConstraint* balance = nullptr);
    static bool ConstrainRootBalance(Engine::Core::Object& root,
        const BalanceConstraint& balance);
    static void ApplyGroup(const std::vector<ChainIK*>& group);

private:
    glm::mat4 m_targetOffset { 1.f };
    bool m_offsetReady = false;
    std::unordered_map<Engine::Core::Object*, glm::vec3> m_lastInputRotations;
    std::unordered_map<Engine::Core::Object*, glm::vec3> m_lastOutputRotations;
    glm::vec3 m_lastInputPosition { 0.f };
    glm::vec3 m_lastOutputPosition { 0.f };
    bool m_lastSolved = false;
};
}
