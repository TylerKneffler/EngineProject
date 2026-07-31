#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>

class Camera : public Component
{
public:
    Camera();
    ~Camera() = default;

    // Projection settings
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", Range = "1.0, 179.0")
    float fov       = 60.f;    // vertical field of view in degrees
    
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", ClampMin = "0.001")
    float nearPlane = 0.01f;
    
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", ClampMin = "1.0")
    float farPlane  = 100.f;

    // View settings — position is driven by the owner's Transform component.
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | View")
    glm::vec3 target { 0.f, 0.f, 0.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | View")
    glm::vec3 up     { 0.f, 1.f, 0.f };

    // Returns the view matrix using the owner Transform's world position as the eye.
    // Asserts that Owner and its Transform are valid.
    glm::mat4 GetViewMatrix() const;

    // Returns a left-handed perspective projection matrix.
    // aspect = viewport width / height.
    glm::mat4 GetProjectionMatrix(float aspect) const;

};
