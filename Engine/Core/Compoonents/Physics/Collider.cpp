#include "Core/Compoonents/Physics/Collider.h"

Collider::Collider()
{
    RegisterField("collisionEnabled", collisionEnabled);
    RegisterField("center", center);
}

PrimitiveObjectCollider::PrimitiveObjectCollider()
{
    SetTypeName(COMPONENT_TYPE_NAME(PrimitiveObjectCollider));
    RegisterField("shape", shape);
    RegisterField("size", size);
    RegisterField("radius", radius);
    RegisterField("height", height);
}

MeshObjectCollider::MeshObjectCollider()
{
    SetTypeName(COMPONENT_TYPE_NAME(MeshObjectCollider));
    RegisterField("meshReference", meshReference);
    RegisterField("meshPath", meshPath);
    RegisterField("convex", convex);
}
