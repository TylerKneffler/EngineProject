#include "Camera.h"
#include "Core/Object.h"
#include <cassert>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

glm::mat4 Camera::GetViewMatrix() const
{
    assert(Owner && "Camera requires an owner Object with a Transform");
    const glm::vec3& p = Owner->transform.position;
    return glm::lookAtLH(p, target, up);
}

glm::mat4 Camera::GetProjectionMatrix(float aspect) const
{
    return glm::perspectiveLH_ZO(
        glm::radians(fov), aspect, nearPlane, farPlane);
}

// ---- Serialization ----------------------------------------------------------
namespace
{
    JsonValue JVec3(const glm::vec3& v)
    {
        return JsonValue::MakeArray().Push(JsonValue(v.x)).Push(JsonValue(v.y)).Push(JsonValue(v.z));
    }
    glm::vec3 Vec3From(const JsonValue& v, glm::vec3 def = {})
    {
        if (!v.IsArray() || v.ArraySize() < 3) return def;
        return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
    }
}

JsonValue Camera::Serialize() const
{
    JsonValue node = JsonValue::MakeObject();
    node.Set("type",  JsonValue(std::string("Camera")));
    node.Set("fov",   JsonValue(fov));
    node.Set("near",  JsonValue(nearPlane));
    node.Set("far",   JsonValue(farPlane));
    node.Set("target",JVec3(target));
    node.Set("up",    JVec3(up));
    return node;
}

void Camera::Deserialize(const JsonValue& v)
{
    if (v.Has("fov"))    fov       = v["fov"].AsFloat();
    if (v.Has("near"))   nearPlane = v["near"].AsFloat();
    if (v.Has("far"))    farPlane  = v["far"].AsFloat();
    if (v.Has("target")) target    = Vec3From(v["target"], target);
    if (v.Has("up"))     up        = Vec3From(v["up"],     up);
}
