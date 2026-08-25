#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>

namespace Engine::Components
{
class Camera : public Engine::Core::Component
{
public:
    Camera();
    ~Camera() = default;

    // Only active scene cameras are eligible to drive the Game view/runtime.
    PROPERTY(Inspector, EditAnywhere, Category = "Camera")
    bool active = true;

    // Scene cameras normally look along their Transform's local +Z axis.
    // The editor navigation camera disables this to retain its orbit target.
    PROPERTY(Inspector, EditAnywhere, Category = "Camera")
    bool useTransformRotation = true;

    // Projection settings
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", Range = "1.0, 179.0")
    float fov       = 60.f;    // vertical field of view in degrees
    
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", ClampMin = "0.001")
    float nearPlane = 0.01f;
    
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", ClampMin = "1.0")
    float farPlane  = 100.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection")
    bool orthographic = false;

    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", ClampMin = "0.01")
    float orthographicSize = 5.f;

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
    glm::mat4 GetProjectionMatrix(float aspect, bool forceOrthographic = false) const;

};
}
