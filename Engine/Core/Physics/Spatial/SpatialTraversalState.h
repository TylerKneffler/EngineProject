#pragma once

#include "Core/Compoonents/Obj/Mesh.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace Engine::Core
{
class Object;
}

namespace Engine::Components
{
class SpatialManipulator;
}

namespace Engine::Physics::Spatial
{
// Per-body portal crossing state. Kept in the physics layer because it tracks
// collision cuts and the temporary two-chart representation of one body.
class PortalTraversalState final
{
public:
    enum class Phase
    {
        Uninitialized,
        ArmedNegative,
        ArmedPositive,
        Cooldown
    };

    const Engine::Components::Mesh* lastMesh = nullptr;
    std::vector<Engine::Components::Mesh::Vertex> baseVertices;
    std::vector<Engine::Components::Mesh::Vertex> localMeshVertices;
    std::vector<Engine::Components::Mesh::Vertex> remoteMeshVertices;
    std::shared_ptr<Engine::Components::Mesh> remoteRenderMesh;
    std::vector<glm::vec3> localCollisionVertices;
    glm::mat3 remoteLinearTransform { 1.f };
    glm::mat4 remoteRenderWorldTransform { 1.f };
    glm::mat4 collisionRemoteWorldTransform { 1.f };
    glm::vec4 localRenderClipPlane { 0.f };
    glm::vec4 remoteRenderClipPlane { 0.f };
    const Engine::Components::SpatialManipulator* localChartPortal = nullptr;
    const Engine::Components::SpatialManipulator* remoteChartPortal = nullptr;
    glm::vec3 lastCollisionPlanePoint { 0.f };
    glm::vec3 lastCollisionPlaneNormal { 0.f, 0.f, 1.f };
    bool meshDeformed = false;
    bool hasCollisionCut = false;
    bool mapPositiveHalf = false;
    bool postTeleportVisual = false;
    bool piecewiseWarp = false;
    Phase phase = Phase::Uninitialized;
    bool waitForOverlapExit = false;
    glm::vec3 previousWorldPosition { 0.f };
    bool hasPreviousWorldPosition = false;
};

// Per-object metric state for opt-in scale persistence through a warp volume.
class WarpVolumeTraversalState final
{
public:
    glm::vec3 authoredScale { 1.f };
    glm::vec3 persistedScale { 1.f };
    glm::vec3 previousLocalPosition { 0.f };
    bool hasPreviousPosition = false;
    bool active = false;
    bool enteredFromNegativeZ = false;
    bool completed = false;
};
}
