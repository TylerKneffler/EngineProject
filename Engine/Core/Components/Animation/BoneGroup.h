#pragma once

#include "Core/Component.h"

namespace Engine::Components
{
// Marks the root of an imported bone section. The object hierarchy defines
// membership, so helper objects between joints remain part of the section.
class BoneGroup final : public Engine::Core::Component
{
public:
    BoneGroup()
    {
        SetTypeName(COMPONENT_TYPE_NAME(BoneGroup));
        RegisterField("skinIndex", skinIndex, "Bone Group");
    }

    unsigned skinIndex = 0;

    static BoneGroup* ForSelection(Engine::Core::Object* selected);
    bool Contains(const Engine::Core::Object* object) const;
};
}
