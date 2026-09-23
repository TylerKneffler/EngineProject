#include "Core/Compoonents/Path/SplineFollower.h"

#include "Core/Compoonents/Path/SplinePath.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

SplineFollower::SplineFollower()
{
    SetTypeName(COMPONENT_TYPE_NAME(SplineFollower));
    RegisterFollowerFields();
}

void SplineFollower::RegisterFollowerFields()
{
    RegisterField("pathObjectName", pathObjectName);
    RegisterField("durationSeconds", durationSeconds);
    RegisterField("startOffset", startOffset);
    RegisterField("playOnStart", playOnStart);
    RegisterField("constantSpeed", constantSpeed);
    RegisterField("loop", loop);
    RegisterField("continuousLoop", continuousLoop);
    RegisterField("pingPong", pingPong);
    RegisterField("reverse", reverse);
    RegisterField("easing", easing);
    RegisterField("orientation", orientation);
    RegisterField("lookTargetObjectName", lookTargetObjectName);
    RegisterField("worldUp", worldUp);
}

SplinePath* SplineFollower::ResolvePath() const
{
    if (!Owner)
        return nullptr;
    Engine::Core::Object* pathObject = pathObjectName.empty()
        ? Owner : Owner->FindObjectInSceneByName(pathObjectName);
    return pathObject ? pathObject->GetComponent<SplinePath>() : nullptr;
}

float SplineFollower::ApplyEasing(float value) const
{
    value = std::clamp(value, 0.f, 1.f);
    if (easing == static_cast<int>(Easing::SmoothStep))
        return value * value * (3.f - 2.f * value);
    if (easing == static_cast<int>(Easing::SmootherStep))
        return value * value * value *
            (value * (value * 6.f - 15.f) + 10.f);
    return value;
}

float SplineFollower::GetNormalizedTime() const
{
    return std::clamp(m_time / std::max(0.001f, durationSeconds), 0.f, 1.f);
}

void SplineFollower::Start()
{
    Restart();
    if (!playOnStart)
        m_playing = false;
}

void SplineFollower::Play()
{
    if (m_finished)
        Restart();
    m_playing = true;
}

void SplineFollower::Pause()
{
    m_playing = false;
}

void SplineFollower::Restart()
{
    const float duration = std::max(0.001f, durationSeconds);
    m_direction = reverse ? -1.f : 1.f;
    const float offset = std::clamp(startOffset, 0.f, 1.f);
    m_time = (reverse ? 1.f - offset : offset) * duration;
    m_finished = false;
    m_playing = true;
    ApplyCurrentPose();
}

void SplineFollower::Stop()
{
    m_playing = false;
    m_finished = true;
}

void SplineFollower::Update()
{
    if (!m_playing || !Owner || !ResolvePath())
        return;
    const float duration = std::max(0.001f, durationSeconds);
    const float deltaTime = Owner->GetScene()
        ? std::max(0.f, Owner->GetScene()->GetDeltaTime()) : 0.f;
    m_time += deltaTime * m_direction;
    if (loop)
    {
        if (pingPong)
        {
            while (m_time > duration || m_time < 0.f)
            {
                if (m_time > duration)
                {
                    m_time = duration - (m_time - duration);
                    m_direction = -1.f;
                }
                else
                {
                    m_time = -m_time;
                    m_direction = 1.f;
                }
            }
        }
        else
        {
            m_time = std::fmod(m_time, duration);
            if (m_time < 0.f)
                m_time += duration;
        }
    }
    else if (m_time >= duration || m_time <= 0.f)
    {
        m_time = std::clamp(m_time, 0.f, duration);
        m_playing = false;
        m_finished = true;
    }
    ApplyCurrentPose();
}

void SplineFollower::SetWorldPosition(const glm::vec3& position)
{
    if (!Owner)
        return;
    Owner->transform.position = Owner->Parent
        ? glm::vec3(glm::inverse(Owner->Parent->transform.GetWorldMatrix()) *
            glm::vec4(position, 1.f))
        : position;
    Owner->transform.MarkDirty();
}

void SplineFollower::SetWorldForward(const glm::vec3& requestedForward)
{
    if (!Owner || glm::dot(requestedForward, requestedForward) < 0.000001f)
        return;
    const glm::vec3 forward = glm::normalize(requestedForward);
    glm::vec3 up = glm::dot(worldUp, worldUp) > 0.000001f
        ? glm::normalize(worldUp) : glm::vec3(0.f, 1.f, 0.f);
    glm::vec3 right = glm::cross(up, forward);
    if (glm::dot(right, right) < 0.000001f)
    {
        up = std::abs(forward.y) < 0.99f
            ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
        right = glm::cross(up, forward);
    }
    right = glm::normalize(right);
    up = glm::normalize(glm::cross(forward, right));
    glm::mat3 basis(1.f);
    basis[0] = right;
    basis[1] = up;
    basis[2] = forward;
    glm::quat rotation = glm::normalize(glm::quat_cast(basis));
    if (Owner->Parent)
    {
        const glm::mat3 parentBasis(Owner->Parent->transform.GetWorldMatrix());
        const glm::quat parentRotation = glm::normalize(glm::quat_cast(glm::mat3(
            glm::normalize(parentBasis[0]), glm::normalize(parentBasis[1]),
            glm::normalize(parentBasis[2]))));
        rotation = glm::normalize(glm::inverse(parentRotation) * rotation);
    }
    Owner->transform.rotation = glm::eulerAngles(rotation);
    Owner->transform.MarkDirty();
}

void SplineFollower::ApplyCurrentPose()
{
    SplinePath* path = ResolvePath();
    if (!Owner || !path || path->GetControlPointCount() == 0u)
        return;
    const bool seamless = loop && !pingPong && continuousLoop;
    const float normalized = seamless
        ? GetNormalizedTime() : ApplyEasing(GetNormalizedTime());
    const glm::vec3 position = path->EvaluatePosition(
        normalized, constantSpeed, seamless);
    SetWorldPosition(position);
    if (orientation == static_cast<int>(Orientation::FollowPath))
    {
        glm::vec3 tangent = path->EvaluateTangent(
            normalized, constantSpeed, seamless);
        if (m_direction < 0.f)
            tangent = -tangent;
        SetWorldForward(tangent);
    }
    else if (orientation == static_cast<int>(Orientation::LookAtTarget))
    {
        Engine::Core::Object* target = lookTargetObjectName.empty()
            ? nullptr : Owner->FindObjectInSceneByName(lookTargetObjectName);
        if (target)
            SetWorldForward(target->transform.GetWorldPosition() - position);
    }
}

bool SplineFollower::DrawProperties(Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    char pathName[256]{};
    std::snprintf(pathName, sizeof(pathName), "%s", pathObjectName.c_str());
    if (ui.InputText("Path Object", pathName, sizeof(pathName)))
    {
        pathObjectName = pathName;
        changed = true;
    }
    changed = ui.DragFloat("Duration (Seconds)", &durationSeconds,
        0.1f, 0.001f, 86400.f) || changed;
    changed = ui.SliderFloat("Start Offset", &startOffset, 0.f, 1.f) || changed;
    changed = ui.Checkbox("Play On Start", &playOnStart) || changed;
    changed = ui.Checkbox("Constant Speed", &constantSpeed) || changed;
    changed = ui.Checkbox("Loop", &loop) || changed;
    if (loop)
    {
        changed = ui.Checkbox("Continuous Loop", &continuousLoop) || changed;
        changed = ui.Checkbox("Ping Pong", &pingPong) || changed;
    }
    changed = ui.Checkbox("Reverse", &reverse) || changed;
    static const char* easingNames[] = {
        "Linear", "Smooth Step", "Smoother Step" };
    changed = ui.Combo("Easing", &easing, easingNames, 3) || changed;
    if (loop && continuousLoop && !pingPong &&
        easing != static_cast<int>(Easing::Linear))
        ui.DisabledLabel("Continuous loops use linear cycle time at the seam.");
    static const char* orientationNames[] = {
        "Keep Rotation", "Follow Path", "Look At Target" };
    changed = ui.Combo("Orientation", &orientation,
        orientationNames, 3) || changed;
    if (orientation == static_cast<int>(Orientation::LookAtTarget))
    {
        char targetName[256]{};
        std::snprintf(targetName, sizeof(targetName), "%s",
            lookTargetObjectName.c_str());
        if (ui.InputText("Look Target", targetName, sizeof(targetName)))
        {
            lookTargetObjectName = targetName;
            changed = true;
        }
    }
    changed = ui.DragFloat3("World Up", &worldUp.x, 0.01f, -1.f, 1.f) || changed;
    ui.Separator();
    if (ui.Button(m_playing ? "Pause" : "Play"))
        m_playing ? Pause() : Play();
    ui.SameLine();
    if (ui.Button("Restart"))
        Restart();
    ui.SameLine();
    if (ui.Button("Stop"))
        Stop();
    char state[96]{};
    std::snprintf(state, sizeof(state), "%s  %.1f%%",
        m_playing ? "Playing" : (m_finished ? "Finished" : "Paused"),
        GetNormalizedTime() * 100.f);
    ui.ValueLabel("Playback", state);
    if (!ResolvePath())
        ui.DisabledLabel("Assign an object containing SplinePath.");
    return changed;
}
