#pragma once

#include "Core/Components/Animation/Skeleton.h"
#include "Core/Components/Animation/SkinnedMesh.h"
#include "Core/Scene/Object.h"

namespace Engine::Editor::AnimationRigSelection
{
inline Engine::Components::Skeleton* ExplicitRig(
    Engine::Core::Object* selected)
{
    if (!selected) return nullptr;
    if (auto* skin = selected->GetComponent<
            Engine::Components::SkinnedMesh>())
        if (auto* skeleton = skin->ResolveSkeleton()) return skeleton;
    Engine::Components::Skeleton* rig = nullptr;
    for (auto* component : selected->Components)
        if (auto* skeleton = dynamic_cast<
                Engine::Components::Skeleton*>(component))
        {
            if (rig) return nullptr;
            rig = skeleton;
        }
    return rig;
}
}
