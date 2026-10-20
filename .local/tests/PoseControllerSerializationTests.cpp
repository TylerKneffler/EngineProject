#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Object.h"
#include "Core/Model/AnimationClip.h"
#include "Core/Model/AnimationControlSpace.h"
#include "Editor/Core/AnimationClipEditing.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <stdexcept>

int main()
{
    Engine::Components::AnimationBone original;
    original.poseControllerActive = true;
    original.poseControllerTarget = {1.25f, -2.f, 3.5f};
    original.poseControllerMode = 1;
    original.poseControllerScope = 2;
    original.poseControllerChainDepth = 4;
    original.poseFacingEnabled = true;
    original.poseFacingTarget = {2.f, 3.f, 4.f};
    original.poseFacingAxis = {1.f, 0.f, 0.f};
    original.poseFacingWeight = .65f;
    original.poseRestOffsetConfigured = true;
    original.poseRestLocalOffset = {.25f, .5f, 0.f};
    original.poseControlOnly = true;
    original.posePinPosition = true;
    original.posePinRotation = true;
    original.posePinTarget = {-1.f, 4.f, .5f};
    original.posePinRotationEuler = {.2f, -.4f, .7f};
    original.posePinPole = {0.f, 1.f, 2.f};

    Engine::Components::AnimationBone restored;
    restored.Deserialize(original.Serialize());
    const auto approximatelyEqual = [](glm::vec3 a, glm::vec3 b) {
        return glm::length(a - b) < 1e-5f;
    };
    if (!restored.poseControllerActive || !restored.posePinPosition ||
        !restored.posePinRotation ||
        !approximatelyEqual(restored.poseControllerTarget, original.poseControllerTarget) ||
        !approximatelyEqual(restored.posePinTarget, original.posePinTarget) ||
        !approximatelyEqual(restored.posePinRotationEuler,
            original.posePinRotationEuler) ||
        !approximatelyEqual(restored.posePinPole, original.posePinPole) ||
        restored.poseControllerMode != 1 ||
        restored.poseControllerScope != 2 ||
        restored.poseControllerChainDepth != 4 ||
        !restored.poseFacingEnabled ||
        !approximatelyEqual(restored.poseFacingTarget,
            original.poseFacingTarget) ||
        !approximatelyEqual(restored.poseFacingAxis,
            original.poseFacingAxis) ||
        std::abs(restored.poseFacingWeight - .65f) > 1e-5f ||
        !restored.poseRestOffsetConfigured ||
        !approximatelyEqual(restored.poseRestLocalOffset,
            original.poseRestLocalOffset) ||
        !restored.poseControlOnly)
        throw std::runtime_error("Pose controller fields did not round trip");
    if (restored.EffectivePoseScope(0) != 2 ||
        restored.EffectivePoseScope(1) != 2)
        throw std::runtime_error("Controller scope override failed");
    restored.poseControllerScope = -1;
    restored.poseControllerMode = 2;
    if (restored.EffectivePoseScope(2) != 0)
        throw std::runtime_error("FK did not select one point");
    restored.poseControllerMode = 1;
    if (restored.EffectivePoseScope(0) != 1)
        throw std::runtime_error("IK did not select an ancestor chain");
    {
        Engine::Core::Object root;
        Engine::Core::Object point;
        point.Parent = &root;
        root.Children.push_back(&point);
        root.transform.scale = {2.f, 1.f, 1.f};
        point.transform.position = {1.f, 0.f, 0.f};
        auto* connection = point.AddComponent<
            Engine::Components::AnimationBone>();
        connection->CapturePoseRestOffset();
        point.transform.position = {2.f, 0.f, 0.f};
        if (std::abs(connection->PoseLengthRatio() - 2.f) > 1e-4f ||
            !connection->RestorePoseLength() ||
            glm::distance(point.transform.position,
                glm::vec3(1.f, 0.f, 0.f)) > 1e-4f)
            throw std::runtime_error("Connection rest length was not restored");
    }

    Engine::Model::AnimationClip clip;
    Engine::Model::AnimationClip::ControllerTrack track;
    track.node = 7;
    track.keys.push_back({0.f, true, true, false,
        {1.f, 2.f, 3.f}, {4.f, 5.f, 6.f}, {.1f, .2f, .3f},
        {0.f, 1.f, 0.f}});
    track.keys.push_back({1.f, false, false, true,
        {2.f, 3.f, 4.f}, {5.f, 6.f, 7.f}, {.2f, .3f, .4f},
        {1.f, 0.f, 0.f}});
    track.keys.back().space =
        Engine::Model::AnimationClip::ControllerKey::Space::Body;
    clip.controllers.push_back(track);
    Engine::Model::AnimationClip restoredClip;
    restoredClip.Deserialize(clip.Serialize());
    if (restoredClip.controllers.size() != 1 ||
        restoredClip.controllers[0].node != 7 ||
        restoredClip.controllers[0].keys.size() != 2)
        throw std::runtime_error("Controller keys did not round trip");
    const auto& keyed = restoredClip.controllers[0];
    using Space = Engine::Model::AnimationClip::ControllerKey::Space;
    if (keyed.keys[0].space != Space::World ||
        keyed.keys[1].space != Space::Body)
        throw std::runtime_error("Controller target spaces did not round trip");
    const glm::mat4 firstRoot = glm::translate(glm::mat4(1.f),
        glm::vec3(3.f, 0.f, 0.f));
    const glm::mat4 movedRoot = glm::translate(glm::mat4(1.f),
        glm::vec3(8.f, 0.f, 0.f));
    const glm::vec3 worldTarget(4.f, 2.f, 1.f);
    const glm::vec3 stored = Engine::Model::ControllerStoredPoint(
        Space::Body, firstRoot, worldTarget);
    if (!approximatelyEqual(Engine::Model::ControllerWorldPoint(Space::Body,
            firstRoot, stored), worldTarget) ||
        !approximatelyEqual(Engine::Model::ControllerWorldPoint(Space::Body,
            movedRoot, stored), worldTarget + glm::vec3(5.f, 0.f, 0.f)) ||
        !approximatelyEqual(Engine::Model::ControllerWorldPoint(Space::World,
            movedRoot, worldTarget), worldTarget))
        throw std::runtime_error("Moved root changed controller target space");
    const glm::mat4 turnedRoot = glm::rotate(glm::mat4(1.f),
        glm::half_pi<float>(), glm::vec3(0.f, 1.f, 0.f));
    const glm::vec3 storedRotation = Engine::Model::ControllerStoredRotation(
        Space::Body, turnedRoot, glm::vec3(0.f));
    const glm::quat worldRotation = Engine::Model::ControllerWorldRotation(
        Space::Body, turnedRoot, storedRotation);
    if (glm::length(worldRotation * glm::vec3(1.f, 0.f, 0.f) -
            glm::vec3(1.f, 0.f, 0.f)) > 1e-4f)
        throw std::runtime_error("Body-space controller rotation changed");
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
