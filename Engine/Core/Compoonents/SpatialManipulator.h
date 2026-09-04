#pragma once
#include "Core/Component.h"
#include "Core/ComponentReference.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Transform.h"
#include "Core/PropertyMacros.h"
#include <array>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine::Components
{
class SpatialManipulator : public Engine::Core::Component
{
public:
    enum class ConnectionMode : int
    {
        None = 0,
        MatrixOverlay = 1,
        Portal = 2,
        LinkedPortal = 3
    };

    enum class WarpVolumeShape : int
    {
        Infinite = 0,
        Box = 1,
        Sphere = 2
    };

    enum class SpaceWarpType : int
    {
        Affine = 0,
        Spiral = 1,
        Formula = 2
    };

    SpatialManipulator();
    ~SpatialManipulator() = default;

    void Disabled() override;
    void OnDestroy() override;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    bool enabled = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 position { 0.f, 0.f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 rotation { 0.f, 0.f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 scale { 1.f, 1.f, 1.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    int connectionMode = static_cast<int>(ConnectionMode::MatrixOverlay);

    // Matrix overlays affect this transform hierarchy by default. Assign a
    // Transform component to map a different explicit hierarchy instead.
    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Matrix Overlay")
    Engine::Core::ComponentReference matrixOverlayScopeRoot { "Transform" };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Matrix Overlay")
    bool matrixOverlayIncludeChildren = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Matrix Overlay")
    int matrixOverlayPriority = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    bool definesWarpVolume = false;

    // Volumes compose from low to high priority. Equal priorities use the
    // stable scene hierarchy path, never incidental update order.
    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    int warpPriority = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    int warpVolumeShape = static_cast<int>(WarpVolumeShape::Infinite);

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    glm::vec3 warpVolumeSize { 10.f, 10.f, 10.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume", ClampMin = "0.001")
    float warpVolumeRadius = 5.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume", ClampMin = "0")
    float warpBoundaryFalloff = 0.f;

    // Dynamic rigid bodies can carry the volume's transverse metric scale as
    // they traverse from local -Z to +Z. This is opt-in because a warp volume
    // normally changes spatial coordinates without changing object ownership.
    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    bool applyTraversalScale = false;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    bool persistTraversalScaleOnExit = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    int spaceWarpType = static_cast<int>(SpaceWarpType::Affine);

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    glm::vec3 spiralAxis { 0.f, 1.f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    float spiralRadiansPerUnit = 0.5f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Formula")
    std::string formulaX { "x" };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Formula")
    std::string formulaY { "y" };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Formula")
    std::string formulaZ { "z" };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Formula")
    float formulaA = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Formula")
    float formulaB = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Formula")
    float formulaC = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Formula")
    float formulaD = 0.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalPoint { 0.f, 0.f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalNormal { 0.f, 0.f, 1.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator", ClampMin = "3", ClampMax = "8")
    int portalPointCount = 4;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalShapePoint0 { -0.5f, -0.5f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalShapePoint1 { 0.5f, -0.5f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalShapePoint2 { 0.5f, 0.5f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalShapePoint3 { -0.5f, 0.5f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalShapePoint4 { 0.f, 0.f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalShapePoint5 { 0.f, 0.f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalShapePoint6 { 0.f, 0.f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    glm::vec3 portalShapePoint7 { 0.f, 0.f, 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    bool autoMatchPortalPointCount = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    bool deformMeshOnTraversal = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator", ClampMin = "0.001")
    float traversalBlendDistance = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator", Range = "0, 2")
    float deformationStrength = 1.f;

    // The solid portal rim blocks bodies which contact the aperture boundary.
    // It deliberately has no centre plane, so a fitting body can traverse.
    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Portal", ClampMin = "0.001")
    float portalEdgeHalfWidth = 0.04f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Portal", ClampMin = "0.001")
    float portalEdgeHalfDepth = 0.15f;

    // Collision cuts are retained between small body movements. A rebuild is
    // only needed after this much motion in the body's local cut plane.
    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Portal", ClampMin = "0.001")
    float portalCollisionCutUpdateDistance = 0.05f;

    // When a linked portal is removed while a mesh is split, keep both cuts
    // as independent scene objects instead of restoring the original mesh.
    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Portal")
    bool materializeSplitOnDisconnect = true;

    // A body can traverse at most one aperture in a post-physics frame.
    // Higher values win; equal values use the stable scene hierarchy path.
    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Portal")
    int portalTraversalPriority = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    Engine::Core::ComponentReference meshReference { "Mesh" };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    Engine::Core::ComponentReference traversalTriggerBodyReference { "RigidBody" };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator")
    Engine::Core::ComponentReference targetManipulator;

    glm::mat4 GetOverlayMatrix() const;
    bool ContainsWorldPoint(const glm::vec3& worldPoint) const;
    glm::vec3 MapWorldPointThroughVolume(const glm::vec3& worldPoint) const;
    glm::mat4 GetPortalWorldTransformTo(const SpatialManipulator& target) const;
    // Rendering has its own spatial chart: apertures and virtual camera rays
    // must use the same active warp mapping as ordinary render objects.
    glm::mat4 GetRenderPortalWorldTransformTo(
        const SpatialManipulator& target) const;
    // The aperture is a simple, convex, consistently-wound planar polygon.
    // Invalid input is never used for rendering or traversal.
    bool IsValidPortalAperture(float tolerance = 0.0005f) const;
    bool IsWorldPointInsidePortalAperture(const glm::vec3& worldPoint,
        float margin = 0.f) const;
    glm::mat4 GetPortalWorldFrame() const;
    glm::mat4 GetRenderPortalWorldFrame() const;
    bool HasCompatiblePortalShapeWith(const SpatialManipulator& target) const;
    std::vector<glm::vec3> GetWorldPortalShapePoints() const;
    std::vector<glm::vec3> GetRenderWorldPortalShapePoints() const;
    glm::vec3 MapWorldPointThroughPortalShape(const glm::vec3& point,
        const SpatialManipulator& target) const;
    glm::vec3 MapRenderWorldPointThroughPortalShape(const glm::vec3& point,
        const SpatialManipulator& target) const;
    void ApplyToOwner();
    void ConnectToTarget(SpatialManipulator* target);
    void Disconnect();
    bool HasTarget() const;
    SpatialManipulator* ResolveTarget() const;
    void ConnectPortalPoints(const glm::vec3& localPoint,
        const glm::vec3& localNormal,
        SpatialManipulator* other);
    void Update() override;
    // Called by Scene after Bullet has advanced and published overlap data.
    void PostPhysicsUpdate(std::unordered_set<const RigidBody*>* claimedBodies = nullptr);
    // Split bodies render as two chart instances of the untouched mesh buffer;
    // the scene applies the supplied world-space GPU clip planes per draw.
    struct TraversalRenderInstance
    {
        Engine::Core::Object* object = nullptr;
        Mesh* mesh = nullptr;
        glm::mat4 world { 1.f };
        glm::vec4 clipPlane { 0.f };
        bool remote = false;
    };
    void AppendTraversalRenderInstances(
        std::vector<TraversalRenderInstance>& output) const;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

private:
    static constexpr int kMaxPortalShapePoints = 8;
    int GetClampedPortalPointCount() const;
    glm::vec3 GetPortalShapePoint(int index) const;
    void SetPortalShapePoint(int index, const glm::vec3& value);
    std::vector<glm::vec3> GetPortalShapePoints() const;
    bool EnsurePointCountCompatibility(SpatialManipulator* target);
    RigidBody* ResolveTraversalTriggerBody() const;
    Mesh* ResolveMeshForObject(Engine::Core::Object* object) const;
    float ComputeSignedDistanceToPortalPlane(const glm::vec3& worldPoint) const;
    void ApplyTraversalMeshDeformation(SpatialManipulator* target,
        RigidBody* traversingBody, bool mapPositiveHalf);
    void MaterializeTraversalMeshSplits(SpatialManipulator* target);
    void ResetTraversalMeshDeformation(RigidBody* traversingBody);
    void ResetTraversalMeshDeformation();
    void UpdateTriggerTraversal(SpatialManipulator* target,
        std::unordered_set<const RigidBody*>* claimedBodies);
    void ClearSpatialWarpState(SpatialManipulator* target = nullptr);
    SpatialManipulator* FindReciprocalManipulator() const;
    void ApplyPortalConnection(SpatialManipulator* target);
    void ApplyMatrixConnection(SpatialManipulator* target);
    void ClearOwnedMatrixOverlayState();
    std::vector<Engine::Core::Object*> ResolveMatrixOverlayScope() const;
    bool IsMatrixOverlayAuthority(const SpatialManipulator* target) const;
    std::string GetStableSceneKey() const;

    struct TraversalState
    {
        enum class Phase
        {
            Uninitialized,
            ArmedNegative,
            ArmedPositive,
            Cooldown
        };

        const Mesh* lastMesh = nullptr;
        std::vector<Mesh::Vertex> baseVertices;
        std::vector<Mesh::Vertex> localMeshVertices;
        std::vector<Mesh::Vertex> remoteMeshVertices;
        std::vector<glm::vec3> localCollisionVertices;
        glm::mat3 remoteLinearTransform { 1.f };
        glm::mat4 remoteRenderWorldTransform { 1.f };
        glm::mat4 collisionRemoteWorldTransform { 1.f };
        glm::vec4 localRenderClipPlane { 0.f };
        glm::vec4 remoteRenderClipPlane { 0.f };
        glm::vec3 lastCollisionPlanePoint { 0.f };
        glm::vec3 lastCollisionPlaneNormal { 0.f, 0.f, 1.f };
        bool meshDeformed = false;
        bool hasCollisionCut = false;
        bool mapPositiveHalf = false;
        // After the physical body anchor has been mapped to the destination,
        // retain the source/remote render pair until its trailing geometry has
        // cleared the aperture. The owner transform is then in target space,
        // so AppendTraversalRenderInstances derives the source chart using
        // the inverse portal transform.
        bool postTeleportVisual = false;
        Phase phase = Phase::Uninitialized;
        bool waitForOverlapExit = false;
        glm::vec3 previousWorldPosition { 0.f };
        bool hasPreviousWorldPosition = false;
    };
    std::unordered_map<const RigidBody*, TraversalState> m_traversalStates;

    struct WarpVolumeTraversalState
    {
        glm::vec3 authoredScale { 1.f };
        glm::vec3 persistedScale { 1.f };
        glm::vec3 previousLocalPosition { 0.f };
        bool hasPreviousPosition = false;
        bool active = false;
        bool enteredFromNegativeZ = false;
        bool completed = false;
    };

    void UpdateWarpVolumeTraversalScale();
    std::unordered_map<const Engine::Core::Object*, WarpVolumeTraversalState>
        m_warpVolumeTraversalStates;
    std::vector<Engine::Core::Object*> m_matrixOverlayObjects;
};
}
