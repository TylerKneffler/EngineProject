#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera.h"
#include <cmath>

Camera* Scene::FindGameCamera()
{
    for (const auto& obj : m_objects)
        if (Camera* cam = obj->GetComponent<Camera>())
            if (obj->IsEnabledInHierarchy() && cam->active)
                return cam;
    return nullptr;
}

void Scene::FocusEditorCamera(Object* obj)
{
    Camera* cam = editorCamera.GetComponent<Camera>();
    if (!cam)
        return;

    const glm::vec3 targetPos =
        obj ? obj->transform.position : glm::vec3(0.f);
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
