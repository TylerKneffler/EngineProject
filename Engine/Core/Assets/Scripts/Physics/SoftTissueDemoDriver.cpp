#include "Scripts/Physics/SoftTissueDemoDriver.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>

namespace
{
struct Registration
{
    Registration()
    {
        Engine::Serialization::RegisterComponentType<SoftTissueDemoDriver>(
            "SoftTissueDemoDriver");
    }
} registration;
}

SoftTissueDemoDriver::SoftTissueDemoDriver()
{
    SetTypeName(COMPONENT_TYPE_NAME(SoftTissueDemoDriver));
    RegisterField("foxObjectName", foxObjectName, "Demo");
    RegisterField("probeObjectName", probeObjectName, "Demo");
    RegisterField("repeatSeconds", repeatSeconds, "Demo");
    RegisterField("probeHeight", probeHeight, "Demo");
    RegisterField("downwardSpeed", downwardSpeed, "Demo");
}

bool SoftTissueDemoDriver::ReleaseProbe()
{
    auto* fox = Owner ? Owner->FindObjectInSceneByName(foxObjectName) : nullptr;
    auto* probe = Owner ? Owner->FindObjectInSceneByName(probeObjectName) : nullptr;
    auto* body = probe ? probe->GetComponent<Engine::Components::RigidBody>()
        : nullptr;
    if (!fox || !body) return false;
    const glm::vec3 origin = glm::vec3(fox->transform.GetWorldMatrix()[3]);
    body->SetWorldPosition(origin + glm::vec3(0.f, probeHeight, 0.f));
    body->SetLinearVelocity(glm::vec3(0.f, -std::max(downwardSpeed, 0.f), 0.f));
    body->SetAngularVelocity(glm::vec3(0.f));
    return true;
}

void SoftTissueDemoDriver::Start()
{
    m_elapsed = 0.f;
    m_releaseCount = 0;
}

void SoftTissueDemoDriver::Update()
{
    if (!Owner || !Owner->GetScene()) return;
    m_elapsed += std::max(Owner->GetScene()->GetDeltaTime(), 0.f);
    if (repeatSeconds > 0.f && m_elapsed >= repeatSeconds)
    {
        m_elapsed = 0.f;
        if (ReleaseProbe()) ++m_releaseCount;
    }
}
