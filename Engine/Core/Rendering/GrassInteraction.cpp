#include "GrassInteraction.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
namespace Engine::Rendering
{
GrassInteractionCapsule MakeGrassInteractionCapsule(const glm::mat4& world,
    const glm::vec3& center, float radius, float height, float reach)
{
    const glm::vec3 axis(world[1]);
    const float verticalScale = glm::length(axis);
    const auto up = verticalScale > .00001f ? axis / verticalScale : glm::vec3(0, 1, 0);
    const float worldRadius = std::max(.001f, radius *
        std::max(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[2]))));
    const float halfSegment = std::max(0.f, height * verticalScale * .5f - worldRadius);
    const glm::vec3 midpoint = glm::vec3(world * glm::vec4(center, 1));
    return {glm::vec4(midpoint - up * halfSegment, worldRadius),
        glm::vec4(midpoint + up * halfSegment, std::max(.01f, reach))};
}
std::vector<GrassInteractionCapsule> CaptureGrassInteractionCapsules(const Engine::Scene::Scene& scene)
{
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    std::vector<GrassInteractionCapsule> result;
    for (const auto* collider : scene.GetComponents<Engine::Components::PrimitiveObjectCollider>())
    {
        const auto* owner = collider ? collider->Owner : nullptr;
        if (!owner || !owner->IsEnabledInHierarchy() || !collider->collisionEnabled ||
            !collider->bendGrass || collider->alignToBoneChild || collider->fitToSkinnedMesh ||
            owner->GetComponent<Engine::Components::AnimationBone>() || lower(collider->shape) != "capsule") continue;
        const Engine::Components::RigidBody* body = nullptr;
        bool boneCollider = false;
        for (auto* ancestor = owner; ancestor; ancestor = ancestor->Parent)
        {
            if (!body) body = ancestor->GetComponent<Engine::Components::RigidBody>();
            boneCollider |= ancestor->GetComponent<Engine::Components::AnimationBone>() != nullptr;
        }
        if (boneCollider) continue;
        if (body && (lower(body->bodyType) == "static" || body->isTrigger)) continue;
        result.push_back(MakeGrassInteractionCapsule(owner->transform.GetWorldMatrix(),
            collider->center, collider->radius, collider->height));
    }
    return result;
}
GrassInteractionSelection SelectGrassInteractionCapsules(
    const std::vector<GrassInteractionCapsule>& capsules, const glm::vec3& camera)
{
    GrassInteractionSelection result;
    std::array<float, 4> distances;
    distances.fill(std::numeric_limits<float>::max());
    for (const auto& capsule : capsules)
    {
        const glm::vec3 start(capsule.startRadius), end(capsule.endReach), segment = end - start;
        const float lengthSquared = glm::dot(segment, segment);
        const float t = lengthSquared > .000001f
            ? std::clamp(glm::dot(camera - start, segment) / lengthSquared, 0.f, 1.f) : 0.f;
        const auto offset = camera - (start + segment * t);
        const float distance = glm::dot(offset, offset);
        if (!std::isfinite(distance) || distance > 1600.f) continue;
        unsigned slot = 0;
        while (slot < 4 && distance >= distances[slot]) ++slot;
        if (slot == 4) continue;
        for (unsigned i = 3; i > slot; --i)
        {
            distances[i] = distances[i - 1];
            result.data[i * 2] = result.data[(i - 1) * 2];
            result.data[i * 2 + 1] = result.data[(i - 1) * 2 + 1];
        }
        distances[slot] = distance;
        result.data[slot * 2] = capsule.startRadius;
        result.data[slot * 2 + 1] = capsule.endReach;
        result.count = std::min(4u, result.count + 1);
    }
    return result;
}
}
