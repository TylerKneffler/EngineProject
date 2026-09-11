#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

// Reverses a moving rigid body after a portal handoff has cleared the
// destination aperture. The return trip exercises the reciprocal mapping
// instead of allowing a one-shot demo object to drift out of the scene.
class PortalTraversalRepeater final : public Engine::Core::Script
{
public:
    PortalTraversalRepeater();

    PROPERTY(Inspector, EditAnywhere, Category = "Portal Traversal Repeater")
    std::string sourcePortalObjectName = "Source Portal";

    PROPERTY(Inspector, EditAnywhere, Category = "Portal Traversal Repeater")
    std::string targetPortalObjectName = "Target Portal";

    PROPERTY(Inspector, EditAnywhere, Category = "Portal Traversal Repeater", ClampMin = "0.01")
    float reverseClearance = 3.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Portal Traversal Repeater", ClampMin = "0.01")
    float teleportDetectionDistance = 2.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Portal Traversal Repeater")
    bool reverseAngularVelocity = false;

    void Start() override;
    void Update() override;

private:
    glm::vec3 m_previousPosition { 0.f };
    std::string m_destinationPortalName;
    bool m_hasPreviousPosition = false;
    bool m_waitingForClearance = false;
};
