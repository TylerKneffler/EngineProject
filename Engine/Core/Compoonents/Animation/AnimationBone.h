#pragma once

#include "Core/Component.h"
#include <vector>

namespace Engine::Components
{
// Imported identity for one skeletal joint. The Object parent/child tree is
// authoritative; paletteIndex only preserves the independent skinning-buffer
// order required by the source asset.
class AnimationBone final : public Engine::Core::Component
{
public:
    AnimationBone();

    unsigned skinIndex = 0;
    unsigned nodeIndex = 0;
    int paletteIndex = -1;
    int parentPaletteIndex = -1;
    bool hierarchyRoot = false;

    AnimationBone* GetParentBone() const;
    std::vector<AnimationBone*> GetChildBones() const;
};
}
