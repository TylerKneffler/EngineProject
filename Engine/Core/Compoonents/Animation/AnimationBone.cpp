#include "AnimationBone.h"

#include "Core/Object.h"
#include <cmath>
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
    RegisterField("poseControllerActive", poseControllerActive,
        "Point Controller");
    RegisterField("poseControllerTarget", poseControllerTarget,
        "Point Controller");
    RegisterField("poseControllerMode", poseControllerMode,
        "Point Controller");
    RegisterField("poseControllerScope", poseControllerScope,
        "Point Controller");
    RegisterField("poseControllerChainDepth", poseControllerChainDepth,
        "Point Controller");
    RegisterField("poseFacingEnabled", poseFacingEnabled,
        "Point Controller");
    RegisterField("poseFacingTarget", poseFacingTarget,
        "Point Controller");
    RegisterField("poseFacingAxis", poseFacingAxis,
        "Point Controller");
    RegisterField("poseFacingWeight", poseFacingWeight,
        "Point Controller");
    RegisterField("poseRestOffsetConfigured", poseRestOffsetConfigured,
        "Point Controller");
    RegisterField("poseRestLocalOffset", poseRestLocalOffset,
        "Point Controller");
    RegisterField("poseControlOnly", poseControlOnly,
        "Point Controller");
    RegisterField("posePinPosition", posePinPosition, "Point Controller");
    RegisterField("posePinRotation", posePinRotation, "Point Controller");
    RegisterField("posePinTarget", posePinTarget, "Point Controller");
    RegisterField("posePinRotationEuler", posePinRotationEuler,
        "Point Controller");
    RegisterField("posePinPole", posePinPole, "Point Controller");
}

void AnimationBone::CapturePoseRestOffset()
{
    if (!Owner || !Owner->Parent) return;
    poseRestLocalOffset = Owner->transform.position;
    poseRestOffsetConfigured = true;
    MarkConfigurationDirty();
}

float AnimationBone::PoseLengthRatio() const
{
    if (!poseRestOffsetConfigured || !Owner || !Owner->Parent) return 1.f;
    const glm::mat3 parentBasis(Owner->Parent->transform.GetWorldMatrix());
    const float intended = glm::length(parentBasis * poseRestLocalOffset);
    const float current = glm::length(parentBasis * Owner->transform.position);
    return intended > 1e-6f ? current / intended : 1.f;
}

bool AnimationBone::RestorePoseLength()
{
    if (!poseRestOffsetConfigured || !Owner || !Owner->Parent) return false;
    const glm::mat3 parentBasis(Owner->Parent->transform.GetWorldMatrix());
    const float intended = glm::length(parentBasis * poseRestLocalOffset);
    const float current = glm::length(parentBasis * Owner->transform.position);
    if (!std::isfinite(intended) || !std::isfinite(current) ||
        intended <= 1e-6f || std::abs(current - intended) <= 1e-5f)
        return false;
    if (current > 1e-6f)
        Owner->transform.position *= intended / current;
    else Owner->transform.position = poseRestLocalOffset;
    return true;
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
