#include "Core/Compoonents/Animation/ChainIK.h"
#include "Core/Object.h"
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

using Engine::Components::ChainIK;
using Engine::Core::Object;

namespace
{
struct Rig
{
    std::vector<std::unique_ptr<Object>> objects;
    Object *root, *pelvis, *spine, *shoulder, *elbow, *hand;
    Object *leftHip, *leftKnee, *leftFoot;
    Object *rightHip, *rightKnee, *rightFoot;

    Object* Add(Object* parent, glm::vec3 position)
    {
        auto object = std::make_unique<Object>();
        auto* result = object.get();
        result->Parent = parent;
        result->transform.position = position;
        if (parent) parent->Children.push_back(result);
        objects.push_back(std::move(object));
        return result;
    }

    Rig()
    {
        root = Add(nullptr, {0.f, 0.f, 0.f});
        pelvis = Add(root, {0.f, 1.f, 0.f});
        spine = Add(pelvis, {0.f, .65f, 0.f});
        shoulder = Add(spine, {.45f, .35f, 0.f});
        elbow = Add(shoulder, {.55f, 0.f, 0.f});
        hand = Add(elbow, {.55f, 0.f, 0.f});
        leftHip = Add(pelvis, {-.3f, -.15f, 0.f});
        leftKnee = Add(leftHip, {0.f, -.65f, .15f});
        leftFoot = Add(leftKnee, {0.f, -.65f, -.15f});
        rightHip = Add(pelvis, {.3f, -.15f, 0.f});
        rightKnee = Add(rightHip, {0.f, -.65f, .15f});
        rightFoot = Add(rightKnee, {0.f, -.65f, -.15f});
    }

    std::vector<Object*> Chain(Object* tip) const
    {
        std::vector<Object*> chain;
        for (auto* joint = tip; joint; joint = joint->Parent)
            chain.insert(chain.begin(), joint);
        return chain;
    }

    std::vector<ChainIK::PosePreference> Preferences() const
    {
        std::vector<ChainIK::PosePreference> result;
        for (const auto& object : objects)
        {
            int depth = 0;
            for (auto* parent = object->Parent; parent;
                parent = parent->Parent) ++depth;
            result.push_back({object.get(),
                glm::quat(object->transform.rotation),
                depth <= 2 ? .08f : .04f});
        }
        return result;
    }
};

float Distance(const Object* joint, glm::vec3 target)
{
    return glm::distance(joint->transform.GetWorldPosition(), target);
}

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    {
        Rig rig;
        const glm::vec3 start = rig.hand->transform.GetWorldPosition();
        const glm::vec3 target = start + glm::vec3(.15f, .2f, .3f);
        ChainIK::Effector reach;
        reach.chain = rig.Chain(rig.hand);
        reach.target = target;
        auto preferences = rig.Preferences();
        Check(ChainIK::SolveMultiple({reach}, 40, .001f, .2f, 45.f,
            &preferences), "Reach solve failed");
        Check(Distance(rig.hand, target) < .09f,
            "Reach did not move the hand toward its target");
        Check(glm::length(rig.shoulder->transform.rotation) > .01f ||
            glm::length(rig.elbow->transform.rotation) > .01f,
            "Reach did not involve connected joints");
    }
    {
        Rig rig;
        const glm::vec3 support =
            rig.rightFoot->transform.GetWorldPosition();
        const glm::vec3 target =
            rig.leftFoot->transform.GetWorldPosition() +
            glm::vec3(-.2f, .2f, .2f);
        ChainIK::Effector step;
        step.chain = rig.Chain(rig.leftFoot);
        step.target = target;
        ChainIK::Effector planted;
        planted.chain = rig.Chain(rig.rightFoot);
        planted.target = support;
        planted.fixed = true;
        auto preferences = rig.Preferences();
        Check(ChainIK::SolveMultiple({step, planted}, 50, .001f,
            .25f, 45.f, &preferences), "Step solve failed");
        Check(Distance(rig.leftFoot, target) < .14f,
            "Stepping foot missed its target");
        Check(Distance(rig.rightFoot, support) < .08f,
            "Planted foot drifted during a step");
    }
    {
        Rig rig;
        const glm::vec3 support =
            rig.rightFoot->transform.GetWorldPosition();
        const glm::vec3 pelvisStart =
            rig.root->transform.GetWorldPosition();
        rig.root->transform.position += glm::vec3(.2f, 0.f, .15f);
        ChainIK::Effector planted;
        planted.chain = rig.Chain(rig.rightFoot);
        planted.target = support;
        planted.fixed = true;
        auto preferences = rig.Preferences();
        Check(ChainIK::SolveMultiple({planted}, 50, .001f,
            0.f, 45.f, &preferences), "Pelvis correction failed");
        Check(Distance(rig.root, pelvisStart +
            glm::vec3(.2f, 0.f, .15f)) < 1e-4f,
            "Pelvis drag did not keep its requested position");
        Check(Distance(rig.rightFoot, support) < .08f,
            "Planted foot drifted during pelvis movement");
    }
    {
        Rig rig;
        const glm::vec3 support =
            rig.rightFoot->transform.GetWorldPosition();
        const glm::vec3 active =
            rig.leftFoot->transform.GetWorldPosition();
        const glm::vec3 target =
            rig.hand->transform.GetWorldPosition() +
            glm::vec3(.15f, .1f, .4f);
        ChainIK::Effector pull;
        pull.chain = rig.Chain(rig.hand);
        pull.target = target;
        ChainIK::Effector secondary;
        secondary.chain = rig.Chain(rig.leftFoot);
        secondary.target = active;
        secondary.positionWeight = .4f;
        ChainIK::Effector planted;
        planted.chain = rig.Chain(rig.rightFoot);
        planted.target = support;
        planted.fixed = true;
        auto preferences = rig.Preferences();
        Check(ChainIK::SolveMultiple({pull, planted, secondary},
            50, .001f, .25f, 45.f, &preferences),
            "Off-center pull solve failed");
        Check(Distance(rig.hand, target) < .16f,
            "Off-center pull did not reach");
        Check(Distance(rig.rightFoot, support) < .08f,
            "Fixed controller drifted during an off-center pull");
        Check(Distance(rig.leftFoot, active) < .15f,
            "Secondary active target drifted during an off-center pull");
    }
    {
        Rig rig;
        ChainIK::BalanceConstraint balance{{0.f, 0.f, 0.f}, .3f};
        rig.root->transform.position = {1.f, 2.f, 0.f};
        Check(ChainIK::ConstrainRootBalance(*rig.root, balance),
            "Balance did not constrain horizontal root drift");
        Check(glm::distance(rig.root->transform.position,
            glm::vec3(.3f, 2.f, 0.f)) < 1e-4f,
            "Balance changed root height or exceeded support radius");
        ChainIK::Effector reach;
        reach.chain = rig.Chain(rig.hand);
        reach.target = {100.f, 2.f, 0.f};
        auto preferences = rig.Preferences();
        Check(ChainIK::SolveMultiple({reach}, 24, .001f, .5f,
            45.f, &preferences, &balance),
            "Unreachable balance solve failed");
        const auto root = rig.root->transform.GetWorldPosition();
        Check(std::isfinite(root.x) && std::isfinite(root.y) &&
            std::isfinite(root.z) &&
            glm::length(glm::vec2(root.x, root.z)) <= .3001f,
            "Unreachable target destabilized balanced root");
    }
}
