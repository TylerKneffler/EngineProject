#include "Core/Compoonents/Constraints/TransformConstraint.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Object.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Components
{
namespace
{
glm::quat WorldRotation(const Engine::Core::Object& object)
{
    glm::mat3 basis(object.transform.GetWorldMatrix());
    for (int axis = 0; axis < 3; ++axis)
        if (glm::length(basis[axis]) > 1e-6f)
            basis[axis] = glm::normalize(basis[axis]);
    if (glm::determinant(basis) < 0.f) basis[0] = -basis[0];
    return glm::normalize(glm::quat_cast(basis));
}

}

TransformConstraint::TransformConstraint()
{
    SetTypeName(COMPONENT_TYPE_NAME(TransformConstraint));
    editorAddable = false;
    singlecomponent = true;
    RegisterField("targetReference", targetReference, "Constraint");
    RegisterField("evaluationPriority", evaluationPriority, "Constraint");
    RegisterField("maintainOffset", maintainOffset, "Constraint");
}

void TransformConstraint::Start()
{
    m_lastTarget = nullptr;
    m_lastMaintainOffset = false;
    m_offsetReady = false;
    m_lastApplied = false;
}

Engine::Core::Object* TransformConstraint::Target() const
{
    auto* transform = Owner ? Engine::Core::ResolveComponentReference<
        Engine::Components::Transform>(Owner, targetReference) : nullptr;
    return transform ? transform->Owner : nullptr;
}

std::string TransformConstraint::ValidationError() const
{
    if (!Owner) return "Constraint has no owner.";
    auto* target = Target();
    if (!target) return "Assign a target Transform.";
    for (auto* ancestor = target; ancestor; ancestor = ancestor->Parent)
        if (ancestor == Owner)
            return "Target cannot be this object or one of its descendants.";
    std::unordered_set<const Engine::Core::Object*> visited;
    const auto reachesOwner = [&](auto&& self,
        const Engine::Core::Object* object) -> bool
    {
        for (auto* ancestor = object; ancestor; ancestor = ancestor->Parent)
        {
            if (ancestor == Owner) return true;
            if (!visited.insert(ancestor).second) continue;
            auto* dependency = ancestor->GetComponent<TransformConstraint>();
            if (!dependency || !dependency->Target()) continue;
            if (self(self, dependency->Target())) return true;
            if (self(self, ancestor->Parent)) return true;
        }
        return false;
    };
    if (reachesOwner(reachesOwner, target) ||
        reachesOwner(reachesOwner, Owner->Parent))
        return "Constraint dependency cycle; choose a different target.";
    return {};
}

bool TransformConstraint::DrawProperties(Engine::Editor::IEditorUi& ui)
{
    const bool changed = Component::DrawProperties(ui);
    ui.ValueLabel("Evaluation", "After animation and IK; targets before dependents");
    const auto error = ValidationError();
    if (!error.empty()) ui.ColoredLabel(error.c_str(), {1.f, .55f, .35f, 1.f});
    return changed;
}

bool TransformConstraint::DesiredRotation(const glm::vec3&,
    const glm::vec3&, const glm::quat& targetRotation,
    const glm::vec3&, const glm::quat& rotationOffset,
    glm::quat& result) const
{
    result = targetRotation * rotationOffset;
    return true;
}

void TransformConstraint::Apply()
{
    auto* target = Target();
    if (!Owner || !target || !ValidationError().empty()) return;
    if (target != m_lastTarget || maintainOffset != m_lastMaintainOffset)
    {
        m_offsetReady = false;
        m_lastApplied = false;
        m_lastTarget = target;
        m_lastMaintainOffset = maintainOffset;
    }
    auto& transform = Owner->transform;
    if (m_lastApplied &&
        glm::distance(transform.position, m_lastOutputPosition) < 1e-5f &&
        glm::distance(transform.rotation, m_lastOutputRotation) < 1e-5f)
    {
        transform.position = m_lastInputPosition;
        transform.rotation = m_lastInputRotation;
    }
    m_lastInputPosition = transform.position;
    m_lastInputRotation = transform.rotation;
    const glm::vec3 currentPosition = transform.GetWorldPosition();
    const glm::quat currentRotation = WorldRotation(*Owner);
    const glm::vec3 targetPosition = target->transform.GetWorldPosition();
    const glm::quat targetRotation = WorldRotation(*target);
    if (maintainOffset && !m_offsetReady)
    {
        m_capturedPosition = glm::inverse(targetRotation) *
            (currentPosition - targetPosition);
        m_capturedRotation = glm::inverse(targetRotation) * currentRotation;
        m_offsetReady = true;
    }
    const glm::vec3 offset = maintainOffset ? m_capturedPosition : positionOffset;
    const glm::quat rotationOffset = maintainOffset &&
        UseCapturedRotationOffset() ? m_capturedRotation :
        glm::quat(glm::radians(rotationOffsetDegrees));
    const float positionBlend = std::clamp(positionWeight, 0.f, 1.f);
    const float rotationBlend = std::clamp(rotationWeight, 0.f, 1.f);
    glm::vec3 desiredPosition = currentPosition;
    glm::quat desiredRotation = currentRotation;
    if (WantsPosition())
        desiredPosition = targetPosition + targetRotation * offset;
    if (WantsRotation() && !DesiredRotation(currentPosition,
            targetPosition, targetRotation, offset, rotationOffset,
            desiredRotation)) return;
    const glm::mat4 parentWorld = Owner->Parent
        ? Owner->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
    if (WantsPosition() && positionBlend > 0.f &&
        std::abs(glm::determinant(parentWorld)) > 1e-7f)
    {
        const glm::vec3 blended = glm::mix(currentPosition,
            desiredPosition, positionBlend);
        transform.position = glm::vec3(glm::inverse(parentWorld) *
            glm::vec4(blended, 1.f));
    }
    if (WantsRotation() && rotationBlend > 0.f)
    {
        const glm::quat parentRotation = Owner->Parent
            ? WorldRotation(*Owner->Parent) : glm::quat(1.f, 0.f, 0.f, 0.f);
        transform.rotation = glm::eulerAngles(glm::normalize(
            glm::inverse(parentRotation) * glm::slerp(currentRotation,
                desiredRotation, rotationBlend)));
    }
    m_lastOutputPosition = transform.position;
    m_lastOutputRotation = transform.rotation;
    m_lastApplied = true;
}
}
