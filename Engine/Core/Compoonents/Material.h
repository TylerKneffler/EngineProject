#pragma once
#include "Core/component.h"
#include <glm/glm.hpp>

class Material : public Component
{
public:
    Material() = default;
    ~Material() = default;

    // Phong shading properties — must match the CBData layout expected by the HLSL.
    glm::vec3 diffuseColor  { 1.f, 0.5f, 0.1f }; // base/albedo colour
    glm::vec3 ambientColor  { 0.1f, 0.1f, 0.1f };
    glm::vec3 specularColor { 1.f, 1.f, 1.f };
    float     shininess     { 32.f };

    // Serialization
    std::string GetTypeName() const override { return "Material"; }
    JsonValue   Serialize()   const override;
    void        Deserialize(const JsonValue& v) override;
};
