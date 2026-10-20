#pragma once

#include "Core/Model/AnimationClip.h"
#include "Core/Object.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Model
{
inline glm::mat4 LimbSpaceMatrix(const AnimationClip::LimbKey& key,
    const glm::mat4& modelWorld, const Engine::Core::Object& upper,
    const std::vector<Engine::Core::Object*>& nodes)
{
    using Space = AnimationClip::LimbKey::Space;
    switch (key.space)
    {
    case Space::World: return glm::mat4(1.f);
    case Space::Local: return upper.transform.GetWorldMatrix();
    case Space::Prop:
        if (key.propReference.IsAssigned())
            if (auto* prop = Engine::Core::ResolveComponentReference<
                    Engine::Components::Transform>(
                    const_cast<Engine::Core::Object*>(&upper), key.propReference))
                return prop->GetWorldMatrix();
        if (key.spaceNode >= 0 &&
            static_cast<size_t>(key.spaceNode) < nodes.size() &&
            nodes[key.spaceNode])
            return nodes[key.spaceNode]->transform.GetWorldMatrix();
        return modelWorld;
    case Space::Body: default: return modelWorld;
    }
}

inline glm::quat LimbSpaceRotation(const glm::mat4& matrix)
{
    glm::mat3 basis(matrix);
    for (int axis = 0; axis < 3; ++axis)
        if (glm::length(basis[axis]) > 1e-6f)
            basis[axis] = glm::normalize(basis[axis]);
    return glm::normalize(glm::quat_cast(basis));
}

inline glm::vec3 LimbWorldPoint(const glm::mat4& space,
    const glm::vec3& point)
{
    return glm::vec3(space * glm::vec4(point, 1.f));
}

inline glm::vec3 ControllerWorldPoint(
    AnimationClip::ControllerKey::Space space,
    const glm::mat4& modelWorld, const glm::vec3& point)
{
    return space == AnimationClip::ControllerKey::Space::Body
        ? LimbWorldPoint(modelWorld, point) : point;
}

inline glm::vec3 ControllerStoredPoint(
    AnimationClip::ControllerKey::Space space,
    const glm::mat4& modelWorld, const glm::vec3& point)
{
    return space == AnimationClip::ControllerKey::Space::Body
        ? LimbWorldPoint(glm::inverse(modelWorld), point) : point;
}

inline glm::quat ControllerWorldRotation(
    AnimationClip::ControllerKey::Space space,
    const glm::mat4& modelWorld, const glm::vec3& euler)
{
    const glm::quat rotation(euler);
    return space == AnimationClip::ControllerKey::Space::Body
        ? glm::normalize(LimbSpaceRotation(modelWorld) * rotation)
        : rotation;
}

inline glm::vec3 ControllerStoredRotation(
    AnimationClip::ControllerKey::Space space,
    const glm::mat4& modelWorld, const glm::vec3& euler)
{
    return space == AnimationClip::ControllerKey::Space::Body
        ? glm::eulerAngles(glm::normalize(glm::inverse(
            LimbSpaceRotation(modelWorld)) * glm::quat(euler))) : euler;
}
}
