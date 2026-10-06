#pragma once

#include "Core/Compoonents/Physics/Collider.h"
#include <string>

namespace Engine::Components
{
class PrimitiveObjectCollider final : public Collider
{
public:
    PrimitiveObjectCollider();

    // Box (or Cube), Sphere (or Circle), Capsule, Cylinder.
    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    std::string shape = "Box";
    PROPERTY(Inspector, EditAnywhere, Category = "Collider", ClampMin = "0.001")
    glm::vec3 size { 1.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Collider", ClampMin = "0.001")
    float radius = 0.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Collider", ClampMin = "0")
    float height = 1.f;
    // For an AnimationBone, align a capsule or cylinder from this bone to a
    // direct child. Zero height uses the current bone-to-child distance.
    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    bool alignToBoneChild = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    std::string childBone;
};
}
