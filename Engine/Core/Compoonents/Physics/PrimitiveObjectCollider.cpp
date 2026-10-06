#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"

namespace Engine::Components
{
PrimitiveObjectCollider::PrimitiveObjectCollider()
{
    SetTypeName(COMPONENT_TYPE_NAME(PrimitiveObjectCollider));
    RegisterField("shape", shape, "Shape");
    RegisterField("size", size, "Shape");
    RegisterField("radius", radius, "Shape");
    RegisterField("height", height, "Shape");
    RegisterField("alignToBoneChild", alignToBoneChild, "Shape");
    RegisterField("childBone", childBone, "Shape");
}
}
