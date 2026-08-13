#include "Transform.h"
#include "Core/Object.h"
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Components
{
Transform::Transform()
{
    SetTypeName(COMPONENT_TYPE_NAME(Transform));
    RegisterField("position", position);
    RegisterField("rotation", rotation);
    RegisterField("scale", scale);
}

Transform::Transform(const Transform& other)
    : Transform()
{
    position = other.position;
    rotation = other.rotation;
    scale = other.scale;
}

Transform& Transform::operator=(const Transform& other)
{
    if (this == &other)
        return *this;

    // Keep this instance's registered field callbacks and owner. They must
    // continue to reference this Transform rather than the copied source.
    position = other.position;
    rotation = other.rotation;
    scale = other.scale;
    return *this;
}

namespace
{
Engine::Serialization::JsonValue JVec3(const glm::vec3& v)
{
    return Engine::Serialization::JsonValue::MakeArray()
        .Push(Engine::Serialization::JsonValue(v.x))
        .Push(Engine::Serialization::JsonValue(v.y))
        .Push(Engine::Serialization::JsonValue(v.z));
}

glm::vec3 Vec3From(const Engine::Serialization::JsonValue& v, const glm::vec3& def)
{
    if (!v.IsArray() || v.ArraySize() < 3)
        return def;
    return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
}
}

glm::vec3 Transform::GetLocalPosition()
{
    return position;
}

glm::vec3 Transform::GetWorldPosition()
{
    return glm::vec3(GetWorldMatrix()[3]);
}

glm::mat4 Transform::GetWorldMatrix() const
{
    glm::mat4 t  = glm::translate(glm::mat4(1.f), position);
    glm::mat4 rx = glm::rotate(glm::mat4(1.f), rotation.x, { 1.f, 0.f, 0.f });
    glm::mat4 ry = glm::rotate(glm::mat4(1.f), rotation.y, { 0.f, 1.f, 0.f });
    glm::mat4 rz = glm::rotate(glm::mat4(1.f), rotation.z, { 0.f, 0.f, 1.f });
    glm::mat4 s  = glm::scale(glm::mat4(1.f), scale);
    glm::mat4 local = t * rz * ry * rx * s;

    if (Owner && Owner->Parent)
        return Owner->Parent->transform.GetWorldMatrix() * local;

    return local;
}
}
