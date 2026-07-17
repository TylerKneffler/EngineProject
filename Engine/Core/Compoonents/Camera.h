#pragma once
#include "Core/component.h"
#include <glm/glm.hpp>

class Camera : public Component
{
public:
    Camera() = default;
    ~Camera() = default;

    // Projection settings
    float fov       = 60.f;    // vertical field of view in degrees
    float nearPlane = 0.01f;
    float farPlane  = 100.f;

    // View settings — position is driven by the owner's Transform component.
    glm::vec3 target { 0.f, 0.f, 0.f };
    glm::vec3 up     { 0.f, 1.f, 0.f };

    // Returns the view matrix using the owner Transform's world position as the eye.
    // Asserts that Owner and its Transform are valid.
    glm::mat4 GetViewMatrix() const;

    // Returns a left-handed perspective projection matrix.
    // aspect = viewport width / height.
    glm::mat4 GetProjectionMatrix(float aspect) const;

    // Serialization
    std::string GetTypeName() const override { return "Camera"; }
    JsonValue   Serialize()   const override;
    void        Deserialize(const JsonValue& v) override;
};
