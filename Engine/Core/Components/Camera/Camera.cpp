#include "Camera.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include <cassert>
#include <algorithm>
#include <cmath>
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

Camera::Pose Camera::GetWorldPose() const
{
    assert(Owner && "Camera requires an owner Object with a Transform");
    const glm::dmat4 world = Owner->transform.GetWorldMatrixD();
    Pose pose;
    pose.eye = glm::dvec3(world[3]);
    if (!useTransformRotation)
    {
        pose.target = glm::dvec3(target);
        pose.up = up;
    }
    else
    {
        const glm::dvec3 forward(world[2]);
        const double length = glm::length(forward);
        pose.target = pose.eye + (length > 1e-12 ? forward / length
            : glm::dvec3(0.0, 0.0, 1.0));
        const glm::dvec3 cameraUp(world[1]);
        const double upLength = glm::length(cameraUp);
        if (upLength > 1e-12) pose.up = glm::vec3(cameraUp / upLength);
    }
    return pose;
}

void Camera::SetWorldPose(const Pose& pose)
{
    assert(Owner && "Camera requires an owner Object with a Transform");
    Transform& transform = Owner->transform;
    glm::dvec3 localEye = pose.eye;
    if (Owner->Parent)
        localEye = glm::dvec3(glm::inverse(Owner->Parent->transform.GetWorldMatrixD()) *
            glm::dvec4(pose.eye, 1.0));
    transform.position = localEye;

    const glm::dvec3 offset = pose.target - pose.eye;
    const double distance = glm::length(offset);
    if (useTransformRotation && !Owner->Parent && distance > 1e-9)
    {
        // World = T * Rz * Ry * Rx * S; with no roll the forward (+Z) axis is
        // (cos x sin y, -sin x, cos x cos y).
        const glm::dvec3 forward = offset / distance;
        transform.rotation = glm::vec3(
            static_cast<float>(std::asin(std::clamp(-forward.y, -1.0, 1.0))),
            static_cast<float>(std::atan2(forward.x, forward.z)), 0.f);
    }
    else
    {
        useTransformRotation = false;
        up = pose.up;
    }
    target = glm::vec3(pose.target);
    transform.MarkDirty();
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

glm::dmat4 Camera::GetRenderProjectionMatrixD(double aspect,
    bool forceOrthographic) const
{
    // Swapping near and far in the ZO builders yields exactly the reversed
    // depth row (z' = w - z) without the float cancellation of converting a
    // forward matrix.
    if (orthographic || forceOrthographic)
    {
        const double halfHeight = std::max(static_cast<double>(orthographicSize), .01);
        const double halfWidth = halfHeight * std::max(aspect, .01);
        return glm::orthoLH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight,
            static_cast<double>(farPlane), static_cast<double>(nearPlane));
    }
    return glm::perspectiveLH_ZO(glm::radians(static_cast<double>(fov)), aspect,
        static_cast<double>(farPlane), static_cast<double>(nearPlane));
}

glm::mat4 Camera::GetRenderProjectionMatrix(float aspect,
    bool forceOrthographic) const
{
    return glm::mat4(GetRenderProjectionMatrixD(aspect, forceOrthographic));
}
}
