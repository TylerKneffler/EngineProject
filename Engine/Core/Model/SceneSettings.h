#pragma once

#include <glm/glm.hpp>
#include <string>

namespace Engine::Model
{
struct SceneSettings
{
    bool showGrid = true;
    int gridHalfSize = 10;
    float gridCellSize = 1.f;
    float gridOpacity = 0.4f;
    float gridFadeDistance = 80.f;
    glm::vec3 gridColor = glm::vec3(0.45f, 0.45f, 0.45f);
    glm::vec3 gridOriginColor = glm::vec3(0.30f, 0.50f, 0.80f);
    glm::vec3 ambientColor = glm::vec3(0.12f, 0.12f, 0.12f);
    std::string skyboxTexture;

};
}

