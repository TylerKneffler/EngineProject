#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Physics/Internal/PhysicsInternal.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cctype>

namespace Engine::Physics
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsDynamic(const Engine::Components::RigidBody& body) { return Lower(body.bodyType) == "dynamic"; }
bool IsKinematic(const Engine::Components::RigidBody& body) { return Lower(body.bodyType) == "kinematic"; }

btVector3 ToBullet(const glm::vec3& value)
{
    return { value.x, value.y, value.z };
}

glm::vec3 ToGlm(const btVector3& value)
{
    return { value.x(), value.y(), value.z() };
}

glm::vec3 WorldScale(const Engine::Core::Object& object)
{
    const glm::mat4 world = object.transform.GetWorldMatrix();
    return {
        glm::length(glm::vec3(world[0])),
        glm::length(glm::vec3(world[1])),
        glm::length(glm::vec3(world[2]))
    };
}

btTransform ObjectWorldTransform(const Engine::Core::Object& object)
{
    const glm::mat4 matrix = object.transform.GetWorldMatrix();
    glm::mat3 rotation;
    for (int column = 0; column < 3; ++column)
    {
        const glm::vec3 axis(matrix[column]);
        const float length = glm::length(axis);
        rotation[column] = length > 0.00001f ? axis / length : glm::vec3(column == 0, column == 1, column == 2);
    }
    const glm::quat quaternion = glm::normalize(glm::quat_cast(rotation));
    btTransform result;
    result.setIdentity();
    result.setOrigin(ToBullet(glm::vec3(matrix[3])));
    result.setRotation(btQuaternion(quaternion.x, quaternion.y, quaternion.z, quaternion.w));
    return result;
}

bool SameVector(const glm::vec3& first, const glm::vec3& second)
{
    return first.x == second.x && first.y == second.y && first.z == second.z;
}
}
