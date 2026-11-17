#pragma once
#include "Core/Components/Constraints/TransformConstraint.h"

namespace Engine::Components
{
class RotationConstraint final : public TransformConstraint
{
public:
    RotationConstraint();
protected:
    bool WantsPosition() const override { return false; }
    bool WantsRotation() const override { return true; }
};
}
