#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include <cmath>

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

    const glm::vec3 targetPos =
        obj ? obj->transform.GetWorldPosition() : glm::vec3(0.f);
    m_editorCameraFollowPosition = targetPos;
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
    constexpr float kDistance = 3.f;

    const glm::vec3& eye = editorCamera.transform.position;
    const glm::vec3 oldTarget = cam->target;
    glm::vec3 oldDir = eye - oldTarget;
    float oldLen = glm::length(oldDir);
    if (oldLen < 0.001f)
    {
        oldDir = glm::vec3(0.f, 1.5f, -1.f);
        oldLen = glm::length(oldDir);
    }

    editorCamera.transform.position = targetPos + oldDir * (kDistance / oldLen);
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
