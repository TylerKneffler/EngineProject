#include "Core/Compoonents/Constraints/Subconstraints/ParentConstraint.h"

namespace Engine::Components
{
ParentConstraint::ParentConstraint()
{
    SetTypeName(COMPONENT_TYPE_NAME(ParentConstraint));
    editorAddable = true;
    RegisterField("positionWeight", positionWeight, "Constraint");
    RegisterField("rotationWeight", rotationWeight, "Constraint");
    RegisterField("positionOffset", positionOffset, "Offset");
    RegisterField("rotationOffsetDegrees", rotationOffsetDegrees, "Offset");
}
}
