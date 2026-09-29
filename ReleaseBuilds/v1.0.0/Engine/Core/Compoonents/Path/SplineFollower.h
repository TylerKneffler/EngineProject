#pragma once

#include "Core/PropertyMacros.h"
#include "Core/component.h"
#include <string>

class SplinePath;

// Drives any scene object along a SplinePath. CameraTrack derives from this
// component only to preserve existing scene/component compatibility.
class SplineFollower : public Engine::Core::Component
{
public:
    enum class Easing : int { Linear = 0, SmoothStep = 1, SmootherStep = 2 };
    enum class Orientation : int
    {
        KeepRotation = 0,
        FollowPath = 1,
        LookAtTarget = 2
    };

    SplineFollower();

    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower")
    std::string pathObjectName;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower", ClampMin = "0.001")
    float durationSeconds = 10.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower", Range = "0, 1")
    float startOffset = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower")
    bool playOnStart = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower")
    bool constantSpeed = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower")
    bool loop = false;
    // Closes an open path for loop playback and keeps linear time at the wrap,
    // preventing an end-to-start jump or easing pause.
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower")
    bool continuousLoop = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower")
    bool pingPong = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower")
    bool reverse = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower")
    int easing = static_cast<int>(Easing::Linear);
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower | Orientation")
    int orientation = static_cast<int>(Orientation::FollowPath);
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower | Orientation")
    std::string lookTargetObjectName;
    PROPERTY(Inspector, EditAnywhere, Category = "Spline Follower | Orientation")
    glm::vec3 worldUp { 0.f, 1.f, 0.f };

    void Start() override;
    void Update() override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    void Play();
    void Pause();
    void Restart();
    void Stop();
    bool IsPlaying() const { return m_playing; }
    bool IsFinished() const { return m_finished; }
    float GetNormalizedTime() const;

protected:
    void RegisterFollowerFields();

private:
    SplinePath* ResolvePath() const;
    void ApplyCurrentPose();
    float ApplyEasing(float value) const;
    void SetWorldPosition(const glm::vec3& position);
    void SetWorldForward(const glm::vec3& forward);
    float m_time = 0.f;
    float m_direction = 1.f;
    bool m_playing = false;
    bool m_finished = false;
};
