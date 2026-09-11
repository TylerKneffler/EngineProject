#include "Scripts/Spatial/WarpVolumeTraversalRepeater.h"

#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <glm/gtc/matrix_inverse.hpp>

WarpVolumeTraversalRepeater::WarpVolumeTraversalRepeater()
{
    SetTypeName(COMPONENT_TYPE_NAME(WarpVolumeTraversalRepeater));
    RegisterField("volumeObjectName", volumeObjectName);
    RegisterField("turnaroundClearance", turnaroundClearance);
}

namespace
{
struct WarpVolumeTraversalRepeaterRegistration
{
    WarpVolumeTraversalRepeaterRegistration()
    {
        Engine::Serialization::RegisterComponentType<WarpVolumeTraversalRepeater>(
            "WarpVolumeTraversalRepeater");
    }
};

WarpVolumeTraversalRepeaterRegistration g_registration;
}

void WarpVolumeTraversalRepeater::Update()
{
    using Engine::Components::RigidBody;
    using Engine::Components::SpatialManipulator;
    if (!Owner)
        return;
    RigidBody* body = Owner->GetComponent<RigidBody>();
    Engine::Core::Object* volumeObject = Owner->FindObjectInSceneByName(
        volumeObjectName);
    SpatialManipulator* volume = volumeObject
        ? volumeObject->GetComponent<SpatialManipulator>() : nullptr;
    if (!body || !volume || !volume->definesWarpVolume ||
        static_cast<SpatialManipulator::WarpVolumeShape>(
            volume->warpVolumeShape) != SpatialManipulator::WarpVolumeShape::Box)
        return;

    const glm::mat4 inverseVolume = glm::inverse(
        volumeObject->transform.GetWorldMatrix());
    const glm::vec3 localPosition(inverseVolume * glm::vec4(
        Owner->transform.GetWorldPosition(), 1.f));
    const glm::vec3 localVelocity(inverseVolume * glm::vec4(
        body->GetLinearVelocity(), 0.f));
    const float halfLength = std::max(0.001f,
        std::abs(volume->warpVolumeSize.z) * 0.5f);
    const float turnaround = halfLength + std::max(0.f, turnaroundClearance);
    if ((localVelocity.z > 0.f && localPosition.z >= turnaround) ||
        (localVelocity.z < 0.f && localPosition.z <= -turnaround))
    {
        body->SetLinearVelocity(-body->GetLinearVelocity());
    }
}
