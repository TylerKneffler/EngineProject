#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <algorithm>
#include <cmath>
#include <glm/trigonometric.hpp>

namespace Engine::Scene
{
namespace
{
glm::quat WorldRotation(const Engine::Core::Object& object)
{
    glm::mat3 basis(object.transform.GetWorldMatrix());
    for (int column = 0; column < 3; ++column)
    {
        const float length = glm::length(basis[column]);
        if (length > 0.0001f)
            basis[column] /= length;
    }
    return glm::normalize(glm::quat_cast(basis));
}

struct FocusBounds
{
    glm::vec3 minimum { 0.f };
    glm::vec3 maximum { 0.f };
    bool hasPoints = false;

    void Include(const glm::vec3& point)
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z))
            return;
        if (!hasPoints)
        {
            minimum = maximum = point;
            hasPoints = true;
            return;
        }
        minimum = glm::min(minimum, point);
        maximum = glm::max(maximum, point);
    }
};

void IncludeFocusBounds(const Engine::Core::Object& object,
    FocusBounds& meshes, FocusBounds& origins)
{
    const glm::mat4 world = object.transform.GetWorldMatrix();
    origins.Include(glm::vec3(world[3]));
    if (const auto* mesh = object.GetComponent<Engine::Components::Mesh>();
        mesh && mesh->HasBounds())
    {
        const glm::vec3 low = mesh->GetBoundsMin();
        const glm::vec3 high = mesh->GetBoundsMax();
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z)
                    meshes.Include(glm::vec3(world * glm::vec4(
                        x ? high.x : low.x, y ? high.y : low.y,
                        z ? high.z : low.z, 1.f)));
    }
    for (const Engine::Core::Object* child : object.Children)
        if (child)
            IncludeFocusBounds(*child, meshes, origins);
}
}

Scene::Camera* Scene::FindGameCamera()
{
    for (const auto& obj : m_objects)
        if (Camera* cam = obj->GetComponent<Camera>())
            if (obj->IsEnabledInHierarchy() && cam->active)
                return cam;
    return nullptr;
}

void Scene::FocusEditorCamera(Object* obj)
{
    m_editorCameraFollowTarget = obj;
    Camera* cam = editorCamera.GetComponent<Camera>();
    if (!cam)
        return;

    const glm::vec3 objectPos =
        obj ? obj->transform.GetWorldPosition() : glm::vec3(0.f);
    FocusBounds meshes, origins;
    if (obj)
        IncludeFocusBounds(*obj, meshes, origins);
    const FocusBounds& bounds = meshes.hasPoints ? meshes : origins;
    const glm::vec3 targetPos = bounds.hasPoints
        ? (bounds.minimum + bounds.maximum) * 0.5f : objectPos;
    m_editorCameraFollowPosition = objectPos;
    if (obj)
        m_editorCameraFollowRotation = WorldRotation(*obj);
    if (m_editorMode2D)
    {
        editorCamera.transform.position.x = targetPos.x;
        editorCamera.transform.position.y = targetPos.y;
        editorCamera.transform.position.z = -10.f;
        cam->target = { targetPos.x, targetPos.y, 0.f };
        return;
    }
    const float radius = meshes.hasPoints
        ? glm::length((bounds.maximum - bounds.minimum) * 0.5f)
        : std::max(glm::length((bounds.maximum - bounds.minimum) * 0.5f), 0.5f);
    const float halfFov = glm::radians(std::clamp(cam->fov, 1.f, 179.f)) * 0.5f;
    const float distance = std::max(
        radius * 1.2f / std::sin(halfFov), cam->nearPlane * 4.f);

    const glm::vec3& eye = editorCamera.transform.position;
    const glm::vec3 oldTarget = cam->target;
    glm::vec3 oldDir = eye - oldTarget;
    float oldLen = glm::length(oldDir);
    if (oldLen < 0.001f)
    {
        oldDir = glm::vec3(0.f, 1.5f, -1.f);
        oldLen = glm::length(oldDir);
    }

    editorCamera.transform.position = targetPos + oldDir * (distance / oldLen);
    cam->target = targetPos;
}

void Scene::UpdateEditorCameraFollow()
{
    if (!m_editorCameraFollowTarget)
        return;

    const glm::vec3 current = m_editorCameraFollowTarget->transform.GetWorldPosition();
    const glm::quat rotation = WorldRotation(*m_editorCameraFollowTarget);
    const glm::quat change = rotation * glm::inverse(m_editorCameraFollowRotation);
    editorCamera.transform.position = current +
        change * (editorCamera.transform.position - m_editorCameraFollowPosition);
    if (Camera* cam = editorCamera.GetComponent<Camera>())
    {
        cam->target = current + change * (cam->target - m_editorCameraFollowPosition);
        cam->up = change * cam->up;
    }
    m_editorCameraFollowPosition = current;
    m_editorCameraFollowRotation = rotation;
}

void Scene::StopEditorCameraFollow()
{
    m_editorCameraFollowTarget = nullptr;
}

}
