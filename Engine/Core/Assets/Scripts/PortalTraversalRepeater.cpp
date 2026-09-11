#include "Scripts/PortalTraversalRepeater.h"

#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

PortalTraversalRepeater::PortalTraversalRepeater()
{
    SetTypeName(COMPONENT_TYPE_NAME(PortalTraversalRepeater));
    RegisterField("sourcePortalObjectName", sourcePortalObjectName);
    RegisterField("targetPortalObjectName", targetPortalObjectName);
    RegisterField("reverseClearance", reverseClearance);
    RegisterField("teleportDetectionDistance", teleportDetectionDistance);
    RegisterField("reverseAngularVelocity", reverseAngularVelocity);
}

namespace
{
struct PortalTraversalRepeaterRegistration
{
    PortalTraversalRepeaterRegistration()
    {
        Engine::Serialization::RegisterComponentType<PortalTraversalRepeater>(
            "PortalTraversalRepeater");
    }
};

PortalTraversalRepeaterRegistration g_registration;
}

void PortalTraversalRepeater::Start()
{
    m_hasPreviousPosition = Owner != nullptr;
    m_previousPosition = Owner
        ? Owner->transform.GetWorldPosition() : glm::vec3(0.f);
    m_destinationPortalName.clear();
    m_waitingForClearance = false;
}

void PortalTraversalRepeater::Update()
{
    using Engine::Components::RigidBody;
    using Engine::Components::SpatialManipulator;
    if (!Owner)
        return;
    RigidBody* body = Owner->GetComponent<RigidBody>();
    Engine::Core::Object* sourceObject = Owner->FindObjectInSceneByName(
        sourcePortalObjectName);
    Engine::Core::Object* targetObject = Owner->FindObjectInSceneByName(
        targetPortalObjectName);
    if (!body || !sourceObject || !targetObject)
        return;
    auto* source = sourceObject->GetComponent<SpatialManipulator>();
    auto* target = targetObject->GetComponent<SpatialManipulator>();
    if (!source || !target)
        return;

    const glm::vec3 position = Owner->transform.GetWorldPosition();
    if (!m_hasPreviousPosition)
    {
        m_previousPosition = position;
        m_hasPreviousPosition = true;
        return;
    }

    const float jumpDistance = glm::length(position - m_previousPosition);
    if (!m_waitingForClearance && jumpDistance >=
        std::max(0.01f, teleportDetectionDistance))
    {
        const glm::vec3 sourceAnchor(source->GetPortalWorldFrame()[3]);
        const glm::vec3 targetAnchor(target->GetPortalWorldFrame()[3]);
        m_destinationPortalName =
            glm::length(position - sourceAnchor) <
                glm::length(position - targetAnchor)
            ? sourcePortalObjectName : targetPortalObjectName;
        m_waitingForClearance = true;
    }

    if (m_waitingForClearance)
    {
        Engine::Core::Object* destinationObject =
            m_destinationPortalName == sourcePortalObjectName
            ? sourceObject : targetObject;
        auto* destination = destinationObject->GetComponent<SpatialManipulator>();
        const glm::mat4 frame = destination->GetPortalWorldFrame();
        const glm::vec3 anchor(frame[3]);
        const glm::vec3 normal = glm::normalize(glm::vec3(frame[2]));
        const float signedDistance = glm::dot(position - anchor, normal);
        const glm::vec3 velocity = body->GetLinearVelocity();
        const bool movingAway = signedDistance * glm::dot(velocity, normal) > 0.f;
        if (movingAway && std::abs(signedDistance) >=
            std::max(0.01f, reverseClearance))
        {
            body->SetLinearVelocity(-velocity);
            if (reverseAngularVelocity)
                body->SetAngularVelocity(-body->GetAngularVelocity());
            m_waitingForClearance = false;
        }
    }

    m_previousPosition = position;
}
