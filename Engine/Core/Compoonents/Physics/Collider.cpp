#include "Core/Compoonents/Physics/Collider.h"

namespace Engine::Components
{
Collider::Collider()
{
    RegisterField("collisionEnabled", collisionEnabled, "General");
    RegisterField("center", center, "General");
}

PrimitiveObjectCollider::PrimitiveObjectCollider()
{
    SetTypeName(COMPONENT_TYPE_NAME(PrimitiveObjectCollider));
    RegisterField("shape", shape, "Shape");
    RegisterField("size", size, "Shape");
    RegisterField("radius", radius, "Shape");
    RegisterField("height", height, "Shape");
}

MeshObjectCollider::MeshObjectCollider()
{
    SetTypeName(COMPONENT_TYPE_NAME(MeshObjectCollider));
    RegisterField("meshReference", meshReference, "Mesh");
    RegisterField("meshPath", meshPath, "Mesh");
    RegisterField("convex", convex, "Mesh");
}
}
