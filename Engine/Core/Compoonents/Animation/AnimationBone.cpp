#include "AnimationBone.h"

#include "Core/Object.h"
#include <functional>

namespace Engine::Components
{
AnimationBone::AnimationBone()
{
    SetTypeName(COMPONENT_TYPE_NAME(AnimationBone));
    RegisterField("skinIndex", skinIndex, "Animation Bone");
    RegisterField("nodeIndex", nodeIndex, "Animation Bone");
    RegisterField("paletteIndex", paletteIndex, "Animation Bone");
    RegisterField("parentPaletteIndex", parentPaletteIndex, "Animation Bone");
    RegisterField("hierarchyRoot", hierarchyRoot, "Animation Bone");
}

AnimationBone* AnimationBone::GetParentBone() const
{
    for (Object* parent = Owner ? Owner->Parent : nullptr;
        parent; parent = parent->Parent)
        for (Engine::Core::Component* component : parent->Components)
            if (auto* bone = dynamic_cast<AnimationBone*>(component);
                bone && bone->skinIndex == skinIndex)
                return bone;
    return nullptr;
}

std::vector<AnimationBone*> AnimationBone::GetChildBones() const
{
    std::vector<AnimationBone*> result;
    const auto visit = [this, &result](auto&& self, Object* object) -> void
    {
        if (!object)
            return;
        for (Engine::Core::Component* component : object->Components)
            if (auto* bone = dynamic_cast<AnimationBone*>(component);
                bone && bone->skinIndex == skinIndex)
            {
                result.push_back(bone);
                return;
            }
        for (Object* child : object->Children)
            self(self, child);
    };
    if (Owner)
        for (Object* child : Owner->Children)
            visit(visit, child);
    return result;
}
}
