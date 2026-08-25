#include "SceneCameraController.h"

#include "Core/Compoonents/Camera.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

namespace Engine::Editor
{
void SceneCameraController::Apply(Engine::Scene::Scene& scene,
    float panDX, float panDY,
    float orbitDX, float orbitDY,
    float zoom, float dolly)
{
    if (panDX == 0.f && panDY == 0.f && orbitDX == 0.f &&
        orbitDY == 0.f && zoom == 0.f && dolly == 0.f)
    {
        return;
    }

    Engine::Components::Camera* cam =
        scene.editorCamera.GetComponent<Engine::Components::Camera>();
    assert(cam && "Scene editorCamera must have a Camera component");
    glm::vec3& pos = scene.editorCamera.transform.position;

    if (scene.IsEditorMode2D())
    {
        const float dragX = panDX + orbitDX;
        const float dragY = panDY + orbitDY;
        const float speed = glm::max(cam->orthographicSize, 0.01f) * 0.002f;
        const glm::vec3 pan(-dragX * speed, dragY * speed, 0.f);
        pos += pan;
        cam->target += pan;
        const float zoomInput = zoom + dolly * 0.01f;
        if (zoomInput != 0.f)
        {
            cam->orthographicSize = std::clamp(
                cam->orthographicSize * std::pow(0.85f, zoomInput),
                0.05f, 10000.f);
        }
        return;
    }

    glm::vec3 eye = pos;
    glm::vec3 target = cam->target;
    glm::vec3 forward = glm::normalize(target - eye);
    const glm::vec3 right = glm::normalize(glm::cross(cam->up, forward));
    const glm::vec3 realUp = glm::normalize(glm::cross(forward, right));

    if (panDX != 0.f || panDY != 0.f)
    {
        const float distance = glm::length(target - eye);
        const float speed = distance * 0.002f;
        const glm::vec3 pan = right * (-panDX * speed) + realUp * (panDY * speed);
        pos += pan;
        cam->target += pan;
        eye = pos;
        target = cam->target;
    }

    if (dolly != 0.f)
    {
        const float speed = std::max(glm::length(target - eye), 0.05f) * 0.002f;
        const glm::vec3 movement = forward * (dolly * speed);
        pos += movement;
        cam->target += movement;
        eye = pos;
        target = cam->target;
    }

    if (orbitDX != 0.f || orbitDY != 0.f)
    {
        constexpr float kSensitivity = 0.005f;
        glm::vec3 arm = eye - target;
        arm = glm::mat3(glm::rotate(glm::mat4(1.f), orbitDX * kSensitivity,
            glm::vec3(0.f, 1.f, 0.f))) * arm;
        arm = glm::mat3(glm::rotate(glm::mat4(1.f), orbitDY * kSensitivity,
            right)) * arm;
        pos = target + arm;
        eye = pos;
    }

    if (zoom != 0.f)
    {
        constexpr float kZoomFactor = 0.1f;
        constexpr float kMinDistance = 0.05f;
        const float distance = glm::length(target - eye);
        float step = zoom * distance * kZoomFactor;
        step = std::min(step, distance - kMinDistance);
        forward = glm::normalize(target - eye);
        pos += forward * step;
    }
}
}
