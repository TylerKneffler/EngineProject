#include "Camera.h"
#include "Core/Object.h"
#include <cassert>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

Camera::Camera()
{
    SetTypeName(COMPONENT_TYPE_NAME(Camera));
    RegisterField("active", active);
    RegisterField("fov", fov);
    RegisterField("near", nearPlane);
    RegisterField("far", farPlane);
    RegisterField("target", target);
    RegisterField("up", up);
}

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

