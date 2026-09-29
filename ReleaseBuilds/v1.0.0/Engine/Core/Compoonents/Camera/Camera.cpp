#include "Camera.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <cassert>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace Engine::Components
{
Camera::Camera()
{
    SetTypeName(COMPONENT_TYPE_NAME(Camera));
    RegisterField("active", active, "General");
    RegisterField("useTransformRotation", useTransformRotation, "General");
    RegisterField("fov", fov, "Projection");
    RegisterField("near", nearPlane, "Projection");
    RegisterField("far", farPlane, "Projection");
    RegisterField("orthographic", orthographic, "Projection");
    RegisterField("orthographicSize", orthographicSize, "Projection");
    RegisterField("target", target, "View");
    RegisterField("up", up, "View");
}

glm::mat4 Camera::GetViewMatrix() const
{
    assert(Owner && "Camera requires an owner Object with a Transform");
    glm::mat4 world = Owner->transform.GetWorldMatrix();
    if (Owner->transform.matrixLayer.enabled)
        world = Owner->transform.matrixLayer.localToLayer * world;
    bool useSourceWarpChart = false;
    if (Owner->GetScene())
    {
        const Engine::Scene::Scene::SpatialQuerySample cameraSample =
            Owner->GetScene()->SampleSpatialPoint(glm::vec3(world[3]),
                { Engine::Scene::Scene::SpatialQueryDomain::Camera, Owner });
        useSourceWarpChart = cameraSample.affectedByWarpVolume;
        if (!useSourceWarpChart)
        {
            world = Owner->GetScene()->MapSpatialMatrix(world,
                { Engine::Scene::Scene::SpatialQueryDomain::Camera, Owner });
        }
    }
    const glm::vec3 p = glm::vec3(world[3]);
    if (!useTransformRotation)
    {
        const glm::vec3 mappedTarget = Owner->GetScene() && !useSourceWarpChart
            ? Owner->GetScene()->MapSpatialPoint(target,
                { Engine::Scene::Scene::SpatialQueryDomain::Camera }) : target;
        return glm::lookAtLH(p, mappedTarget, up);
    }

    const glm::vec3 forward = glm::normalize(glm::vec3(world[2]));
    const glm::vec3 cameraUp = glm::normalize(glm::vec3(world[1]));
    return glm::lookAtLH(p, p + forward, cameraUp);
}

glm::mat4 Camera::GetProjectionMatrix(float aspect, bool forceOrthographic) const
{
    if (orthographic || forceOrthographic)
    {
        const float halfHeight = glm::max(orthographicSize, 0.01f);
        const float halfWidth = halfHeight * glm::max(aspect, 0.01f);
        return glm::orthoLH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight,
            nearPlane, farPlane);
    }
    return glm::perspectiveLH_ZO(
        glm::radians(fov), aspect, nearPlane, farPlane);
}
}
