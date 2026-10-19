#pragma once

#include "Core/Component.h"
#include <glm/glm.hpp>
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
    bool posePinPosition = false;
    bool posePinRotation = false;
    glm::vec3 posePinTarget{0.f};
    glm::vec3 posePinRotationEuler{0.f};
    glm::vec3 posePinPole{0.f};

    AnimationBone* GetParentBone() const;
    std::vector<AnimationBone*> GetChildBones() const;
};
}
