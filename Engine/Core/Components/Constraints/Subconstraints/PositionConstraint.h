#pragma once
#include "Core/Components/Constraints/TransformConstraint.h"

namespace Engine::Components
{
class PositionConstraint final : public TransformConstraint
{
public:
    PositionConstraint();
protected:
    bool WantsPosition() const override { return true; }
    bool WantsRotation() const override { return false; }
};
}
