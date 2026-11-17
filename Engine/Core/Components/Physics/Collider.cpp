#include "Core/Components/Physics/Collider.h"

namespace Engine::Components
{
Collider::Collider()
{
    RegisterField("collisionEnabled", collisionEnabled, "General");
    RegisterField("center", center, "General");
}
}
