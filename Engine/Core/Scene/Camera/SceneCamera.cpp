#include "Core/Scene/Scene.h"
#include "Core/Components/Camera/Camera.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <glm/trigonometric.hpp>

namespace Engine::Scene
{
void Scene::Init(Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    // Components create their own GPU resources through this provider. The
    // scene's renderer (Engine::Rendering::RenderSystem) is created on first
    // use and reads it from here.
    if (!graphicsProvider)
        throw std::runtime_error("Scene::Init requires a non-null graphics provider");
    m_graphicsProvider = graphicsProvider;
    QueueGraphicsPreparation();

    // Set up the default editor camera
    Engine::Components::Camera* editorCameraComponent = editorCamera.AddComponent<Engine::Components::Camera>();
    editorCameraComponent->useTransformRotation = false;
    editorCameraComponent->farPlane = 1000.f;
    SetMode2D(
        settings.dimension == Engine::Model::SceneDimension::TwoD);
}

void Scene::SetMode2D(bool enabled)
{
    settings.dimension = enabled
        ? Engine::Model::SceneDimension::TwoD
        : Engine::Model::SceneDimension::ThreeD;
    if (m_fallbackCameraConfigured && m_mode2D == enabled)
        return;
    m_mode2D = enabled;
    Engine::Components::Camera* camera = editorCamera.GetComponent<Engine::Components::Camera>();
    if (!camera)
        return;
    m_fallbackCameraConfigured = true;
    camera->useTransformRotation = false;
    camera->orthographic = enabled;
    camera->target = { 0.f, 0.f, 0.f };
    camera->up = { 0.f, 1.f, 0.f };
    editorCamera.transform.position = enabled
        ? glm::vec3(0.f, 0.f, -10.f)
        : glm::vec3(0.f, 1.5f, 3.f);
}

Scene::Camera* Scene::FindGameCamera()
{
    for (const auto& obj : m_objects)
        if (Camera* cam = obj->GetComponent<Camera>())
            if (obj->IsEnabledInHierarchy() && cam->active)
                return cam;
    return nullptr;
}

}
