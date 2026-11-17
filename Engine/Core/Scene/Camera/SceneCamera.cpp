#include "Core/Scene/Scene.h"
#include "Core/Components/Camera/Camera.h"
#include <algorithm>
#include <cmath>
#include <glm/trigonometric.hpp>

namespace Engine::Scene
{
Scene::Camera* Scene::FindGameCamera()
{
    for (const auto& obj : m_objects)
        if (Camera* cam = obj->GetComponent<Camera>())
            if (obj->IsEnabledInHierarchy() && cam->active)
                return cam;
    return nullptr;
}

}
