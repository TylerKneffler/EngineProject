#include "Camera.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <cassert>
#include <algorithm>
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

glm::dmat4 Camera::GetViewMatrixD() const
{
    assert(Owner && "Camera requires an owner Object with a Transform");
    // Spatial warp mappings currently expose float matrices. Preserve their
    // established behavior; ordinary world cameras keep double translation.
    if (Owner->transform.matrixLayer.enabled)
        return glm::dmat4(GetViewMatrix());
    const glm::dmat4 world = Owner->transform.GetWorldMatrixD();
    const glm::dvec3 eye(world[3]);
    if (!useTransformRotation)
        return glm::lookAtLH(eye, glm::dvec3(target), glm::dvec3(up));
    return glm::lookAtLH(eye, eye + glm::normalize(glm::dvec3(world[2])),
        glm::normalize(glm::dvec3(world[1])));
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

glm::dmat4 Camera::GetProjectionMatrixD(double aspect,
    bool forceOrthographic) const
{
    if (orthographic || forceOrthographic)
    {
        const double halfHeight = std::max(static_cast<double>(orthographicSize), .01);
        const double halfWidth = halfHeight * std::max(aspect, .01);
        return glm::orthoLH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight,
            static_cast<double>(nearPlane), static_cast<double>(farPlane));
    }
    return glm::perspectiveLH_ZO(glm::radians(static_cast<double>(fov)), aspect,
        static_cast<double>(nearPlane), static_cast<double>(farPlane));
}
}
