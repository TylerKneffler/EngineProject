#pragma once
#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Transform : public Component
{
public:
    Transform();
    Transform(const Transform& other);
    Transform& operator=(const Transform& other);
    ~Transform() = default;

    PROPERTY(Inspector, EditAnywhere, Category = "Transform")
    glm::vec3 position { 0.f, 0.f, 0.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Transform")
    glm::vec3 rotation { 0.f, 0.f, 0.f }; // Euler angles in radians (XYZ)
    
    PROPERTY(Inspector, EditAnywhere, Category = "Transform")
    glm::vec3 scale    { 1.f, 1.f, 1.f };

    // Returns the world matrix: T * Rz * Ry * Rx * S
    glm::mat4 GetWorldMatrix() const;
    
    glm::vec3 GetWorldPosition();
    glm::vec3 GetLocalPosition();
};
