#pragma once
#include "Core/Components/Component.h"
#include "Core/Components/PropertyMacros.h"
#include <glm/glm.hpp>

namespace Engine::Components
{
class Camera : public Engine::Core::Component
{
public:
    Camera();
    ~Camera() = default;

    // Only active scene cameras are eligible to drive the Game view/runtime.
    PROPERTY(Inspector, EditAnywhere, Category = "Camera")
    bool active = true;

    // Scene cameras normally look along their Transform's local +Z axis.
    // The editor navigation camera disables this to retain its orbit target.
    PROPERTY(Inspector, EditAnywhere, Category = "Camera")
    bool useTransformRotation = true;

    // Projection settings
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", Range = "1.0, 179.0")
    float fov       = 60.f;    // vertical field of view in degrees
    
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", ClampMin = "0.001")
    float nearPlane = 0.01f;
    
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", ClampMin = "1.0")
    float farPlane  = 100.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection")
    bool orthographic = false;

    PROPERTY(Inspector, EditAnywhere, Category = "Camera | Projection", ClampMin = "0.01")
    float orthographicSize = 5.f;

    // View settings — position is driven by the owner's Transform component.
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | View")
    glm::vec3 target { 0.f, 0.f, 0.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Camera | View")
    glm::vec3 up     { 0.f, 1.f, 0.f };

    // Returns the view matrix using the owner Transform's world position as the eye.
    // Asserts that Owner and its Transform are valid.
    glm::mat4 GetViewMatrix() const;
    glm::dmat4 GetViewMatrixD() const;

    // Returns a left-handed projection matrix with forward depth (near 0,
    // far 1), for screen/picking math that never reaches a depth buffer.
    // aspect = viewport width / height.
    glm::mat4 GetProjectionMatrix(float aspect, bool forceOrthographic = false) const;
    glm::dmat4 GetProjectionMatrixD(double aspect,
        bool forceOrthographic = false) const;
    // The same projection with reverse-Z depth (near 1, far 0), which every
    // rendered view uses (Graphics/DepthConvention.h). x, y and w match
    // GetProjectionMatrix exactly.
    glm::mat4 GetRenderProjectionMatrix(float aspect,
        bool forceOrthographic = false) const;
    glm::dmat4 GetRenderProjectionMatrixD(double aspect,
        bool forceOrthographic = false) const;

    // World-space eye and look target, whichever way the camera aims
    // (target point or Transform rotation). Used to treat scene cameras as
    // selectable viewpoints: the editor jumps its view to one, and captures
    // place the game camera at one (`--viewpoint <object name>`).
    struct Pose
    {
        glm::dvec3 eye { 0.0 };
        glm::dvec3 target { 0.0, 0.0, 1.0 };
        glm::vec3 up { 0.f, 1.f, 0.f };
    };
    Pose GetWorldPose() const;
    // Moves the owner so the eye is at pose.eye and aims at pose.target.
    // A camera that aims by Transform rotation keeps doing so when its owner
    // has no parent (yaw/pitch, no roll); otherwise it switches to aiming at
    // the target point.
    void SetWorldPose(const Pose& pose);
};
}
