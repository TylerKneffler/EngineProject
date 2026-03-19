#pragma once
#include "../component.h"
#include <glm/glm.hpp>

class MaterialComponent : public Component
{
public:
    MaterialComponent() = default;
    ~MaterialComponent() = default;

    // Phong shading properties — must match the CBData layout expected by the HLSL.
    glm::vec3 diffuseColor  { 1.f, 0.5f, 0.1f }; // base/albedo colour
    glm::vec3 ambientColor  { 0.1f, 0.1f, 0.1f };
    glm::vec3 specularColor { 1.f, 1.f, 1.f };
    float     shininess     { 32.f };
};
