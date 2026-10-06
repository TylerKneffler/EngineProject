#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>

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
}
