#include "BoneGroup.h"
#include "Core/Object.h"

namespace Engine::Components
{
BoneGroup* BoneGroup::ForSelection(Engine::Core::Object* selected)
{
    for (auto* object = selected; object; object = object->Parent)
        if (auto* group = object->GetComponent<BoneGroup>())
            return group;
    return nullptr;
}

bool BoneGroup::Contains(const Engine::Core::Object* object) const
{
    for (auto* current = object; current; current = current->Parent)
    {
        if (current == Owner) return true;
        if (current->GetComponent<BoneGroup>()) return false;
    }
    return false;
}
}
