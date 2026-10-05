#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <cstdio>

int main()
{
    using namespace Engine::Components;
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Physics/fox_ragdoll.scene", nullptr))
        return 1;
    auto* fox = scene.FindObjectByName("Fox");
    auto* skeleton = fox ? fox->GetComponent<Skeleton>() : nullptr;
    auto* collider = fox ? fox->GetComponent<MeshObjectCollider>() : nullptr;
    auto* body = fox ? fox->GetComponent<RigidBody>() : nullptr;
    if (!skeleton || !skeleton->UsesWholeMeshCollider() ||
        !collider || skeleton->ResolveMeshCollider() != collider ||
        !body || body->bodyType != "Kinematic" ||
        !collider->meshReference.IsAssigned())
        return 2;
    auto* probe = scene.FindObjectByName("Fox Mesh Contact Probe");
    auto* probeCollider = probe
        ? probe->GetComponent<PrimitiveObjectCollider>() : nullptr;
    auto* probeBody = probe ? probe->GetComponent<RigidBody>() : nullptr;
    if (!probeCollider || !probeBody || probeBody->bodyType != "Dynamic")
        return 2;
    scene.Start();
    glm::vec3 minimum, maximum;
    glm::vec3 initialMinimum, initialMaximum;
    unsigned activeBoneBodies = 0;
    bool touchedFox = false;
    bool poseChanged = false;
    for (int frame = 0; frame < 180; ++frame)
    {
        scene.Update(1.f / 60.f);
        touchedFox = touchedFox || body->IsColliding();
        if (!body->GetWorldCollisionBounds(minimum, maximum))
            return 3;
        if (frame == 0)
        {
            initialMinimum = minimum;
            initialMaximum = maximum;
        }
        else if (glm::length(minimum - initialMinimum) > 0.01f ||
            glm::length(maximum - initialMaximum) > 0.01f)
            poseChanged = true;
    }
    for (const auto& object : scene.GetObjects())
        if (auto* bone = object->GetComponent<IKBone>();
            bone && bone->IsSimulating())
            ++activeBoneBodies;
    const bool hasBounds = body->GetWorldCollisionBounds(minimum, maximum);
    std::fprintf(stderr,
        "Fox whole mesh: bounds=%d min=(%.2f, %.2f, %.2f) max=(%.2f, %.2f, %.2f) active bones=%u contact=%d poseChanged=%d probeY=%.2f\n",
        hasBounds, minimum.x, minimum.y, minimum.z,
        maximum.x, maximum.y, maximum.z, activeBoneBodies,
        touchedFox, poseChanged, probe->transform.GetWorldPosition().y);
    return hasBounds && activeBoneBodies == 0 &&
        touchedFox && poseChanged &&
        std::isfinite(minimum.x) && std::isfinite(maximum.x) &&
        maximum.y > minimum.y ? 0 : 3;
}
