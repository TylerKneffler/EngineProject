#pragma once

#include "Core/Component.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>

namespace Engine::Components
{
// Shared target, offsets, weights, and dependency validation. Add a concrete
// subclass to the driven object; the base itself is not editor-addable.
class TransformConstraint : public Engine::Core::Component
{
public:
    TransformConstraint();
    ComponentReference targetReference { "Transform" };
    int evaluationPriority = 0;
    float positionWeight = 1.f;
    float rotationWeight = 1.f;
    bool maintainOffset = false;
    glm::vec3 positionOffset { 0.f }; // Target local space.
    glm::vec3 rotationOffsetDegrees { 0.f };

    void Start() override;
    void Apply();
    Engine::Core::Object* Target() const;
    std::string ValidationError() const;
    bool DrawProperties(Engine::Editor::IEditorUi& ui) override;

protected:
    virtual bool WantsPosition() const = 0;
    virtual bool WantsRotation() const = 0;
    virtual bool UseCapturedRotationOffset() const { return true; }
    virtual bool DesiredRotation(const glm::vec3& currentPosition,
        const glm::vec3& targetPosition, const glm::quat& targetRotation,
        const glm::vec3& offset, const glm::quat& rotationOffset,
        glm::quat& result) const;

private:
    Engine::Core::Object* m_lastTarget = nullptr;
    bool m_lastMaintainOffset = false;
    bool m_offsetReady = false;
    bool m_lastApplied = false;
    glm::vec3 m_capturedPosition { 0.f };
    glm::quat m_capturedRotation { 1.f, 0.f, 0.f, 0.f };
    glm::vec3 m_lastInputPosition { 0.f }, m_lastInputRotation { 0.f };
    glm::vec3 m_lastOutputPosition { 0.f }, m_lastOutputRotation { 0.f };
};
}
