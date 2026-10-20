#pragma once

#include "Core/Component.h"
#include <glm/glm.hpp>
#include <algorithm>
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
    // Editor point-controller target. Active controllers participate as soft
    // effectors in full-body posing; fixed pins are managed separately.
    bool poseControllerActive = false;
    glm::vec3 poseControllerTarget{0.f};
    // 0 follows the viewport scope, 1 solves an IK chain, 2 uses FK.
    int poseControllerMode = 0;
    // -1 follows the viewport scope; 0/1/2 select one joint, a chain,
    // or the whole rig. FK always moves the selected joint directly.
    int poseControllerScope = -1;
    int poseControllerChainDepth = 2;
    bool poseFacingEnabled = false;
    glm::vec3 poseFacingTarget{0.f}; // World-space aim point.
    glm::vec3 poseFacingAxis{0.f, 0.f, 1.f}; // Local forward direction.
    float poseFacingWeight = 1.f;
    bool poseRestOffsetConfigured = false;
    glm::vec3 poseRestLocalOffset{0.f};
    void CapturePoseRestOffset();
    float PoseLengthRatio() const;
    bool RestorePoseLength();
    // Extra controls have no skinning palette entry. Model-bound controls
    // also have a node index so their transforms can be keyed.
    bool poseControlOnly = false;
    int EffectivePoseScope(int viewportScope) const
    {
        int scope = poseControllerScope >= 0
            ? std::clamp(poseControllerScope, 0, 2)
            : std::clamp(viewportScope, 0, 2);
        if (poseControllerMode == 2) scope = 0;
        else if (poseControllerMode == 1 && scope == 0) scope = 1;
        return scope;
    }
    bool posePinPosition = false;
    bool posePinRotation = false;
    glm::vec3 posePinTarget{0.f};
    glm::vec3 posePinRotationEuler{0.f};
    glm::vec3 posePinPole{0.f};

    AnimationBone* GetParentBone() const;
    std::vector<AnimationBone*> GetChildBones() const;
};
}
