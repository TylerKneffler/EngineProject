#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Model/AnimationClip.h"
#include "Editor/Core/AnimationClipEditing.h"
#include <cmath>
#include <stdexcept>

int main()
{
    Engine::Components::AnimationBone original;
    original.poseControllerActive = true;
    original.poseControllerTarget = {1.25f, -2.f, 3.5f};
    original.posePinPosition = true;
    original.posePinRotation = true;
    original.posePinTarget = {-1.f, 4.f, .5f};
    original.posePinRotationEuler = {.2f, -.4f, .7f};
    original.posePinPole = {0.f, 1.f, 2.f};

    Engine::Components::AnimationBone restored;
    restored.Deserialize(original.Serialize());
    const auto near = [](glm::vec3 a, glm::vec3 b) {
        return glm::length(a - b) < 1e-5f;
    };
    if (!restored.poseControllerActive || !restored.posePinPosition ||
        !restored.posePinRotation ||
        !near(restored.poseControllerTarget, original.poseControllerTarget) ||
        !near(restored.posePinTarget, original.posePinTarget) ||
        !near(restored.posePinRotationEuler,
            original.posePinRotationEuler) ||
        !near(restored.posePinPole, original.posePinPole))
        throw std::runtime_error("Pose controller fields did not round trip");

    Engine::Model::AnimationClip clip;
    Engine::Model::AnimationClip::ControllerTrack track;
    track.node = 7;
    track.keys.push_back({0.f, true, true, false,
        {1.f, 2.f, 3.f}, {4.f, 5.f, 6.f}, {.1f, .2f, .3f},
        {0.f, 1.f, 0.f}});
    track.keys.push_back({1.f, false, false, true,
        {2.f, 3.f, 4.f}, {5.f, 6.f, 7.f}, {.2f, .3f, .4f},
        {1.f, 0.f, 0.f}});
    clip.controllers.push_back(track);
    Engine::Model::AnimationClip restoredClip;
    restoredClip.Deserialize(clip.Serialize());
    if (restoredClip.controllers.size() != 1 ||
        restoredClip.controllers[0].node != 7 ||
        restoredClip.controllers[0].keys.size() != 2)
        throw std::runtime_error("Controller keys did not round trip");
    const auto& keyed = restoredClip.controllers[0];
    if (Engine::Model::AnimationClip::SampleController(keyed, -.1f).active ||
        !Engine::Model::AnimationClip::SampleController(keyed, .5f).active ||
        Engine::Model::AnimationClip::SampleController(keyed, 1.f).active ||
        !Engine::Model::AnimationClip::SampleController(keyed, 1.f).
            fixedRotation)
        throw std::runtime_error("Controller state did not step at keys");
    Engine::Model::AnimationClip edited = restoredClip;
    if (!Engine::Editor::AnimationClipEditing::MoveKeys(edited,
            {{7, 1.f}}, .5f) ||
        edited.controllers[0].keys[1].time != 1.5f ||
        !Engine::Editor::AnimationClipEditing::DeleteBoneKey(edited,
            7, 1.5f) || edited.controllers[0].keys.size() != 1)
        throw std::runtime_error("Controller timeline edit failed");
}
