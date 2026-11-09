#include "SkeletonBindPose.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Transform.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace Engine::Editor::SkeletonBindPose
{
bool Commit(Engine::Scene::Scene& scene,
    Engine::Components::Skeleton& skeleton, std::string& error)
{
    auto* model = skeleton.ResolveModel();
    if (!model || !model->Owner)
    { error = "A model root is required."; return false; }
    const glm::mat4 modelWorld = model->Owner->transform.GetWorldMatrix();
    if (std::abs(glm::determinant(modelWorld)) < 1e-8f)
    { error = "The model root has zero scale."; return false; }
    const auto& joints = skeleton.ResolveJoints();
    if (joints.size() != skeleton.jointNodes.size())
    { error = "The joint palette is incomplete."; return false; }
    const glm::mat4 inverseModel = glm::inverse(modelWorld);
    std::vector<glm::mat4> inverseBind;
    inverseBind.reserve(joints.size());
    for (auto* joint : joints)
    {
        if (!joint)
        { error = "The joint palette contains a missing node."; return false; }
        const glm::mat4 modelJoint = inverseModel *
            joint->transform.GetWorldMatrix();
        if (std::abs(glm::determinant(modelJoint)) < 1e-8f)
        { error = "A joint transform has zero scale."; return false; }
        inverseBind.push_back(glm::inverse(modelJoint));
    }
    skeleton.inverseBindMatrices = std::move(inverseBind);
    skeleton.MarkConfigurationDirty();
    for (const auto& object : scene.GetObjects())
        if (object)
            if (auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>();
                skin && skin->ResolveSkeleton() == &skeleton)
            {
                skin->MarkConfigurationDirty();
                skin->Start();
                skin->BuildPalette();
            }
    error.clear();
    return true;
}

bool Restore(Engine::Components::Skeleton& skeleton)
{
    auto* model = skeleton.ResolveModel();
    const auto& joints = skeleton.ResolveJoints();
    if (!model || !model->Owner ||
        joints.size() != skeleton.inverseBindMatrices.size()) return false;

    std::vector<size_t> order(joints.size());
    std::iota(order.begin(), order.end(), 0);
    const auto depth = [](const Engine::Core::Object* object)
    {
        size_t result = 0;
        for (; object; object = object->Parent) ++result;
        return result;
    };
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
        { return depth(joints[a]) < depth(joints[b]); });

    const glm::mat4 modelWorld = model->Owner->transform.GetWorldMatrix();
    bool complete = true;
    for (size_t index : order)
    {
        auto* joint = joints[index];
        const glm::mat4& inverseBind = skeleton.inverseBindMatrices[index];
        if (!joint || std::abs(glm::determinant(inverseBind)) < 1e-8f)
        { complete = false; continue; }
        const glm::mat4 world = modelWorld * glm::inverse(inverseBind);
        const glm::mat4 parentWorld = joint->Parent
            ? joint->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
        if (std::abs(glm::determinant(parentWorld)) < 1e-8f)
        { complete = false; continue; }
        const glm::mat4 local = glm::inverse(parentWorld) * world;
        glm::vec3 scale, translation, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        if (!glm::decompose(local, scale, rotation, translation, skew,
                perspective))
        { complete = false; continue; }
        joint->transform.position = translation;
        joint->transform.rotation = glm::eulerAngles(glm::normalize(rotation));
        joint->transform.scale = scale;
    }
    return complete;
}
}
