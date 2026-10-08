#include "Scripts/Physics/FoxProceduralWalk.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Physics/Physics.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace
{
struct Registration
{
    Registration()
    {
        Engine::Serialization::RegisterComponentType<FoxProceduralWalk>(
            "FoxProceduralWalk");
    }
} registration;
constexpr std::array<const char*, 4> footNames {
    "b_RightHand_08", "b_LeftHand_011",
    "b_RightFoot02_022", "b_LeftFoot02_018"
};
constexpr std::array<const char*, 5> courseNames {
    "Walk Floor A", "Walk Floor B", "Low Walk Rise",
    "High Walk Rise", "Uneven Walk Rise"
};
}

FoxProceduralWalk::FoxProceduralWalk()
{
    SetTypeName(COMPONENT_TYPE_NAME(FoxProceduralWalk));
    RegisterField("foxObjectName", foxObjectName, "Fox Walk");
    RegisterField("cameraObjectName", cameraObjectName, "Fox Walk");
    RegisterField("animationClip", animationClip, "Fox Walk");
    RegisterField("forwardSpeed", forwardSpeed, "Fox Walk");
    RegisterField("strideDistance", strideDistance, "Fox Walk");
    RegisterField("cameraFollowDelayDistance", cameraFollowDelayDistance,
        "Fox Walk");
    RegisterField("bodyClearance", bodyClearance, "Fox Walk");
    RegisterField("obstacleRepeatDistance", obstacleRepeatDistance, "Fox Walk");
    RegisterField("floorTileLength", floorTileLength, "Fox Walk");
}

bool FoxProceduralWalk::ResolveCourse()
{
    if (!Owner || !m_fox) return false;
    const float tile = std::max(floorTileLength, 1.f);
    const float obstacle = std::max(obstacleRepeatDistance, 1.f);
    for (unsigned index = 0; index < m_course.size(); ++index)
    {
        auto* object = Owner->FindObjectInSceneByName(courseNames[index]);
        auto* body = object ? object->GetComponent<
            Engine::Components::RigidBody>() : nullptr;
        if (!body) return false;
        CourseBody& course = m_course[index];
        course.body = body;
        course.startPosition = object->transform.GetWorldPosition();
        course.initialProgress = glm::dot(
            course.startPosition - m_startRoot, m_forward);
        course.progress = course.initialProgress;
        course.repeat = index < 2 ? 2.f * tile : obstacle;
        course.behindMargin = index < 2 ? tile * 0.5f + 3.f : 2.f;
    }
    return true;
}

void FoxProceduralWalk::RecycleCourse()
{
    for (CourseBody& course : m_course)
    {
        if (!course.body || course.repeat <= 0.f) continue;
        const float oldProgress = course.progress;
        while (m_travel > course.progress + course.behindMargin)
            course.progress += course.repeat;
        if (course.progress == oldProgress) continue;
        course.body->SetWorldPosition(course.startPosition +
            m_forward * (course.progress - course.initialProgress));
        // Static bodies need their broadphase proxy refreshed after a move.
        course.body->MarkConfigurationDirty();
    }
}

bool FoxProceduralWalk::Resolve()
{
    m_fox = Owner ? Owner->FindObjectInSceneByName(foxObjectName) : nullptr;
    m_camera = Owner ? Owner->FindObjectInSceneByName(cameraObjectName) : nullptr;
    if (!m_fox) return false;
    for (unsigned index = 0; index < 4; ++index)
    {
        m_feet[index] = m_fox->FindObjectInChildrenByName(footNames[index]);
        if (!m_feet[index]) return false;
    }
    return true;
}

void FoxProceduralWalk::Start()
{
    m_travel = 0.f;
    m_clipDuration = m_gaitPeriod = 0.f;
    m_lastStrideDistance = std::max(strideDistance, 0.01f);
    m_heightCalibrated = false;
    if (!Resolve()) return;
    m_startRoot = m_fox->transform.GetWorldPosition();
    m_forward = glm::quat(m_fox->transform.rotation) *
        glm::vec3(0.f, 0.f, 1.f);
    m_forward.y = 0.f;
    m_forward = glm::length(m_forward) > 1e-5f
        ? glm::normalize(m_forward) : glm::vec3(0.f, 0.f, -1.f);
    ResolveCourse();
    if (m_camera) m_startCamera = m_camera->transform.GetWorldPosition();
    m_smoothedRootY = m_startRoot.y;
    if (auto* skeleton = m_fox->GetComponent<Engine::Components::Skeleton>())
    {
        skeleton->colliderMode = "MeshCollider";
        skeleton->MarkConfigurationDirty();
    }
    if (auto* body = m_fox->GetComponent<Engine::Components::RigidBody>())
    {
        body->bodyType = "Kinematic";
        body->useGravity = false;
        body->MarkConfigurationDirty();
    }
    if (auto* animation = m_fox->GetComponent<
        Engine::Components::AnimationManager>())
    {
        if (const auto* selected = animation->FindClip(animationClip))
            m_clipDuration = selected->duration;
        if (m_clipDuration > 0.f)
            animation->Play(animationClip, 0.f);
        animation->playing = true;
        animation->looping = true;
        animation->speed = m_clipDuration > 0.f
            ? std::max(forwardSpeed, 0.f) * m_clipDuration /
                std::max(strideDistance, 0.01f) : 0.f;
    }
    for (auto* bone : m_fox->GetComponentsInChildren<
        Engine::Components::IKBone>())
    {
        bone->simulate = false;
        bone->SetInfluence(0.f);
        bone->ResetSimulation();
    }
}

void FoxProceduralWalk::Update()
{
    if (!m_fox && !Resolve()) return;
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (!scene) return;
    const float dt = std::clamp(scene->GetDeltaTime(), 0.f, 0.1f);
    if (dt <= 0.f) return;
    const float distancePerCycle = std::max(strideDistance, 0.01f);
    const float speed = std::max(forwardSpeed, 0.f);
    m_gaitPeriod = speed > 0.f ? distancePerCycle / speed : 0.f;
    m_travel += speed * dt;
    if (auto* animation = m_fox->GetComponent<
        Engine::Components::AnimationManager>(); animation &&
        m_clipDuration > 0.f)
    {
        const float playbackSpeed = speed * m_clipDuration /
            distancePerCycle;
        if (std::abs(animation->speed - playbackSpeed) > 1e-5f ||
            std::abs(m_lastStrideDistance - distancePerCycle) > 1e-5f)
        {
            animation->speed = playbackSpeed;
            animation->time = std::fmod(m_travel / distancePerCycle *
                m_clipDuration, m_clipDuration);
            // AnimationManager has already updated this frame. Resample only
            // on speed changes so the pose and root distance stay aligned.
            animation->Tick(0.f);
        }
        m_lastStrideDistance = distancePerCycle;
    }
    RecycleCourse();
    glm::vec3 nextRoot = m_startRoot + m_forward * m_travel;
    nextRoot.y = m_smoothedRootY;
    m_fox->transform.position = nextRoot;
    if (m_camera)
        m_camera->transform.position = m_startCamera + m_forward *
            std::max(m_travel - std::max(cameraFollowDelayDistance, 0.f), 0.f);

    // Calibrate to the animated foot positions, then keep the body above the
    // highest support under its feet. The root remains kinematic.
    float feetY = 0.f, highestGroundY = -1000.f;
    unsigned groundCount = 0;
    for (unsigned index = 0; index < 4; ++index)
    {
        const auto* foot = m_feet[index];
        if (!foot) continue;
        const glm::vec3 position = foot->transform.GetWorldPosition();
        feetY += position.y;
        const auto hits = scene->GetPhysics().RaycastAll(
            position + glm::vec3(0.f, 2.f, 0.f),
            glm::vec3(0.f, -1.f, 0.f), 4.5f);
        for (const auto& hit : hits)
            if (hit.rigidBody && hit.object != m_fox &&
                hit.rigidBody->bodyType != "Dynamic" && hit.normal.y > 0.45f)
            {
                highestGroundY = std::max(highestGroundY, hit.point.y);
                ++groundCount;
                break;
            }
    }
    if (!m_heightCalibrated)
    {
        m_rootAboveFeet = m_fox->transform.GetWorldPosition().y - feetY / 4.f;
        m_heightCalibrated = true;
    }
    if (groundCount)
    {
        const float desired = highestGroundY + m_rootAboveFeet +
            std::max(bodyClearance, 0.f);
        const float blend = 1.f - std::exp(-10.f * dt);
        m_smoothedRootY = glm::mix(m_smoothedRootY, desired, blend);
        nextRoot.y = m_smoothedRootY;
    }
    m_fox->transform.position = nextRoot;
}
