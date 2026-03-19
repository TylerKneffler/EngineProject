#pragma once
#include "component.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Transform : public Component
{
public:
    Transform() = default;
    ~Transform() = default;

    glm::vec3 position { 0.f, 0.f, 0.f };
    glm::vec3 rotation { 0.f, 0.f, 0.f }; // Euler angles in radians (XYZ)
    glm::vec3 scale    { 1.f, 1.f, 1.f };

    // Returns the world matrix: T * Rz * Ry * Rx * S
    glm::mat4 GetWorldMatrix() const;
    
    glm::vec3 GetWorldPosition();
    glm::vec3 GetLocalPosition();
};