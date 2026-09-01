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

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    bool definesWarpVolume = false;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    int warpVolumeShape = static_cast<int>(WarpVolumeShape::Infinite);

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume")
    glm::vec3 warpVolumeSize { 10.f, 10.f, 10.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume", ClampMin = "0.001")
    float warpVolumeRadius = 5.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Spatial Manipulator | Warp Volume", ClampMin = "0")
    float warpBoundaryFalloff = 0.f;

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
    bool HasCompatiblePortalShapeWith(const SpatialManipulator& target) const;
    std::vector<glm::vec3> GetWorldPortalShapePoints() const;
    glm::vec3 MapWorldPointThroughPortalShape(const glm::vec3& point,
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
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

private:
    static constexpr int kMaxPortalShapePoints = 8;
    int GetClampedPortalPointCount() const;
    glm::vec3 GetPortalShapePoint(int index) const;
    void SetPortalShapePoint(int index, const glm::vec3& value);
    std::vector<glm::vec3> GetPortalShapePoints() const;
    glm::mat4 GetPortalWorldFrame(float& averageRadius) const;
    bool EnsurePointCountCompatibility(SpatialManipulator* target);
    RigidBody* ResolveTraversalTriggerBody() const;
    Mesh* ResolveMeshForObject(Engine::Core::Object* object) const;
    float ComputeSignedDistanceToPortalPlane(const glm::vec3& worldPoint) const;
    void ApplyTraversalMeshDeformation(SpatialManipulator* target,
        RigidBody* traversingBody);
    void ResetTraversalMeshDeformation(const RigidBody* traversingBody);
    void ResetTraversalMeshDeformation();
    void UpdateTriggerTraversal(SpatialManipulator* target);
    void ClearSpatialWarpState(SpatialManipulator* target = nullptr);
    SpatialManipulator* FindReciprocalManipulator() const;
    void ApplyPortalConnection(SpatialManipulator* target);
    void ApplyMatrixConnection(SpatialManipulator* target);

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
        bool meshDeformed = false;
        Phase phase = Phase::Uninitialized;
        bool waitForOverlapExit = false;
    };
    std::unordered_map<const RigidBody*, TraversalState> m_traversalStates;
};
}
