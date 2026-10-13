#include "Core/Compoonents/Animation/ChainIK.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

namespace Engine::Components
{
namespace
{
using Engine::Core::Object;

glm::quat WorldRotation(const Object& object)
{
    glm::mat3 basis(object.transform.GetWorldMatrix());
    for (int axis = 0; axis < 3; ++axis)
        if (glm::length(basis[axis]) > 1e-6f)
            basis[axis] = glm::normalize(basis[axis]);
    return glm::normalize(glm::quat_cast(basis));
}

void RotateWorld(Object& object, const glm::quat& delta)
{
    const glm::quat parent = object.Parent
        ? WorldRotation(*object.Parent) : glm::quat(1.f, 0.f, 0.f, 0.f);
    object.transform.rotation = glm::eulerAngles(glm::normalize(
        glm::inverse(parent) * delta * parent *
        glm::quat(object.transform.rotation)));
}

glm::quat FromTo(const glm::vec3& from, const glm::vec3& to,
    const glm::vec3& pole, float maxAngle)
{
    const float cosine = std::clamp(glm::dot(from, to), -1.f, 1.f);
    float angle = std::min(std::acos(cosine), maxAngle);
    if (angle < 1e-6f) return glm::quat(1.f, 0.f, 0.f, 0.f);
    glm::vec3 axis = glm::cross(from, to);
    if (glm::dot(axis, axis) < 1e-10f)
    {
        axis = pole - from * glm::dot(pole, from);
        if (glm::dot(axis, axis) < 1e-10f)
        {
            const glm::vec3 basis = std::abs(from.y) < .9f
                ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
            axis = glm::cross(from, basis);
        }
    }
    return glm::angleAxis(angle, glm::normalize(axis));
}

bool ValidChain(const ChainIK::Effector& effector)
{
    if (effector.chain.size() < 2 || !effector.chain.back()) return false;
    for (size_t i = 0; i < effector.chain.size(); ++i)
        if (!effector.chain[i] || (i > 0 &&
            effector.chain[i]->Parent != effector.chain[i - 1])) return false;
    return true;
}
}

ChainIK::ChainIK()
{
    SetTypeName(COMPONENT_TYPE_NAME(ChainIK));
    RegisterField("tipReference", tipReference);
    RegisterField("targetReference", targetReference);
    RegisterField("poleReference", poleReference);
    RegisterField("positionWeight", positionWeight);
    RegisterField("rotationWeight", rotationWeight);
    RegisterField("rootTranslationWeight", rootTranslationWeight);
    RegisterField("maxJointAngleDegrees", maxJointAngleDegrees);
    RegisterField("iterations", iterations);
    RegisterField("tolerance", tolerance);
    RegisterField("maintainTargetOffset", maintainTargetOffset);
    RegisterField("projectTargetToGround", projectTargetToGround);
    RegisterField("alignToGroundNormal", alignToGroundNormal);
    RegisterField("groundProbeHeight", groundProbeHeight);
    RegisterField("groundProbeDistance", groundProbeDistance);
    RegisterField("groundClearance", groundClearance);
}

void ChainIK::Start()
{
    m_offsetReady = false;
    m_lastSolved = false;
}

bool ChainIK::SolveMultiple(const std::vector<Effector>& effectors,
    int iterations, float tolerance, float rootTranslationWeight,
    float maxJointAngleDegrees)
{
    if (effectors.empty()) return false;
    Object* root = effectors.front().chain.empty() ? nullptr :
        effectors.front().chain.front();
    if (!root) return false;
    for (const auto& effector : effectors)
        if (!ValidChain(effector) || effector.chain.front() != root)
            return false;
    iterations = std::clamp(iterations, 1, 128);
    tolerance = std::clamp(tolerance, 1e-6f, 1.f);
    const float maxAngle = glm::radians(std::clamp(maxJointAngleDegrees,
        1.f, 180.f));
    const float rootBlend = std::clamp(rootTranslationWeight, 0.f, 1.f);
    std::vector<glm::vec3> goals;
    goals.reserve(effectors.size());
    for (const auto& effector : effectors)
        goals.push_back(glm::mix(
            effector.chain.back()->transform.GetWorldPosition(),
            effector.target, std::clamp(effector.positionWeight, 0.f, 1.f)));

    for (int pass = 0; pass < iterations; ++pass)
    {
        float largestError = 0.f;
        for (size_t i = 0; i < effectors.size(); ++i)
            largestError = std::max(largestError, glm::distance(
                effectors[i].chain.back()->transform.GetWorldPosition(), goals[i]));
        if (largestError <= tolerance) break;
        if (rootBlend > 0.f)
        {
            glm::vec3 error(0.f);
            float total = 0.f;
            for (size_t i = 0; i < effectors.size(); ++i)
            {
                const float weight = std::clamp(effectors[i].positionWeight,
                    0.f, 1.f);
                error += (goals[i] - effectors[i].chain.back()->transform
                    .GetWorldPosition()) * weight;
                total += weight;
            }
            if (total > 1e-6f)
            {
                const glm::vec3 delta = error / total * rootBlend * .25f;
                if (root->Parent)
                    root->transform.position += glm::vec3(
                        glm::inverse(root->Parent->transform.GetWorldMatrix()) *
                        glm::vec4(delta, 0.f));
                else root->transform.position += delta;
            }
        }
        for (size_t i = 0; i < effectors.size(); ++i)
        {
            const auto& effector = effectors[i];
            if (effector.positionWeight <= 0.f) continue;
            Object* tip = effector.chain.back();
            for (size_t jointIndex = effector.chain.size() - 1;
                jointIndex-- > 0;)
            {
                Object* joint = effector.chain[jointIndex];
                const glm::vec3 pivot = joint->transform.GetWorldPosition();
                const glm::vec3 toTip = tip->transform.GetWorldPosition() - pivot;
                const glm::vec3 toGoal = goals[i] - pivot;
                if (glm::dot(toTip, toTip) < 1e-12f ||
                    glm::dot(toGoal, toGoal) < 1e-12f) continue;
                const glm::vec3 pole = effector.usePole
                    ? effector.pole - pivot
                    : effector.chain[std::min(jointIndex + 1,
                        effector.chain.size() - 1)]->transform.GetWorldPosition() - pivot;
                RotateWorld(*joint, FromTo(glm::normalize(toTip),
                    glm::normalize(toGoal), pole, maxAngle));
            }
        }
    }
    for (const auto& effector : effectors)
    {
        const float weight = std::clamp(effector.rotationWeight, 0.f, 1.f);
        if (weight <= 0.f) continue;
        Object* tip = effector.chain.back();
        const glm::quat current = WorldRotation(*tip);
        RotateWorld(*tip, glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f),
            effector.rotation * glm::inverse(current), weight));
    }
    return true;
}

void ChainIK::ApplyGroup(const std::vector<ChainIK*>& group)
{
    if (group.empty() || !group.front() || !group.front()->Owner) return;
    Object* root = group.front()->Owner;
    std::vector<Effector> effectors;
    int passes = 1;
    float threshold = 1.f;
    float translation = 0.f;
    float maxAngle = 180.f;
    for (auto* component : group)
    {
        if (!component || component->Owner != root) continue;
        auto* tipTransform = Engine::Core::ResolveComponentReference<Transform>(
            root, component->tipReference);
        auto* targetTransform = Engine::Core::ResolveComponentReference<Transform>(
            root, component->targetReference);
        auto* poleTransform = Engine::Core::ResolveComponentReference<Transform>(
            root, component->poleReference);
        Object* tip = tipTransform ? tipTransform->Owner : nullptr;
        Object* target = targetTransform ? targetTransform->Owner : nullptr;
        if (!tip || (!target && !component->projectTargetToGround) ||
            target == root) continue;
        Effector effector;
        for (auto* joint = tip; joint; joint = joint->Parent)
        {
            effector.chain.push_back(joint);
            if (joint == root) break;
        }
        if (effector.chain.empty() || effector.chain.back() != root)
            continue;
        std::reverse(effector.chain.begin(), effector.chain.end());
        if (!ValidChain(effector)) continue;
        // A target inside its own chain would move when the solver rotates it.
        bool cycle = false;
        for (auto* parent = target; parent; parent = parent->Parent)
            if (parent == root) { cycle = true; break; }
        if (cycle) continue;
        if (target && component->maintainTargetOffset &&
            !component->m_offsetReady)
        {
            component->m_targetOffset =
                glm::inverse(target->transform.GetWorldMatrix()) *
                tip->transform.GetWorldMatrix();
            component->m_offsetReady = true;
        }
        glm::mat4 targetWorld = target
            ? target->transform.GetWorldMatrix()
            : tip->transform.GetWorldMatrix();
        if (target && component->maintainTargetOffset)
            targetWorld *= component->m_targetOffset;
        glm::vec3 groundNormal(0.f, 1.f, 0.f);
        if (component->projectTargetToGround)
        {
            const glm::vec3 desired(targetWorld[3]);
            const auto hits = root->GetScene()->GetPhysics().RaycastAll(
                desired + glm::vec3(0.f,
                    std::max(0.f, component->groundProbeHeight), 0.f),
                glm::vec3(0.f, -1.f, 0.f),
                std::max(0.01f, component->groundProbeDistance));
            bool grounded = false;
            for (const auto& hit : hits)
            {
                if (!hit.object || !hit.rigidBody || hit.normal.y < .45f ||
                    hit.rigidBody->bodyType == "Dynamic") continue;
                bool ownBody = false;
                for (auto* parent = hit.object; parent; parent = parent->Parent)
                    if (parent == root) { ownBody = true; break; }
                if (ownBody) continue;
                targetWorld[3] = glm::vec4(hit.point + hit.normal *
                    component->groundClearance, 1.f);
                groundNormal = glm::normalize(hit.normal);
                grounded = true;
                break;
            }
            if (!grounded) continue;
        }
        effector.target = glm::vec3(targetWorld[3]);
        glm::mat3 basis(targetWorld);
        for (int axis = 0; axis < 3; ++axis)
            if (glm::length(basis[axis]) > 1e-6f)
                basis[axis] = glm::normalize(basis[axis]);
        effector.rotation = glm::normalize(glm::quat_cast(basis));
        if (component->projectTargetToGround &&
            component->alignToGroundNormal)
            effector.rotation = FromTo(effector.rotation *
                    glm::vec3(0.f, 1.f, 0.f),
                groundNormal, glm::vec3(1.f, 0.f, 0.f), glm::pi<float>()) *
                effector.rotation;
        effector.usePole = poleTransform != nullptr;
        if (poleTransform)
            effector.pole = poleTransform->GetWorldPosition();
        effector.positionWeight = component->positionWeight;
        effector.rotationWeight = component->rotationWeight;
        passes = std::max(passes, component->iterations);
        threshold = std::min(threshold, component->tolerance);
        translation = std::max(translation, component->rootTranslationWeight);
        maxAngle = std::min(maxAngle, component->maxJointAngleDegrees);
        effectors.push_back(std::move(effector));
    }
    if (effectors.empty()) return;
    auto* history = group.front();
    std::unordered_set<Object*> affected;
    for (const auto& effector : effectors)
        affected.insert(effector.chain.begin(), effector.chain.end());
    if (history->m_lastSolved && affected.size() ==
        history->m_lastOutputRotations.size())
    {
        bool unchanged = glm::distance(root->transform.position,
            history->m_lastOutputPosition) < 1e-5f;
        for (auto* joint : affected)
        {
            const auto found = history->m_lastOutputRotations.find(joint);
            unchanged = unchanged && found !=
                history->m_lastOutputRotations.end() &&
                glm::distance(joint->transform.rotation, found->second) < 1e-5f;
        }
        if (unchanged)
        {
            root->transform.position = history->m_lastInputPosition;
            for (auto* joint : affected)
                joint->transform.rotation =
                    history->m_lastInputRotations[joint];
        }
    }
    history->m_lastInputPosition = root->transform.position;
    history->m_lastInputRotations.clear();
    for (auto* joint : affected)
        history->m_lastInputRotations[joint] = joint->transform.rotation;
    history->m_lastSolved = SolveMultiple(effectors, passes, threshold,
        translation, maxAngle);
    history->m_lastOutputPosition = root->transform.position;
    history->m_lastOutputRotations.clear();
    if (history->m_lastSolved)
        for (auto* joint : affected)
            history->m_lastOutputRotations[joint] = joint->transform.rotation;
}
}
