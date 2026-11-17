#pragma once
#include "Core/Components/Constraints/TransformConstraint.h"

namespace Engine::Components
{
class ParentConstraint final : public TransformConstraint
{
public:
    ParentConstraint();
protected:
    bool WantsPosition() const override { return true; }
    bool WantsRotation() const override { return true; }
};
}
