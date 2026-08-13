#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

namespace Engine::Components
{
// Geometry is intentionally separate from RigidBody. Multiple collider
// components on one object are combined into one compound rigid body.
class Collider : public Engine::Core::Component
{
public:
    bool collisionEnabled = true;
    glm::vec3 center { 0.f };

protected:
    Collider();
};

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
    PROPERTY(Inspector, EditAnywhere, Category = "Collider", ClampMin = "0.001")
    float height = 1.f;
};

class MeshObjectCollider final : public Collider
{
public:
    MeshObjectCollider();

    // Empty uses the Mesh component on the same object. Assigning a component
    // reference takes priority over meshPath and may target another object.
    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    ComponentReference meshReference { "Mesh" };
    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    std::string meshPath;
    // Convex shapes support dynamic bodies. Concave triangle meshes are
    // intended for Static/Kinematic rigid bodies.
    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    bool convex = true;
};
}
