#include "Core/Compoonents/Constraints/Subconstraints/RotationConstraint.h"

namespace Engine::Components
{
RotationConstraint::RotationConstraint()
{
    SetTypeName(COMPONENT_TYPE_NAME(RotationConstraint));
    editorAddable = true;
    RegisterField("rotationWeight", rotationWeight, "Constraint");
    RegisterField("rotationOffsetDegrees", rotationOffsetDegrees, "Offset");
}
}
