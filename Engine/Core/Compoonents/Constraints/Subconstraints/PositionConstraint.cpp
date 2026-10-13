#include "Core/Compoonents/Constraints/Subconstraints/PositionConstraint.h"

namespace Engine::Components
{
PositionConstraint::PositionConstraint()
{
    SetTypeName(COMPONENT_TYPE_NAME(PositionConstraint));
    editorAddable = true;
    RegisterField("positionWeight", positionWeight, "Constraint");
    RegisterField("positionOffset", positionOffset, "Offset");
}
}
