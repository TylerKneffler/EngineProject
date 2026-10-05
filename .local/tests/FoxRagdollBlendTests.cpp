#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Scripts/Physics/FoxRagdollBlend.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <cstdio>

int main()
{
    Engine::Serialization::RegisterComponentType<FoxRagdollBlend>(
        "FoxRagdollBlend");
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Physics/fox_ragdoll.scene", nullptr))
        return 1;
    auto* fox = scene.FindObjectByName("Fox");
    auto* controllerObject = scene.FindObjectByName(
        "Fox Ragdoll Blend Controller");
    auto* controller = controllerObject
        ? controllerObject->GetComponent<FoxRagdollBlend>() : nullptr;
    auto* skeleton = fox
        ? fox->GetComponent<Engine::Components::Skeleton>() : nullptr;
    auto* animation = fox
        ? fox->GetComponent<Engine::Components::AnimationManager>() : nullptr;
    auto bones = fox
        ? fox->GetComponentsInChildren<Engine::Components::IKBone>()
        : std::vector<Engine::Components::IKBone*>{};
    if (!controller || !skeleton || !animation || bones.size() != 23)
        return 2;

    scene.Start();
    if (skeleton->UsesWholeMeshCollider() || !animation->playing)
        return 3;
    const float startHeight = fox->transform.GetWorldPosition().y;
    float initial = -1.f, mixed = -1.f, complete = -1.f;
    float initialBone = -1.f, mixedBone = -1.f, completeBone = -1.f;
    unsigned initialBodies = 0, mixedBodies = 0, completeBodies = 0;
    bool animationPlayingDuringBlend = false;
    for (int frame = 1; frame <= 300; ++frame)
    {
        scene.Update(1.f / 60.f);
        if (frame != 60 && frame != 150 && frame != 300)
            continue;
        unsigned bodies = 0;
        for (auto* bone : bones)
            bodies += bone->IsSimulating() ? 1u : 0u;
        if (frame == 60)
        {
            initial = controller->GetInfluence();
            initialBone = bones.front()->GetInfluence();
            initialBodies = bodies;
        }
        else if (frame == 150)
        {
            mixed = controller->GetInfluence();
            mixedBone = bones.front()->GetInfluence();
            mixedBodies = bodies;
            animationPlayingDuringBlend = animation->playing;
        }
        else
        {
            complete = controller->GetInfluence();
            completeBone = bones.front()->GetInfluence();
            completeBodies = bodies;
        }
    }
    const float endHeight = fox->transform.GetWorldPosition().y;
    std::fprintf(stderr,
        "Fox blend: animation=%.2f/%u, mixed=%.2f/%u, ragdoll=%.2f/%u, rootY=%.2f -> %.2f\n",
        initial, initialBodies, mixed, mixedBodies, complete,
        completeBodies, startHeight, endHeight);
    return initial == 0.f && initialBone == 0.f && initialBodies == 0 &&
        mixed > 0.1f && mixed < 0.9f &&
        std::abs(mixedBone - mixed) < 0.001f &&
        mixedBodies == 23 && animationPlayingDuringBlend &&
        complete == 1.f && completeBone == 1.f && completeBodies == 23 &&
        !animation->playing && std::isfinite(endHeight) &&
        endHeight < startHeight - 0.25f ? 0 : 4;
}
